#include "jit.h"

#include "bbc.h"
#include "debug.h"
#include "opdefs.h"

#include <assert.h>
#include <err.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const size_t k_addr_space_size = 0x10000;
static const size_t k_guard_size = 4096;
static const int k_jit_bytes_per_byte = 256;
static const int k_jit_bytes_shift = 8;

static const int k_offset_debug = 8;
static const int k_offset_debug_callback = 16;
static const int k_offset_bbc = 24;
static const int k_offset_read_callback = 32;
static const int k_offset_write_callback = 40;
static const int k_offset_interrupt = 48;

struct jit_struct {
  unsigned char* p_mem;     /* 0  */
  void* p_debug;            /* 8  */
  void* p_debug_callback;   /* 16 */
  struct bbc_struct* p_bbc; /* 24 */
  void* p_read_callback;    /* 32 */
  void* p_write_callback;   /* 40 */
  uint64_t interrupt;       /* 48 */
};

static size_t jit_emit_int(unsigned char* p_jit, size_t index, ssize_t offset) {
  p_jit[index++] = offset & 0xff;
  offset >>= 8;
  p_jit[index++] = offset & 0xff;
  offset >>= 8;
  p_jit[index++] = offset & 0xff;
  offset >>= 8;
  p_jit[index++] = offset & 0xff;

  return index;
}

static size_t jit_emit_op1_op2(unsigned char* p_jit,
                               size_t index,
                               unsigned char operand1,
                               unsigned char operand2) {
  p_jit[index++] = operand1;
  p_jit[index++] = operand2;
  p_jit[index++] = 0;
  p_jit[index++] = 0;

  return index;
}

static size_t jit_emit_do_jmp_next(unsigned char* p_jit,
                                   size_t index,
                                   size_t oplen) {
  assert(index + 2 <= k_jit_bytes_per_byte);
  size_t offset = (k_jit_bytes_per_byte * oplen) - (index + 2);
  if (offset <= 0x7f) {
    // jmp
    p_jit[index++] = 0xeb;
    p_jit[index++] = offset;
  } else {
    offset -= 3;
    p_jit[index++] = 0xe9;
    index = jit_emit_int(p_jit, index, offset);
  }

  return index;
}

static size_t jit_emit_do_relative_jump(unsigned char* p_jit,
                                        size_t index,
                                        unsigned char intel_opcode,
                                        unsigned char unsigned_jump_size) {
  char jump_size = (char) unsigned_jump_size;
  ssize_t offset = (k_jit_bytes_per_byte * (jump_size + 2)) - (index + 2);
  if (offset <= 0x7f && offset >= -0x80) {
    // Fits in a 1-byte offset.
    assert(index + 2 <= k_jit_bytes_per_byte);
    p_jit[index++] = intel_opcode;
    p_jit[index++] = (unsigned char) offset;
  } else {
    offset -= 4;
    assert(index + 6 <= k_jit_bytes_per_byte);
    p_jit[index++] = 0x0f;
    p_jit[index++] = intel_opcode + 0x10;
    index = jit_emit_int(p_jit, index, offset);
  }

  return index;
}

static size_t jit_emit_intel_to_6502_zero(unsigned char* p_jit, size_t index) {
  // sete r13b
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x94;
  p_jit[index++] = 0xc5;

  return index;
}

static size_t jit_emit_intel_to_6502_negative(unsigned char* p_jit,
                                              size_t index) {
  // sets r14b
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x98;
  p_jit[index++] = 0xc6;

  return index;
}

static size_t jit_emit_intel_to_6502_carry(unsigned char* p_jit, size_t index) {
  // setb r12b
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x92;
  p_jit[index++] = 0xc4;

  return index;
}

static size_t jit_emit_intel_to_6502_sub_carry(unsigned char* p_jit,
                                               size_t index) {
  // setae r12b
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x93;
  p_jit[index++] = 0xc4;

  return index;
}

static size_t jit_emit_intel_to_6502_overflow(unsigned char* p_jit,
                                              size_t index) {
  // seto r15b
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x90;
  p_jit[index++] = 0xc7;

  return index;
}

static size_t jit_emit_carry_to_6502_zero(unsigned char* p_jit, size_t index) {
  // setb r13b
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x92;
  p_jit[index++] = 0xc5;

  return index;
}

static size_t jit_emit_carry_to_6502_negative(unsigned char* p_jit,
                                              size_t index) {
  // setb r14b
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x92;
  p_jit[index++] = 0xc6;

  return index;
}

static size_t jit_emit_carry_to_6502_overflow(unsigned char* p_jit,
                                              size_t index) {
  // setb r15b
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x92;
  p_jit[index++] = 0xc7;

  return index;
}

static size_t jit_emit_do_zn_flags(unsigned char* p_jit,
                                   size_t index,
                                   int reg) {
  assert(index + 8 <= k_jit_bytes_per_byte);
  if (reg == -1) {
    // Nothing -- flags already set.
  } else if (reg == 0) {
    // test al, al
    p_jit[index++] = 0x84;
    p_jit[index++] = 0xc0;
  } else if (reg == 1) {
    // test bl, bl
    p_jit[index++] = 0x84;
    p_jit[index++] = 0xdb;
  } else if (reg == 2) {
    // test cl, cl
    p_jit[index++] = 0x84;
    p_jit[index++] = 0xc9;
  }

  index = jit_emit_intel_to_6502_zero(p_jit, index);
  index = jit_emit_intel_to_6502_negative(p_jit, index);

  return index;
}

static size_t jit_emit_intel_to_6502_znc(unsigned char* p_jit, size_t index) {
  index = jit_emit_intel_to_6502_zero(p_jit, index);
  index = jit_emit_intel_to_6502_negative(p_jit, index);
  index = jit_emit_intel_to_6502_carry(p_jit, index);

  return index;
}

static size_t jit_emit_intel_to_6502_sub_znc(unsigned char* p_jit,
                                             size_t index) {
  index = jit_emit_intel_to_6502_zero(p_jit, index);
  index = jit_emit_intel_to_6502_negative(p_jit, index);
  index = jit_emit_intel_to_6502_sub_carry(p_jit, index);

  return index;
}

static size_t jit_emit_intel_to_6502_znco(unsigned char* p_jit, size_t index) {
  index = jit_emit_intel_to_6502_znc(p_jit, index);
  index = jit_emit_intel_to_6502_overflow(p_jit, index);

  return index;
}

static size_t jit_emit_intel_to_6502_sub_znco(unsigned char* p_jit,
                                              size_t index) {
  index = jit_emit_intel_to_6502_sub_znc(p_jit, index);
  index = jit_emit_intel_to_6502_overflow(p_jit, index);

  return index;
}

static size_t jit_emit_6502_carry_to_intel(unsigned char* p_jit, size_t index) {
  // Note: doesn't just check carry value but also trashes it.
  // shr r12b, 1
  p_jit[index++] = 0x41;
  p_jit[index++] = 0xd0;
  p_jit[index++] = 0xec;

  return index;
}

