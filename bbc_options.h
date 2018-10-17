#ifndef BEEBJIT_BBC_OPTIONS_H
#define BEEBJIT_BBC_OPTIONS_H

#include <stddef.h>
#include <stdint.h>

struct bbc_options {
  /* External options. */
  const char* p_opt_flags;
  const char* p_log_flags;

  /* Internal options, callbacks, etc. */
  void* p_debug_callback_object;
  int (*debug_subsystem_active)(void* p);
  int (*debug_active_at_addr)(void* p, uint16_t addr);
  int (*debug_counter_at_addr)(void* p, uint16_t addr);
  size_t* (*debug_get_counter_ptr)(void* p);
  void* (*debug_callback)(void* p);
};

#endif /* BEEBJIT_BBC_OPTIONS_H */