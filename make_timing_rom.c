#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "defs_6502.h"
#include "emit_6502.h"
#include "test_helper.h"
#include "util.h"

static const size_t k_rom_size = 16384;

static void
set_new_index(struct util_buffer* p_buf, size_t new_index) {
  size_t curr_index = util_buffer_get_pos(p_buf);
  assert(new_index >= curr_index);
  util_buffer_set_pos(p_buf, new_index);
}

int
main(int argc, const char* argv[]) {
  int fd;
  ssize_t write_ret;

  uint8_t* p_mem = malloc(k_rom_size);
  struct util_buffer* p_buf = util_buffer_create();

  (void) argc;
  (void) argv;

  (void) memset(p_mem, '\xf2', k_rom_size);
  util_buffer_setup(p_buf, p_mem, k_rom_size);

  /* Reset vector: jump to 0xC000, start of OS ROM. */
  p_mem[0x3FFC] = 0x00;
  p_mem[0x3FFD] = 0xC0;
  /* IRQ vector. */
  p_mem[0x3FFE] = 0x00;
  p_mem[0x3FFF] = 0xFF;

  /* Check instruction timings for page crossings in abx mode. */
  set_new_index(p_buf, 0x0000);
  emit_CYCLES_RESET(p_buf);
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 1);
  emit_LDX(p_buf, k_imm, 0x01);
  emit_CYCLES_RESET(p_buf);
  emit_LDA(p_buf, k_abx, 0x1000); /* LDA abx, no page crossing, 4 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 5);      /* Includes 1 cycle from CYCLES_RESET. */
  emit_CYCLES_RESET(p_buf);
  emit_LDA(p_buf, k_abx, 0x10FF); /* LDA abx, page crossing, 5 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 6);
  emit_CYCLES_RESET(p_buf);
  emit_STA(p_buf, k_abx, 0x1000); /* STA abx, no page crossing, 5 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 6);
  emit_CYCLES_RESET(p_buf);
  emit_STA(p_buf, k_abx, 0x10FF); /* STA abx, page crossing, 5 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 6);
  emit_JMP(p_buf, k_abs, 0xC040);

  /* Check instruction timings for page crossings in idy mode. */
  set_new_index(p_buf, 0x0040);
  emit_LDA(p_buf, k_imm, 0xFF);
  emit_STA(p_buf, k_abs, 0xB0);
  emit_LDA(p_buf, k_imm, 0x10);
  emit_STA(p_buf, k_abs, 0xB1);
  emit_LDY(p_buf, k_imm, 0x00);
  emit_CYCLES_RESET(p_buf);
  emit_LDA(p_buf, k_idy, 0xB0);   /* LDA idy, no page crossing, 5 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 6);
  emit_CYCLES_RESET(p_buf);
  emit_STA(p_buf, k_idy, 0xB0);   /* STA idy, no page crossing, 6 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 7);
  emit_LDY(p_buf, k_imm, 0x01);
  emit_CYCLES_RESET(p_buf);
  emit_LDA(p_buf, k_idy, 0xB0);   /* LDA idy, no page crossing, 6 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 7);
  emit_CYCLES_RESET(p_buf);
  emit_STA(p_buf, k_idy, 0xB0);   /* STA idy, page crossing, 6 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 7);
  emit_JMP(p_buf, k_abs, 0xC080);

  /* Check instruction timings for branching. */
  set_new_index(p_buf, 0x0080);
  emit_LDA(p_buf, k_imm, 0x00);
  emit_CYCLES_RESET(p_buf);
  emit_BNE(p_buf, -2);            /* Branch, not taken, 2 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 3);
  emit_CYCLES_RESET(p_buf);
  emit_BEQ(p_buf, 0);             /* Branch, taken, 3 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 4);
  emit_CYCLES_RESET(p_buf);
  emit_BEQ(p_buf, 0x69);          /* Branch, taken, page crossing, 4 cycles. */

  set_new_index(p_buf, 0x0100);
  /* This is the landing point for the BEQ above. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 5);
  emit_JMP(p_buf, k_abs, 0xC140);

  /* Check simple instruction timings that hit 1Mhz peripherals. */
  set_new_index(p_buf, 0x0140);
  emit_CYCLES_RESET(p_buf);
  emit_LDA(p_buf, k_abs, 0xFE4E); /* Read IER, odd cycle start, 5 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 6);
  emit_CYCLES_RESET(p_buf);
  emit_CYCLES(p_buf);
  emit_LDA(p_buf, k_abs, 0xFE4E); /* Read IER, even cycle start, 6 cycles. */
  emit_CYCLES(p_buf);
  emit_REQUIRE_EQ(p_buf, 8);
  emit_JMP(p_buf, k_abs, 0xC180);

  /* Check T1 timer tick values. */
  /* T1, latch (e.g.) 4, ticks 4... 3... 2... 1... 0... -1... 4... */
  set_new_index(p_buf, 0x0180);
  emit_LDA(p_buf, k_imm, 0x7F);
  emit_STA(p_buf, k_abs, 0xFE4E); /* Write IER, interrupts off. */
  emit_LDA(p_buf, k_imm, 0x06);
  emit_STA(p_buf, k_abs, 0xFE44); /* T1CL: 6. */
  emit_LDA(p_buf, k_imm, 0x00);
  emit_STA(p_buf, k_abs, 0xFE45); /* T1CH: 0, timer starts. */
  emit_LDA(p_buf, k_abs, 0xFE44); /* T1CL: should be 4. */
  emit_STA(p_buf, k_abs, 0x1000);
  emit_LDA(p_buf, k_abs, 0xFE44); /* T1CL: should be -1. */
  emit_STA(p_buf, k_abs, 0x1001);
  emit_LDA(p_buf, k_abs, 0xFE44); /* T1CL: should be 2. */
  emit_STA(p_buf, k_abs, 0x1002);

  emit_LDA(p_buf, k_abs, 0x1000);
  emit_REQUIRE_EQ(p_buf, 0x04);
  emit_LDA(p_buf, k_abs, 0x1001);
  emit_REQUIRE_EQ(p_buf, 0xFF);
  emit_LDA(p_buf, k_abs, 0x1002);
  emit_REQUIRE_EQ(p_buf, 0x02);
  emit_JMP(p_buf, k_abs, 0xC1C0);

  /* Check T2 timer tick values. */
  /* T2 ticks (e.g.) 4... 3... 2... 1... 0... FFFF (-1)... FFFE */
  set_new_index(p_buf, 0x01C0);
  emit_LDA(p_buf, k_imm, 0x7F);
  emit_STA(p_buf, k_abs, 0xFE4E); /* Write IER, interrupts off. */
  emit_LDA(p_buf, k_imm, 0x06);
  emit_STA(p_buf, k_abs, 0xFE48); /* T2CL: 6. */
  emit_LDA(p_buf, k_imm, 0x00);
  emit_STA(p_buf, k_abs, 0xFE49); /* T2CH: 0, timer starts. */
  emit_LDA(p_buf, k_abs, 0xFE48); /* T2CL: should be 4. */
  emit_STA(p_buf, k_abs, 0x1000);
  emit_LDA(p_buf, k_abs, 0xFE48); /* T2CL: should be -1 (0xFF) */
  emit_STA(p_buf, k_abs, 0x1001);
  emit_LDA(p_buf, k_abs, 0xFE48); /* T2CL: should be 0xFA */
  emit_STA(p_buf, k_abs, 0x1002);

  emit_LDA(p_buf, k_abs, 0x1000);
  emit_REQUIRE_EQ(p_buf, 0x04);
  emit_LDA(p_buf, k_abs, 0x1001);
  emit_REQUIRE_EQ(p_buf, 0xFF);
  emit_LDA(p_buf, k_abs, 0x1002);
  emit_REQUIRE_EQ(p_buf, 0xFA);
  emit_JMP(p_buf, k_abs, 0xC200);

  /* Check an interrupt fires immediately when T1 expires. */
  set_new_index(p_buf, 0x0200);
  emit_LDA(p_buf, k_imm, 0x00);
  emit_STA(p_buf, k_zpg, 0x10);   /* Clear IRQ count. */
  emit_LDX(p_buf, k_imm, 0x42);
  emit_LDA(p_buf, k_imm, 0x7F);
  emit_STA(p_buf, k_abs, 0xFE4E); /* Write IER, interrupts off. */
  emit_LDA(p_buf, k_imm, 0xC0);
  emit_STA(p_buf, k_abs, 0xFE4E); /* Write IER, TIMER1 interrupt on. */
  emit_LDA(p_buf, k_imm, 0x01);
  emit_STA(p_buf, k_abs, 0xFE44); /* T1CL: 1. */
  emit_LDA(p_buf, k_imm, 0x00);
  emit_STA(p_buf, k_abs, 0xFE45); /* T1CH: 0, timer starts, IFR cleared. */
  emit_CLI(p_buf);                /* 2 cycles. At timer value 1. */
  emit_INC(p_buf, k_zpg, 0x00);   /* 5 cycles. At timer value 0, -1. */
                                  /* Interrupt here. */
  emit_INX(p_buf);                /* Used to check if interrupt is late. */
  emit_SEI(p_buf);
  emit_LDA(p_buf, k_zpg, 0x10);
  emit_REQUIRE_EQ(p_buf, 0x01);
  emit_LDA(p_buf, k_zpg, 0x12);
  emit_REQUIRE_EQ(p_buf, 0x42);
  emit_JMP(p_buf, k_abs, 0xC240);

  set_new_index(p_buf, 0x0240);
  emit_LDA(p_buf, k_imm, 0xC2);
  emit_LDX(p_buf, k_imm, 0xC1);
  emit_LDY(p_buf, k_imm, 0xC0);
  emit_EXIT(p_buf);

  /* IRQ routine. */
  set_new_index(p_buf, 0x3F00);
  emit_INC(p_buf, k_zpg, 0x10);
  emit_STA(p_buf, k_zpg, 0x11);
  emit_STX(p_buf, k_zpg, 0x12);
  emit_STY(p_buf, k_zpg, 0x13);
  emit_LDA(p_buf, k_imm, 0x7F);
  emit_STA(p_buf, k_abs, 0xFE4E); /* Write IER, interrupts off. */
  emit_RTI(p_buf);

  fd = open("timing.rom", O_CREAT | O_WRONLY, 0600);
  if (fd < 0) {
    errx(1, "can't open output rom");
  }
  write_ret = write(fd, p_mem, k_rom_size);
  if (write_ret < 0) {
    errx(1, "can't write output rom");
  }
  if ((size_t) write_ret != k_rom_size) {
    errx(1, "can't write output rom");
  }
  close(fd);

  util_buffer_destroy(p_buf);
  free(p_mem);

  return 0;
}