static size_t jit_emit_set_carry(unsigned char* p_jit,
                                 size_t index,
                                 unsigned char val) {
  // mov r12b, val
  p_jit[index++] = 0x41;
  p_jit[index++] = 0xb4;
  p_jit[index++] = val;

  return index;
}

static size_t jit_emit_test_carry(unsigned char* p_jit, size_t index) {
  // test r12b, r12b
  p_jit[index++] = 0x45;
  p_jit[index++] = 0x84;
  p_jit[index++] = 0xe4;

  return index;
}

static size_t jit_emit_test_zero(unsigned char* p_jit, size_t index) {
  // test r13b, r13b
  p_jit[index++] = 0x45;
  p_jit[index++] = 0x84;
  p_jit[index++] = 0xed;

  return index;
}

static size_t jit_emit_test_negative(unsigned char* p_jit, size_t index) {
  // test r14b, r14b
  p_jit[index++] = 0x45;
  p_jit[index++] = 0x84;
  p_jit[index++] = 0xf6;

  return index;
}

static size_t jit_emit_test_overflow(unsigned char* p_jit, size_t index) {
  // test r15b, r15b
  p_jit[index++] = 0x45;
  p_jit[index++] = 0x84;
  p_jit[index++] = 0xff;

  return index;
}

static size_t jit_emit_abs_x_to_scratch(unsigned char* p_jit,
                                        size_t index,
                                        unsigned char operand1,
                                        unsigned char operand2) {
  // mov edx, ebx
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xda;
  // add dx, op1,op2
  p_jit[index++] = 0x66;
  p_jit[index++] = 0x81;
  p_jit[index++] = 0xc2;
  p_jit[index++] = operand1;
  p_jit[index++] = operand2;

  return index;
}

static size_t jit_emit_abs_y_to_scratch(unsigned char* p_jit,
                                        size_t index,
                                        unsigned char operand1,
                                        unsigned char operand2) {
  // mov edx, ecx
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xca;
  // add dx, op1,op2
  p_jit[index++] = 0x66;
  p_jit[index++] = 0x81;
  p_jit[index++] = 0xc2;
  p_jit[index++] = operand1;
  p_jit[index++] = operand2;

  return index;
}

static size_t jit_emit_ind_y_to_scratch(unsigned char* p_jit,
                                        size_t index,
                                        unsigned char operand1) {
  if (operand1 == 0xff) {
    // movzx edx, BYTE PTR [rdi + 0xff]
    p_jit[index++] = 0x0f;
    p_jit[index++] = 0xb6;
    p_jit[index++] = 0x97;
    index = jit_emit_op1_op2(p_jit, index, 0xff, 0);
    // mov dh, BYTE PTR [rdi]
    p_jit[index++] = 0x8a;
    p_jit[index++] = 0x37;
  } else {
    // movzx edx, WORD PTR [rdi + op1]
    p_jit[index++] = 0x0f;
    p_jit[index++] = 0xb7;
    p_jit[index++] = 0x97;
    index = jit_emit_op1_op2(p_jit, index, operand1, 0);
  }
  // add dx, cx
  p_jit[index++] = 0x66;
  p_jit[index++] = 0x01;
  p_jit[index++] = 0xca;

  return index;
}

static size_t jit_emit_ind_x_to_scratch(unsigned char* p_jit,
                                        size_t index,
                                        unsigned char operand1) {
  unsigned char operand1_inc = operand1 + 1;
  // mov r9, rbx
  p_jit[index++] = 0x49;
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xd9;
  // add r9b, operand1_inc
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x80;
  p_jit[index++] = 0xc1;
  p_jit[index++] = operand1_inc;
  // movzx rdx, BYTE PTR [rdi + r9]
  p_jit[index++] = 0x4a;
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0xb6;
  p_jit[index++] = 0x14;
  p_jit[index++] = 0x0f;
  // shl edx, 8
  p_jit[index++] = 0xc1;
  p_jit[index++] = 0xe2;
  p_jit[index++] = 0x08;
  // dec r9b
  p_jit[index++] = 0x41;
  p_jit[index++] = 0xfe;
  p_jit[index++] = 0xc9;
  // mov dl, BYTE PTR [rdi + r9]
  p_jit[index++] = 0x42;
  p_jit[index++] = 0x8a;
  p_jit[index++] = 0x14;
  p_jit[index++] = 0x0f;

  return index;
}

size_t jit_emit_zp_x_to_scratch(unsigned char* p_jit,
                                size_t index,
                                unsigned char operand1) {
  // mov edx, ebx
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xda;
  // add dl, op1
  p_jit[index++] = 0x80;
  p_jit[index++] = 0xc2;
  p_jit[index++] = operand1;

  return index;
}

size_t jit_emit_zp_y_to_scratch(unsigned char* p_jit,
                                size_t index,
                                unsigned char operand1) {
  // mov edx, ecx
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xca;
  // add dl, op1
  p_jit[index++] = 0x80;
  p_jit[index++] = 0xc2;
  p_jit[index++] = operand1;

  return index;
}

static size_t jit_emit_scratch_bit_test(unsigned char* p_jit,
                                        size_t index,
                                        unsigned char bit) {
  // bt edx, bit
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0xba;
  p_jit[index++] = 0xe2;
  p_jit[index++] = bit;

  return index;
}

static size_t jit_emit_jmp_scratch(unsigned char* p_jit, size_t index) {
  // jmp rdx
  p_jit[index++] = 0xff;
  p_jit[index++] = 0xe2;

  return index;
}

static size_t jit_emit_jmp_op1_op2(unsigned char* p_jit,
                                   size_t index,
                                   unsigned char operand1,
                                   unsigned char operand2) {
  // lea rdx, [rdi + k_addr_space_size + k_guard_size +
  //               op1,op2 * k_jit_bytes_per_byte]
  p_jit[index++] = 0x48;
  p_jit[index++] = 0x8d;
  p_jit[index++] = 0x97;
  index = jit_emit_int(p_jit,
                       index,
                       k_addr_space_size + k_guard_size +
                           ((operand1 + (operand2 << 8)) *
                               k_jit_bytes_per_byte));
  index = jit_emit_jmp_scratch(p_jit, index);

  return index;
}

static size_t jit_emit_jit_bytes_shift_scratch_left(unsigned char* p_jit,
                                                    size_t index) {
  // shl edx, k_jit_bytes_shift
  p_jit[index++] = 0xc1;
  p_jit[index++] = 0xe2;
  p_jit[index++] = k_jit_bytes_shift;

  return index;
}

static size_t jit_emit_jit_bytes_shift_scratch_right(unsigned char* p_jit,
                                                     size_t index) {
  // shr edx, k_jit_bytes_shift
  p_jit[index++] = 0xc1;
  p_jit[index++] = 0xea;
  p_jit[index++] = k_jit_bytes_shift;

  return index;
}

static size_t jit_emit_stack_inc(unsigned char* p_jit, size_t index) {
  // inc sil
  p_jit[index++] = 0x40;
  p_jit[index++] = 0xfe;
  p_jit[index++] = 0xc6;

  return index;
}

static size_t jit_emit_stack_dec(unsigned char* p_jit, size_t index) {
  // dec sil
  p_jit[index++] = 0x40;
  p_jit[index++] = 0xfe;
  p_jit[index++] = 0xce;

  return index;
}

static size_t jit_emit_pull_to_a(unsigned char* p_jit, size_t index) {
  index = jit_emit_stack_inc(p_jit, index);
  // mov al, [rsi]
  p_jit[index++] = 0x8a;
  p_jit[index++] = 0x06;

  return index;
}

static size_t jit_emit_pull_to_scratch(unsigned char* p_jit, size_t index) {
  index = jit_emit_stack_inc(p_jit, index);
  // mov dl, [rsi]
  p_jit[index++] = 0x8a;
  p_jit[index++] = 0x16;

  return index;
}

static size_t jit_emit_pull_to_scratch_word(unsigned char* p_jit,
                                            size_t index) {
  index = jit_emit_stack_inc(p_jit, index);
  // movzx edx, BYTE PTR [rsi]
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0xb6;
  p_jit[index++] = 0x16;
  index = jit_emit_stack_inc(p_jit, index);
  // mov dh, BYTE PTR [rsi]
  p_jit[index++] = 0x8a;
  p_jit[index++] = 0x36;

  return index;
}

static size_t jit_emit_push_from_a(unsigned char* p_jit, size_t index) {
  // mov [rsi], al
  p_jit[index++] = 0x88;
  p_jit[index++] = 0x06;
  index = jit_emit_stack_dec(p_jit, index);

  return index;
}

static size_t jit_emit_push_from_scratch(unsigned char* p_jit, size_t index) {
  // mov [rsi], dl
  p_jit[index++] = 0x88;
  p_jit[index++] = 0x16;
  index = jit_emit_stack_dec(p_jit, index);

  return index;
}

static size_t jit_emit_push_from_scratch_word(unsigned char* p_jit,
                                              size_t index) {
  // mov [rsi], dh
  p_jit[index++] = 0x88;
  p_jit[index++] = 0x36;
  index = jit_emit_stack_dec(p_jit, index);
  // mov [rsi], dl
  p_jit[index++] = 0x88;
  p_jit[index++] = 0x16;
  index = jit_emit_stack_dec(p_jit, index);

  return index;
}

static size_t jit_emit_6502_ip_to_scratch(unsigned char* p_jit, size_t index) {
  // lea rdx, [rip - (k_addr_space_size + k_guard_size)]
  p_jit[index++] = 0x48;
  p_jit[index++] = 0x8d;
  p_jit[index++] = 0x15;
  index = jit_emit_int(p_jit,
                       index,
                       -(ssize_t) (k_addr_space_size + k_guard_size));
  // sub rdx, rdi
  p_jit[index++] = 0x48;
  p_jit[index++] = 0x29;
  p_jit[index++] = 0xfa;
  index = jit_emit_jit_bytes_shift_scratch_right(p_jit, index);

  return index;
}

static size_t jit_emit_push_ip_plus_two(unsigned char* p_jit, size_t index) {
  index = jit_emit_6502_ip_to_scratch(p_jit, index);
  // add edx, 2
  p_jit[index++] = 0x83;
  p_jit[index++] = 0xc2;
  p_jit[index++] = 0x02;
  index = jit_emit_push_from_scratch_word(p_jit, index);

  return index;
}

static size_t jit_emit_php(unsigned char* p_jit, size_t index) {
  // mov rdx, r8
  p_jit[index++] = 0x4c;
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xc2;
  // or rdx, r12
  p_jit[index++] = 0x4c;
  p_jit[index++] = 0x09;
  p_jit[index++] = 0xe2;

  // mov r9, r13
  p_jit[index++] = 0x4d;
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xe9;
  // shl r9, 1
  p_jit[index++] = 0x49;
  p_jit[index++] = 0xd1;
  p_jit[index++] = 0xe1;
  // or rdx, r9
  p_jit[index++] = 0x4c;
  p_jit[index++] = 0x09;
  p_jit[index++] = 0xca;

  // mov r9, r14
  p_jit[index++] = 0x4d;
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xf1;
  // shl r9, 7
  p_jit[index++] = 0x49;
  p_jit[index++] = 0xc1;
  p_jit[index++] = 0xe1;
  p_jit[index++] = 0x07;
  // or rdx, r9
  p_jit[index++] = 0x4c;
  p_jit[index++] = 0x09;
  p_jit[index++] = 0xca;

  // mov r9, r15
  p_jit[index++] = 0x4d;
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xf9;
  // shl r9, 6
  p_jit[index++] = 0x49;
  p_jit[index++] = 0xc1;
  p_jit[index++] = 0xe1;
  p_jit[index++] = 0x06;
  // or rdx, r9
  p_jit[index++] = 0x4c;
  p_jit[index++] = 0x09;
  p_jit[index++] = 0xca;

  index = jit_emit_push_from_scratch(p_jit, index);

  return index;
}

static size_t jit_emit_jmp_indirect(unsigned char* p_jit,
                                    size_t index,
                                    unsigned char addr_low,
                                    unsigned char addr_high) {
  unsigned char next_addr_high = addr_high;
  unsigned char next_addr_low = addr_low + 1;
  if (next_addr_low == 0) {
    next_addr_high++;
  }
  // movzx edx, BYTE PTR [rdi + low,high]
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0xb6;
  p_jit[index++] = 0x97;
  index = jit_emit_op1_op2(p_jit, index, addr_low, addr_high);
  // mov dh, BYTE PTR [rdi + low,high + 1]
  p_jit[index++] = 0x8a;
  p_jit[index++] = 0xb7;
  index = jit_emit_op1_op2(p_jit, index, next_addr_low, next_addr_high);
  index = jit_emit_jit_bytes_shift_scratch_left(p_jit, index);
  // lea rdx, [rdi + rdx + k_addr_space_size + k_guard_size]
  p_jit[index++] = 0x48;
  p_jit[index++] = 0x8d;
  p_jit[index++] = 0x94;
  p_jit[index++] = 0x17;
  index = jit_emit_int(p_jit, index, k_addr_space_size + k_guard_size);
  index = jit_emit_jmp_scratch(p_jit, index);

  return index;
}

size_t jit_emit_undefined(unsigned char* p_jit,
                          size_t index,
                          unsigned char opcode,
                          size_t jit_offset) {
  // ud2
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x0b;
  // Copy of unimplemented 6502 opcode.
  p_jit[index++] = opcode;
  // Virtual address of opcode, big endian.
  p_jit[index++] = jit_offset >> 8;
  p_jit[index++] = jit_offset & 0xff;

  return index;
}

static size_t
jit_emit_save_registers(unsigned char* p_jit, size_t index) {
  // No need to push rdx because it's a scratch register.
  // push rax / rcx / rsi / rdi
  p_jit[index++] = 0x50;
  p_jit[index++] = 0x51;
  p_jit[index++] = 0x56;
  p_jit[index++] = 0x57;
  // push r8
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x50;

  return index;
}

static size_t
jit_emit_restore_registers(unsigned char* p_jit, size_t index) {
  // pop r8
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x58;
  // pop rdi / rsi / rcx / rax
  p_jit[index++] = 0x5f;
  p_jit[index++] = 0x5e;
  p_jit[index++] = 0x59;
  p_jit[index++] = 0x58;

  return index;
}

static size_t
jit_emit_debug_sequence(unsigned char* p_jit, size_t index) {
  index = jit_emit_save_registers(p_jit, index);

  // param11: 6502 S
  // push rsi
  p_jit[index++] = 0x56;

  // param10: 6502 Y
  // push rcx
  p_jit[index++] = 0x51;

  // param9: 6502 X
  // push rbx
  p_jit[index++] = 0x53;

  // param8: 6502 A
  // push rax
  p_jit[index++] = 0x50;

  // param7: remaining flags
  // push r8
  p_jit[index++] = 0x41;
  p_jit[index++] = 0x50;

  // param2: 6502 IP
  // (Must be done before param1, which trashes rdi.)
  index = jit_emit_6502_ip_to_scratch(p_jit, index);
  // mov rsi, rdx
  p_jit[index++] = 0x48;
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xd6;

  // param1
  // mov rdi, [rbp + k_offset_debug]
  p_jit[index++] = 0x48;
  p_jit[index++] = 0x8b;
  p_jit[index++] = 0x7d;
  p_jit[index++] = k_offset_debug;

  // param3: 6502 FZ
  // mov rdx, r13
  p_jit[index++] = 0x4c;
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xea;

  // param4: 6502 FN
  // mov rcx, r14
  p_jit[index++] = 0x4c;
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xf1;

  // param5: 6502 FC
  // mov r8, r12
  p_jit[index++] = 0x4d;
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xe0;

  // param6: 6502 FO
  // mov r9, r15
  p_jit[index++] = 0x4d;
  p_jit[index++] = 0x89;
  p_jit[index++] = 0xf9;

  // call [rbp + k_offset_debug_callback]
  p_jit[index++] = 0xff;
  p_jit[index++] = 0x55;
  p_jit[index++] = k_offset_debug_callback;

  // add rsp, 40
  p_jit[index++] = 0x48;
  p_jit[index++] = 0x83;
  p_jit[index++] = 0xc4;
  p_jit[index++] = 40;

  index = jit_emit_restore_registers(p_jit, index);

  return index;
}

static size_t jit_emit_calc_op(unsigned char* p_jit,
                               size_t index,
                               unsigned char opmode,
                               unsigned char operand1,
                               unsigned char operand2,
                               unsigned char intel_op_base) {
  switch (opmode) {
  case k_imm:
    // OP al, op1
    p_jit[index++] = intel_op_base + 2;
    p_jit[index++] = operand1;
    break;
  case k_zpg:
  case k_abs:
    // OP al, [rdi + op1,op2?]
    p_jit[index++] = intel_op_base;
    p_jit[index++] = 0x87;
    index = jit_emit_op1_op2(p_jit, index, operand1, operand2);
    break;
  default:
    // OP al, [rdi + rdx]
    p_jit[index++] = intel_op_base;
    p_jit[index++] = 0x04;
    p_jit[index++] = 0x17;
    break;
  }

  return index;
}

static size_t jit_emit_shift_op(unsigned char* p_jit,
                                size_t index,
                                unsigned char opmode,
                                unsigned char operand1,
                                unsigned char operand2,
                                unsigned char intel_op_base) {
  switch (opmode) {
  case k_nil:
    // OP al, 1
    p_jit[index++] = 0xd0;
    p_jit[index++] = intel_op_base;
    break;
  case k_zpg:
  case k_abs:
    // OP BYTE PTR [rdi + op1,op2?], 1
    p_jit[index++] = 0xd0;
    p_jit[index++] = intel_op_base - 0x39;
    index = jit_emit_op1_op2(p_jit, index, operand1, operand2);
    break;
  default:
    // OP BYTE PTR [rdi + rdx], 1
    p_jit[index++] = 0xd0;
    p_jit[index++] = intel_op_base - 0xbc;
    p_jit[index++] = 0x17;
    break;
  }

  return index;
}

static size_t jit_emit_post_rotate(unsigned char* p_jit,
                                   size_t index,
                                   unsigned char opmode,
                                   unsigned char operand1,
                                   unsigned char operand2) {
  index = jit_emit_intel_to_6502_carry(p_jit, index);
  switch (opmode) {
  case k_nil:
    index = jit_emit_do_zn_flags(p_jit, index, 0);
    break;
  case k_zpg:
  case k_abs:
    // test BYTE PTR [rdi + op1,op2?], 0xff
    p_jit[index++] = 0xf6;
    p_jit[index++] = 0x87;
    index = jit_emit_op1_op2(p_jit, index, operand1, operand2);
    p_jit[index++] = 0xff;
    index = jit_emit_do_zn_flags(p_jit, index, -1);
    break;
  default:
    // test BYTE PTR [rdi + rdx], 0xff
    p_jit[index++] = 0xf6;
    p_jit[index++] = 0x04;
    p_jit[index++] = 0x17;
    p_jit[index++] = 0xff;
    index = jit_emit_do_zn_flags(p_jit, index, -1);
    break;
  }

  return index;
}

static size_t
jit_check_special_read(struct jit_struct* p_jit,
                       uint16_t addr,
                       unsigned char* p_jit_buf,
                       size_t index) {
  if (!bbc_is_special_read_addr(p_jit->p_bbc, addr)) {
    return index;
  }
  index = jit_emit_save_registers(p_jit_buf, index);

  // mov rdi, [rbp + k_offset_bbc]
  p_jit_buf[index++] = 0x48;
  p_jit_buf[index++] = 0x8b;
  p_jit_buf[index++] = 0x7d;
  p_jit_buf[index++] = k_offset_bbc;
  // mov si, addr
  p_jit_buf[index++] = 0x66;
  p_jit_buf[index++] = 0xbe;
  p_jit_buf[index++] = addr & 0xff;
  p_jit_buf[index++] = addr >> 8;
  // call [rbp + k_offset_read_callback]
  p_jit_buf[index++] = 0xff;
  p_jit_buf[index++] = 0x55;
  p_jit_buf[index++] = k_offset_read_callback;
  // mov rdx, rax
  p_jit_buf[index++] = 0x48;
  p_jit_buf[index++] = 0x89;
  p_jit_buf[index++] = 0xc2;

  index = jit_emit_restore_registers(p_jit_buf, index);
  // mov BYTE PTR [rdi + addr], dl
  p_jit_buf[index++] = 0x88;
  p_jit_buf[index++] = 0x97;
  p_jit_buf[index++] = addr & 0xff;
  p_jit_buf[index++] = addr >> 8;
  p_jit_buf[index++] = 0;
  p_jit_buf[index++] = 0;

  return index;
}

static size_t
jit_check_special_write(struct jit_struct* p_jit,
                        uint16_t addr,
                        unsigned char* p_jit_buf,
                        size_t index) {
  if (!bbc_is_special_write_addr(p_jit->p_bbc, addr)) {
    return index;
  }
  index = jit_emit_save_registers(p_jit_buf, index);

  // mov rdi, [rbp + k_offset_bbc]
  p_jit_buf[index++] = 0x48;
  p_jit_buf[index++] = 0x8b;
  p_jit_buf[index++] = 0x7d;
  p_jit_buf[index++] = k_offset_bbc;
  // mov si, addr
  p_jit_buf[index++] = 0x66;
  p_jit_buf[index++] = 0xbe;
  p_jit_buf[index++] = addr & 0xff;
  p_jit_buf[index++] = addr >> 8;
  // call [rbp + k_offset_write_callback]
  p_jit_buf[index++] = 0xff;
  p_jit_buf[index++] = 0x55;
  p_jit_buf[index++] = k_offset_write_callback;

  index = jit_emit_restore_registers(p_jit_buf, index);

  return index;
}

void
jit_set_interrupt(struct jit_struct* p_jit, int interrupt) {
  p_jit->interrupt = interrupt;
}

void
jit_jit(struct jit_struct* p_jit,
        size_t jit_offset,
        size_t jit_len,
        unsigned int debug_flags) {
  unsigned char* p_mem = p_jit->p_mem;
  unsigned char* p_jit_buf = p_mem + k_addr_space_size + k_guard_size;
  size_t jit_end = jit_offset + jit_len;
  p_mem += jit_offset;
  p_jit_buf += (jit_offset * k_jit_bytes_per_byte);
  while (jit_offset < jit_end) {
    unsigned char opcode = p_mem[0];
    unsigned char opmode = g_opmodes[opcode];
    unsigned char optype = g_optypes[opcode];
    unsigned char oplen = 1;
    unsigned char operand1 = 0;
    unsigned char operand2 = 0;
    uint16_t addr;
    size_t index = 0;

    // Note: not correct if JIT code wraps the address space but that shouldn't
    // happen in normal operation: the end of address space contains IRQ / reset
    // etc. vectors.
    if (jit_offset + 1 < jit_end) {
      operand1 = p_mem[1];
    }
    if (jit_offset + 2 < jit_end) {
      operand2 = p_mem[2];
    }

    if (debug_flags) {
      index = jit_emit_debug_sequence(p_jit_buf, index);
    }

    switch (opmode) {
    case k_imm:
      oplen = 2;
      break;
    case k_zpg:
      oplen = 2;
      break;
    case k_abs:
      oplen = 3;
      break;
    case k_zpx:
      index = jit_emit_zp_x_to_scratch(p_jit_buf, index, operand1);
      oplen = 2;
      break;
    case k_zpy:
      index = jit_emit_zp_y_to_scratch(p_jit_buf, index, operand1);
      oplen = 2;
      break;
    case k_abx:
      index = jit_emit_abs_x_to_scratch(p_jit_buf, index, operand1, operand2);
      oplen = 3;
      break;
    case k_aby:
      index = jit_emit_abs_y_to_scratch(p_jit_buf, index, operand1, operand2);
      oplen = 3;
      break;
    case k_idy:
      index = jit_emit_ind_y_to_scratch(p_jit_buf, index, operand1);
      oplen = 2;
      break;
    case k_idx:
      index = jit_emit_ind_x_to_scratch(p_jit_buf, index, operand1);
      oplen = 2;
      break;
    case k_ind:
      oplen = 3;
      break;
    default:
      break;
    }

    if (oplen < 3) { 
      // Clear operand2 if we're not using it. This enables us to re-use the
      // same x64 opcode generation code for both k_zpg and k_abs.
      operand2 = 0;
    }
    addr = (operand2 << 8) | operand1;

    switch (optype) {
    case k_kil:
      switch (opcode) {
      case 0x02:
        // Illegal opcode. Hangs a standard 6502.
        // Bounce out of JIT.
        // ret
        p_jit_buf[index++] = 0xc3;
        break;
      case 0x12:
        // Illegal opcode. Hangs a standard 6502.
        // Generate a debug trap and continue.
        // int 3
        p_jit_buf[index++] = 0xcc;
        break;
      case 0xf2:
        // Illegal opcode. Hangs a standard 6502.
        // Generate a SEGV.
        // xor rdx, rdx
        p_jit_buf[index++] = 0x31;
        p_jit_buf[index++] = 0xd2;
        index = jit_emit_jmp_scratch(p_jit_buf, index);
        break;
      default:
        index = jit_emit_undefined(p_jit_buf, index, opcode, jit_offset);
        break;
      }
      break;
    case k_brk:
      // BRK
      index = jit_emit_push_ip_plus_two(p_jit_buf, index);
      index = jit_emit_php(p_jit_buf, index);
      index = jit_emit_jmp_indirect(p_jit_buf, index, 0xfe, 0xff);
      break;
    case k_ora:
      // ORA
      index = jit_emit_calc_op(p_jit_buf,
                               index,
                               opmode,
                               operand1,
                               operand2,
                               0x0a);
      index = jit_emit_do_zn_flags(p_jit_buf, index, -1);
      break;
    case k_asl:
      // ASL
      index = jit_emit_shift_op(p_jit_buf,
                                index,
                                opmode,
                                operand1,
                                operand2,
                                0xe0);
      index = jit_emit_intel_to_6502_znc(p_jit_buf, index);
      break;
    case k_php:
      // PHP
      index = jit_emit_php(p_jit_buf, index);
      break;
    case k_bpl:
      // BPL
      index = jit_emit_test_negative(p_jit_buf, index);
      // je
      index = jit_emit_do_relative_jump(p_jit_buf, index, 0x74, operand1);
      break;
    case k_clc:
      // CLC
      index = jit_emit_set_carry(p_jit_buf, index, 0);
      break;
    case k_jsr:
      // JSR
      index = jit_emit_push_ip_plus_two(p_jit_buf, index);
      index = jit_emit_jmp_op1_op2(p_jit_buf, index, operand1, operand2);
      break;
    case k_bit:
      // BIT
      // Only has zp and abs
      index = jit_check_special_read(p_jit, addr, p_jit_buf, index);
      // mov dl [rdi + op1,op2?]
      p_jit_buf[index++] = 0x8a;
      p_jit_buf[index++] = 0x97;
      index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
      index = jit_emit_scratch_bit_test(p_jit_buf, index, 7);
      index = jit_emit_carry_to_6502_negative(p_jit_buf, index);
      index = jit_emit_scratch_bit_test(p_jit_buf, index, 6);
      index = jit_emit_carry_to_6502_overflow(p_jit_buf, index);
      // and dl, al
      p_jit_buf[index++] = 0x20;
      p_jit_buf[index++] = 0xc2;
      index = jit_emit_intel_to_6502_zero(p_jit_buf, index);
      break;
    case k_and:
      // AND
      index = jit_emit_calc_op(p_jit_buf,
                               index,
                               opmode,
                               operand1,
                               operand2,
                               0x22);
      index = jit_emit_do_zn_flags(p_jit_buf, index, -1);
      break;
    case k_rol:
      // ROL
      index = jit_emit_6502_carry_to_intel(p_jit_buf, index);
      index = jit_emit_shift_op(p_jit_buf,
                                index,
                                opmode,
                                operand1,
                                operand2,
                                0xd0);
      index = jit_emit_post_rotate(p_jit_buf,
                                   index,
                                   opmode,
                                   operand1,
                                   operand2);
      break;
    case k_plp:
      // PLP
      index = jit_emit_pull_to_scratch(p_jit_buf, index);

      index = jit_emit_scratch_bit_test(p_jit_buf, index, 0);
      index = jit_emit_intel_to_6502_carry(p_jit_buf, index);
      index = jit_emit_scratch_bit_test(p_jit_buf, index, 1);
      index = jit_emit_carry_to_6502_zero(p_jit_buf, index);
      index = jit_emit_scratch_bit_test(p_jit_buf, index, 6);
      index = jit_emit_carry_to_6502_overflow(p_jit_buf, index);
      index = jit_emit_scratch_bit_test(p_jit_buf, index, 7);
      index = jit_emit_carry_to_6502_negative(p_jit_buf, index);
      // mov r8b, dl
      p_jit_buf[index++] = 0x41;
      p_jit_buf[index++] = 0x88;
      p_jit_buf[index++] = 0xd0;
      // and r8b, 0x3c
      p_jit_buf[index++] = 0x41;
      p_jit_buf[index++] = 0x80;
      p_jit_buf[index++] = 0xe0;
      p_jit_buf[index++] = 0x3c;
      break;
    case k_bmi:
      // BMI
      index = jit_emit_test_negative(p_jit_buf, index);
      // jne
      index = jit_emit_do_relative_jump(p_jit_buf, index, 0x75, operand1);
      break;
    case k_sec:
      // SEC
      index = jit_emit_set_carry(p_jit_buf, index, 1);
      break;
    case k_eor:
      // EOR
      index = jit_emit_calc_op(p_jit_buf,
                               index,
                               opmode,
                               operand1,
                               operand2,
                               0x32);
      index = jit_emit_do_zn_flags(p_jit_buf, index, -1);
      break;
    case k_lsr:
      // LSR
      index = jit_emit_shift_op(p_jit_buf,
                                index,
                                opmode,
                                operand1,
                                operand2,
                                0xe8);
      index = jit_emit_intel_to_6502_znc(p_jit_buf, index);
      break;
    case k_pha:
      // PHA
      index = jit_emit_push_from_a(p_jit_buf, index);
      break;
    case k_jmp:
      // JMP
      if (opmode == k_abs) {
        index = jit_emit_jmp_op1_op2(p_jit_buf, index, operand1, operand2);
      } else {
        index = jit_emit_jmp_indirect(p_jit_buf, index, operand1, operand2);
      }
      break;
    case k_bvc:
      // BVC
      index = jit_emit_test_overflow(p_jit_buf, index);
      // je
      index = jit_emit_do_relative_jump(p_jit_buf, index, 0x74, operand1);
      break;
    case k_cli:
      // CLI
      // btr r8, 2
      p_jit_buf[index++] = 0x49;
      p_jit_buf[index++] = 0x0f;
      p_jit_buf[index++] = 0xba;
      p_jit_buf[index++] = 0xf0;
      p_jit_buf[index++] = 0x02;
      break;
    case k_rts:
      // RTS
      index = jit_emit_pull_to_scratch_word(p_jit_buf, index);
      // inc dx
      p_jit_buf[index++] = 0x66;
      p_jit_buf[index++] = 0xff;
      p_jit_buf[index++] = 0xc2;
      index = jit_emit_jit_bytes_shift_scratch_left(p_jit_buf, index);
      // lea rdx, [rdi + rdx + k_addr_space_size + k_guard_size]
      p_jit_buf[index++] = 0x48;
      p_jit_buf[index++] = 0x8d;
      p_jit_buf[index++] = 0x94;
      p_jit_buf[index++] = 0x17;
      index = jit_emit_int(p_jit_buf, index, k_addr_space_size + k_guard_size);
      index = jit_emit_jmp_scratch(p_jit_buf, index);
      break;
    case k_adc:
      // ADC
      index = jit_emit_6502_carry_to_intel(p_jit_buf, index);
      index = jit_emit_calc_op(p_jit_buf,
                               index,
                               opmode,
                               operand1,
                               operand2,
                               0x12);
      index = jit_emit_intel_to_6502_znco(p_jit_buf, index);
      break;
    case k_ror:
      // ROR
      index = jit_emit_6502_carry_to_intel(p_jit_buf, index);
      index = jit_emit_shift_op(p_jit_buf,
                                index,
                                opmode,
                                operand1,
                                operand2,
                                0xd8);
      index = jit_emit_post_rotate(p_jit_buf, index, opmode, operand1, operand2);
      break;
    case k_pla:
      // PLA
      index = jit_emit_pull_to_a(p_jit_buf, index);
      index = jit_emit_do_zn_flags(p_jit_buf, index, 0);
      break;
    case k_bvs:
      // BVS
      index = jit_emit_test_overflow(p_jit_buf, index);
      // jne
      index = jit_emit_do_relative_jump(p_jit_buf, index, 0x75, operand1);
      break;
    case k_sei:
      // SEI
      // bts r8, 2
      p_jit_buf[index++] = 0x49;
      p_jit_buf[index++] = 0x0f;
      p_jit_buf[index++] = 0xba;
      p_jit_buf[index++] = 0xe8;
      p_jit_buf[index++] = 0x02;
      break;
    case k_sta:
      // STA
      switch (opmode) {
      case k_zpg:
      case k_abs:
        // mov [rdi + op1,op2?], al
        p_jit_buf[index++] = 0x88;
        p_jit_buf[index++] = 0x87;
        index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
        index = jit_check_special_write(p_jit, addr, p_jit_buf, index);
        break;
      default:
        // mov [rdi + rdx], al
        p_jit_buf[index++] = 0x88;
        p_jit_buf[index++] = 0x04;
        p_jit_buf[index++] = 0x17;
        break;
      }
      break;
    case k_sty:
      // STY
      switch (opmode) {
      case k_zpg:
      case k_abs:
        // mov [rdi + op1,op2?], cl
        p_jit_buf[index++] = 0x88;
        p_jit_buf[index++] = 0x8f;
        index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
        index = jit_check_special_write(p_jit, addr, p_jit_buf, index);
        break;
      default:
        // mov [rdi + rdx], cl
        p_jit_buf[index++] = 0x88;
        p_jit_buf[index++] = 0x0c;
        p_jit_buf[index++] = 0x17;
        break;
      }
      break;
    case k_stx:
      // STX
      switch (opmode) {
      case k_zpg:
      case k_abs:
        // mov [rdi + op1,op2?], bl
        p_jit_buf[index++] = 0x88;
        p_jit_buf[index++] = 0x9f;
        index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
        index = jit_check_special_write(p_jit, addr, p_jit_buf, index);
        break;
      default:
        // mov [rdi + rdx], bl
        p_jit_buf[index++] = 0x88;
        p_jit_buf[index++] = 0x1c;
        p_jit_buf[index++] = 0x17;
        break;
      }
      break;
    case k_dey:
      // DEY
      // dec cl
      p_jit_buf[index++] = 0xfe;
      p_jit_buf[index++] = 0xc9;
      index = jit_emit_do_zn_flags(p_jit_buf, index, -1);
      break;
    case k_txa:
      // TXA
      // mov al, bl
      p_jit_buf[index++] = 0x88;
      p_jit_buf[index++] = 0xd8;
      index = jit_emit_do_zn_flags(p_jit_buf, index, 0);
      break;
    case k_bcc:
      // BCC
      index = jit_emit_test_carry(p_jit_buf, index);
      // je
      index = jit_emit_do_relative_jump(p_jit_buf, index, 0x74, operand1);
      break;
    case k_tya:
      // TYA
      // mov al, cl
      p_jit_buf[index++] = 0x88;
      p_jit_buf[index++] = 0xc8;
      index = jit_emit_do_zn_flags(p_jit_buf, index, 0);
      break;
    case k_txs:
      // TXS
      // mov sil, bl
      p_jit_buf[index++] = 0x40;
      p_jit_buf[index++] = 0x88;
      p_jit_buf[index++] = 0xde;
      break;
    case k_ldy:
      // LDY
      switch (opmode) {
      case k_imm:
        // mov cl, op1
        p_jit_buf[index++] = 0xb1;
        p_jit_buf[index++] = operand1;
        break;
      case k_zpg:
      case k_abs:
        index = jit_check_special_read(p_jit, addr, p_jit_buf, index);
        // mov cl, [rdi + op1,op2?]
        p_jit_buf[index++] = 0x8a;
        p_jit_buf[index++] = 0x8f;
        index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
        break;
      default:
        // mov cl, [rdi + rdx]
        p_jit_buf[index++] = 0x8a;
        p_jit_buf[index++] = 0x0c;
        p_jit_buf[index++] = 0x17;
        break;
      }
      index = jit_emit_do_zn_flags(p_jit_buf, index, 2);
      break;
    case k_ldx:
      // LDX
      switch (opmode) {
      case k_imm:
        // mov bl, op1
        p_jit_buf[index++] = 0xb3;
        p_jit_buf[index++] = operand1;
        break;
      case k_zpg:
      case k_abs:
        index = jit_check_special_read(p_jit, addr, p_jit_buf, index);
        // mov bl, [rdi + op1,op2?]
        p_jit_buf[index++] = 0x8a;
        p_jit_buf[index++] = 0x9f;
        index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
        break;
      default:
        // mov bl, [rdi + rdx]
        p_jit_buf[index++] = 0x8a;
        p_jit_buf[index++] = 0x1c;
        p_jit_buf[index++] = 0x17;
        break;
      }
      index = jit_emit_do_zn_flags(p_jit_buf, index, 1);
      break;
    case k_lda:
      // LDA
      switch (opmode) {
      case k_imm:
        // mov al, op1
        p_jit_buf[index++] = 0xb0;
        p_jit_buf[index++] = operand1;
        break;
      case k_zpg:
      case k_abs:
        index = jit_check_special_read(p_jit, addr, p_jit_buf, index);
        // mov al, [rdi + op1,op2?]
        p_jit_buf[index++] = 0x8a;
        p_jit_buf[index++] = 0x87;
        index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
        break;
      default:
        // mov al, [rdi + rdx]
        p_jit_buf[index++] = 0x8a;
        p_jit_buf[index++] = 0x04;
        p_jit_buf[index++] = 0x17;
        break;
      }
      index = jit_emit_do_zn_flags(p_jit_buf, index, 0);
      break;
    case k_tay:
      // TAY
      // mov cl, al
      p_jit_buf[index++] = 0x88;
      p_jit_buf[index++] = 0xc1;
      index = jit_emit_do_zn_flags(p_jit_buf, index, 2);
      break;
    case k_tax:
      // TAX
      // mov bl, al
      p_jit_buf[index++] = 0x88;
      p_jit_buf[index++] = 0xc3;
      index = jit_emit_do_zn_flags(p_jit_buf, index, 1);
      break;
    case k_bcs:
      // BCS
      index = jit_emit_test_carry(p_jit_buf, index);
      // jne
      index = jit_emit_do_relative_jump(p_jit_buf, index, 0x75, operand1);
      break;
    case k_clv:
      // CLV
      // mov r15b, 0
      p_jit_buf[index++] = 0x41;
      p_jit_buf[index++] = 0xb7;
      p_jit_buf[index++] = 0x00;
      break;
    case k_tsx:
      // TSX
      // mov bl, sil
      p_jit_buf[index++] = 0x40;
      p_jit_buf[index++] = 0x88;
      p_jit_buf[index++] = 0xf3;
      index = jit_emit_do_zn_flags(p_jit_buf, index, 1);
      break;
    case k_cpy:
      // CPY
      switch (opmode) {
      case k_imm:
        // cmp cl, op1
        p_jit_buf[index++] = 0x80;
        p_jit_buf[index++] = 0xf9;
        p_jit_buf[index++] = operand1;
        break;
      case k_zpg:
      case k_abs:
        // cmp cl, [rdi + op1,op2?]
        p_jit_buf[index++] = 0x3a;
        p_jit_buf[index++] = 0x8f;
        index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
        break;
      }
      index = jit_emit_intel_to_6502_sub_znc(p_jit_buf, index);
      break;
    case k_cmp:
      // CMP
      index = jit_emit_calc_op(p_jit_buf,
                               index,
                               opmode,
                               operand1,
                               operand2,
                               0x3a);
      index = jit_emit_intel_to_6502_sub_znc(p_jit_buf, index);
      break;
    case k_dec:
      // DEC
      switch (opmode) {
      case k_zpg:
      case k_abs:
        // dec BYTE PTR [rdi + op1,op2?]
        p_jit_buf[index++] = 0xfe;
        p_jit_buf[index++] = 0x8f;
        index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
        break;
      default: 
        // dec BYTE PTR [rdi + rdx]
        p_jit_buf[index++] = 0xfe;
        p_jit_buf[index++] = 0x0c;
        p_jit_buf[index++] = 0x17;
        break;
      }
      index = jit_emit_do_zn_flags(p_jit_buf, index, -1);
      break;
    case k_iny:
      // INY
      // inc cl
      p_jit_buf[index++] = 0xfe;
      p_jit_buf[index++] = 0xc1;
      index = jit_emit_do_zn_flags(p_jit_buf, index, -1);
      break;
    case k_dex:
      // DEX
      // dec bl
      p_jit_buf[index++] = 0xfe;
      p_jit_buf[index++] = 0xcb;
      index = jit_emit_do_zn_flags(p_jit_buf, index, -1);
      break;
    case k_bne:
      // BNE
      index = jit_emit_test_zero(p_jit_buf, index);
      // je
      index = jit_emit_do_relative_jump(p_jit_buf, index, 0x74, operand1);
      break;
    case k_cld:
      // CLD
      // btr r8, 3
      p_jit_buf[index++] = 0x49;
      p_jit_buf[index++] = 0x0f;
      p_jit_buf[index++] = 0xba;
      p_jit_buf[index++] = 0xf0;
      p_jit_buf[index++] = 0x03;
      break;
    case k_cpx:
      // CPX
      switch (opmode) {
      case k_imm:
        // cmp bl, op1
        p_jit_buf[index++] = 0x80;
        p_jit_buf[index++] = 0xfb;
        p_jit_buf[index++] = operand1;
        break;
      case k_zpg:
      case k_abs:
        // cmp bl, [rdi + op1,op2?]
        p_jit_buf[index++] = 0x3a;
        p_jit_buf[index++] = 0x9f;
        index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
        break;
      }
      index = jit_emit_intel_to_6502_sub_znc(p_jit_buf, index);
      break;
    case k_inc:
      // INC
      switch (opmode) {
      case k_zpg:
      case k_abs:
        p_jit_buf[index++] = 0xfe;
        p_jit_buf[index++] = 0x87;
        index = jit_emit_op1_op2(p_jit_buf, index, operand1, operand2);
        break;
      default: 
        // inc BYTE PTR [rdi + rdx]
        p_jit_buf[index++] = 0xfe;
        p_jit_buf[index++] = 0x04;
        p_jit_buf[index++] = 0x17;
        break;
      }
      index = jit_emit_do_zn_flags(p_jit_buf, index, -1);
      break;
    case k_inx:
      // INX
      // inc bl
      p_jit_buf[index++] = 0xfe;
      p_jit_buf[index++] = 0xc3;
      index = jit_emit_do_zn_flags(p_jit_buf, index, -1);
      break;
    case k_sbc:
      // SBC
      index = jit_emit_6502_carry_to_intel(p_jit_buf, index);
      // cmc
      p_jit_buf[index++] = 0xf5;
      index = jit_emit_calc_op(p_jit_buf,
                               index,
                               opmode,
                               operand1,
                               operand2,
                               0x1a);
      index = jit_emit_intel_to_6502_sub_znco(p_jit_buf, index);
      break;
    case k_nop:
      // NOP
      break;
    case k_beq:
      // BEQ
      index = jit_emit_test_zero(p_jit_buf, index);
      // jne
      index = jit_emit_do_relative_jump(p_jit_buf, index, 0x75, operand1);
      break;
    default:
      index = jit_emit_undefined(p_jit_buf, index, opcode, jit_offset);
      break;
    }

    index = jit_emit_do_jmp_next(p_jit_buf, index, oplen);

    assert(index <= k_jit_bytes_per_byte);

    p_mem++;
    p_jit_buf += k_jit_bytes_per_byte;
    jit_offset++;
  }
}

void
jit_enter(struct jit_struct* p_jit, size_t vector_addr) {
  unsigned char* p_mem = p_jit->p_mem;
  unsigned char addr_lsb = p_mem[vector_addr];
  unsigned char addr_msb = p_mem[vector_addr + 1];
  unsigned int addr = (addr_msb << 8) | addr_lsb;
  unsigned char* p_jit_buf = p_mem + k_addr_space_size + k_guard_size;
  unsigned char* p_entry = p_jit_buf + (addr * k_jit_bytes_per_byte);

  // The memory must be aligned to at least 0x100 so that our stack access
  // trick works.
  assert(((size_t) p_mem & 0xff) == 0);

  asm volatile (
    // al is 6502 A.
    "xor %%eax, %%eax;"
    // bl is 6502 X.
    "xor %%ebx, %%ebx;"
    // cl is 6502 Y.
    "xor %%ecx, %%ecx;"
    // rdx is scratch.
    "xor %%edx, %%edx;"
    // r8 is the rest of the 6502 flags or'ed together.
    // Bit 2 is interrupt disable.
    // Bit 3 is decimal mode.
    // Bit 4 is set for BRK and PHP.
    // Bit 5 is always set.
    "xor %%r8, %%r8;"
    "bts $4, %%r8;"
    "bts $5, %%r8;"
    // r12 is carry flag.
    "xor %%r12, %%r12;"
    // r13 is zero flag.
    "xor %%r13, %%r13;"
    // r14 is negative flag.
    "xor %%r14, %%r14;"
    // r15 is overflow flag.
    "xor %%r15, %%r15;"
    // rdi points to the virtual RAM, guard page, JIT space.
    "mov %1, %%rdi;"
    // sil is 6502 S.
    // rsi is a pointer to the real (aligned) backing memory.
    "lea 0x100(%%rdi), %%rsi;"
    // Use scratch register for jump location.
    "mov %0, %%rdx;"
    // Pass a pointer to the jit_struct in rbp.
    "mov %2, %%r9;"
    "push %%rbp;"
    "mov %%r9, %%rbp;"
    "call *%%rdx;"
    "pop %%rbp;"
    :
    : "g" (p_entry), "g" (p_mem), "g" (p_jit)
    : "rax", "rbx", "rcx", "rdx", "rdi", "rsi",
      "r8", "r9", "r12", "r13", "r14", "r15"
  );
}

struct jit_struct*
jit_create(unsigned char* p_mem,
           void* p_debug_callback,
           struct debug_struct* p_debug,
           struct bbc_struct* p_bbc,
           void* p_read_callback,
           void* p_write_callback) {
  unsigned char* p_jit_buf = p_mem + k_addr_space_size + k_guard_size;
  struct jit_struct* p_jit = malloc(sizeof(struct jit_struct));
  if (p_jit == NULL) {
    errx(1, "cannot allocate jit_struct");
  }
  memset(p_jit, '\0', sizeof(struct jit_struct));
  p_jit->p_mem = p_mem;
  p_jit->p_debug = p_debug;
  p_jit->p_debug_callback = p_debug_callback;
  p_jit->p_bbc = p_bbc;
  p_jit->p_read_callback = p_read_callback;
  p_jit->p_write_callback = p_write_callback;

  // nop
  memset(p_jit_buf, '\x90', k_addr_space_size * k_jit_bytes_per_byte);
  size_t num_bytes = k_addr_space_size;
  while (num_bytes--) {
    // ud2
    p_jit_buf[0] = 0x0f;
    p_jit_buf[1] = 0x0b;
    p_jit_buf += k_jit_bytes_per_byte;
  }

  return p_jit;
}

void
jit_destroy(struct jit_struct* p_jit) {
  free(p_jit);
}