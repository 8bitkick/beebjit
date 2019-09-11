#include "keyboard.h"

#include "os_thread.h"
#include "timing.h"
#include "util.h"

#include <assert.h>
#include <err.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char* k_capture_header = "beebjit-capture";

enum {
  k_keyboard_state_flag_down = 1,
  k_keyboard_state_flag_pressed_not_released = 2,
  k_keyboard_state_flag_unconsumed_press = 4,
};

enum {
  k_keyboard_queue_size = 16,
};

enum {
  k_capture_header_size = 32,
};

struct keyboard_struct {
  struct timing_struct* p_timing;

  /* The OS thread populates the queue of key events and the BBC thread
   * empties it from time to time.
   */
  struct os_lock_struct* p_lock;
  uint8_t queue_key[k_keyboard_queue_size];
  uint8_t queue_isdown[k_keyboard_queue_size];
  uint8_t queue_pos;

  uint64_t capture_handle;
  uint64_t replay_handle;
  uint64_t replay_next_time;
  uint8_t replay_next_keys;

  uint8_t bbc_keys[16][16];
  uint8_t bbc_keys_count;
  uint8_t bbc_keys_count_col[16];
  uint8_t key_state[256];
  uint8_t alt_key_state[256];
};

struct keyboard_struct*
keyboard_create(struct timing_struct* p_timing) {
  struct keyboard_struct* p_keyboard = malloc(sizeof(struct keyboard_struct));
  if (p_keyboard == NULL) {
    errx(1, "cannot allocate keyboard_struct");
  }

  (void) memset(p_keyboard, '\0', sizeof(struct keyboard_struct));

  p_keyboard->p_timing = p_timing;
  p_keyboard->p_lock = os_lock_create();
  p_keyboard->queue_pos = 0;
  p_keyboard->capture_handle = 0;
  p_keyboard->replay_handle = 0;
  p_keyboard->replay_next_time = 0;
  p_keyboard->replay_next_keys = 0;

  return p_keyboard;
}

void
keyboard_destroy(struct keyboard_struct* p_keyboard) {
  if (p_keyboard->capture_handle) {
    util_file_handle_close(p_keyboard->capture_handle);
  }
  if (p_keyboard->replay_handle) {
    util_file_handle_close(p_keyboard->replay_handle);
  }
  os_lock_destroy(p_keyboard->p_lock);
  free(p_keyboard);
}

void
keyboard_set_capture_file_name(struct keyboard_struct* p_keyboard,
                               const char* p_name) {
  char buf[k_capture_header_size];

  p_keyboard->capture_handle = util_file_handle_open(p_name, 1, 1);

  (void) memset(buf, '\0', sizeof(buf));
  (void) memcpy(buf, k_capture_header, strlen(k_capture_header));
  util_file_handle_write(p_keyboard->capture_handle, buf, sizeof(buf));
}

static void
keyboard_read_replay_frame(struct keyboard_struct* p_keyboard) {
  uint64_t ret;
  uint64_t handle = p_keyboard->replay_handle;
  assert(handle);

  ret = util_file_handle_read(handle,
                              &p_keyboard->replay_next_time,
                              sizeof(p_keyboard->replay_next_time));
  if (ret == 0) {
    /* EOF. */
    util_file_handle_close(handle);
    p_keyboard->replay_handle = 0;
    return;
  }
  ret += util_file_handle_read(handle,
                               &p_keyboard->replay_next_keys,
                               sizeof(p_keyboard->replay_next_keys));
  if (ret != (sizeof(p_keyboard->replay_next_time) +
              sizeof(p_keyboard->replay_next_keys))) {
    errx(1, "corrupt replay file, truncated frame header");
  }
}

void
keyboard_set_replay_file_name(struct keyboard_struct* p_keyboard,
                              const char* p_name) {
  char buf[k_capture_header_size];
  uint64_t ret;
  uint64_t handle = util_file_handle_open(p_name, 0, 0);

  p_keyboard->replay_handle = handle;

  ret = util_file_handle_read(handle, buf, sizeof(buf));
  if (ret != sizeof(buf)) {
    errx(1, "capture file too short");
  }
  if (memcmp(buf, k_capture_header, strlen(k_capture_header))) {
    errx(1, "capture file has bad header");
  }
  keyboard_read_replay_frame(p_keyboard);
}

static void
keyboard_bbc_key_to_rowcol(uint8_t key, int32_t* p_row, int32_t* p_col) {
  int32_t row = -1;
  int32_t col = -1;
  switch (key) {
  case k_keyboard_key_escape:
    row = 7;
    col = 0;
    break;
  case '1':
    row = 3;
    col = 0;
    break;
  case '2':
    row = 3;
    col = 1;
    break;
  case '3':
    row = 1;
    col = 1;
    break;
  case '4':
    row = 1;
    col = 2;
    break;
  case '5':
    row = 1;
    col = 3;
    break;
  case '6':
    row = 3;
    col = 4;
    break;
  case '7':
    row = 2;
    col = 4;
    break;
  case '8':
    row = 1;
    col = 5;
    break;
  case '9':
    row = 2;
    col = 6;
    break;
  case '0':
    row = 2;
    col = 7;
    break;
  case '-':
    row = 1;
    col = 7;
    break;
  case '=': /* BBC ^ */
    row = 1;
    col = 8;
    break;
  case k_keyboard_key_backspace: /* BBC DELETE */
    row = 5;
    col = 9;
    break;
  case k_keyboard_key_tab:
    row = 6;
    col = 0;
    break;
  case 'Q':
    row = 1;
    col = 0;
    break;
  case 'W':
    row = 2;
    col = 1;
    break;
  case 'E':
    row = 2;
    col = 2;
    break;
  case 'R':
    row = 3;
    col = 3;
    break;
  case 'T':
    row = 2;
    col = 3;
    break;
  case 'Y':
    row = 4;
    col = 4;
    break;
  case 'U':
    row = 3;
    col = 5;
    break;
  case 'I':
    row = 2;
    col = 5;
    break;
  case 'O':
    row = 3;
    col = 6;
    break;
  case 'P':
    row = 3;
    col = 7;
    break;
  case '[': /* BBC @ */
    row = 4;
    col = 7;
    break;
  case ']': /* BBC [ */
    row = 3;
    col = 8;
    break;
  case k_keyboard_key_enter: /* BBC RETURN */
    row = 4;
    col = 9;
    break;
  case k_keyboard_key_ctrl_left:
    row = 0;
    col = 1;
    break;
  case 'A':
    row = 4;
    col = 1;
    break;
  case 'S':
    row = 5;
    col = 1;
    break;
  case 'D':
    row = 3;
    col = 2;
    break;
  case 'F':
    row = 4;
    col = 3;
    break;
  case 'G':
    row = 5;
    col = 3;
    break;
  case 'H':
    row = 5;
    col = 4;
    break;
  case 'J':
    row = 4;
    col = 5;
    break;
  case 'K':
    row = 4;
    col = 6;
    break;
  case 'L':
    row = 5;
    col = 6;
    break;
  case ';':
    row = 5;
    col = 7;
    break;
  case '\'': /* BBC : */
    row = 4;
    col = 8;
    break;
  case k_keyboard_key_shift_left:
    row = 0;
    col = 0;
    break;
  case '\\': /* BBC ] */
    row = 5;
    col = 8;
    break;
  case 'Z':
    row = 6;
    col = 1;
    break;
  case 'X':
    row = 4;
    col = 2;
    break;
  case 'C':
    row = 5;
    col = 2;
    break;
  case 'V':
    row = 6;
    col = 3;
    break;
  case 'B':
    row = 6;
    col = 4;
    break;
  case 'N':
    row = 5;
    col = 5;
    break;
  case 'M':
    row = 6;
    col = 5;
    break;
  case ',':
    row = 6;
    col = 6;
    break;
  case '.':
    row = 6;
    col = 7;
    break;
  case '/':
    row = 6;
    col = 8;
    break;
  case k_keyboard_key_shift_right:
    row = 0;
    col = 0;
    break;
  case ' ':
    row = 6;
    col = 2;
    break;
  case k_keyboard_key_caps_lock:
    row = 4;
    col = 0;
    break;
  case k_keyboard_key_f1:
    row = 7;
    col = 1;
    break;
  case k_keyboard_key_f2:
    row = 7;
    col = 2;
    break;
  case k_keyboard_key_f3:
    row = 7;
    col = 3;
    break;
  case k_keyboard_key_f4:
    row = 1;
    col = 4;
    break;
  case k_keyboard_key_f5:
    row = 7;
    col = 4;
    break;
  case k_keyboard_key_f6:
    row = 7;
    col = 5;
    break;
  case k_keyboard_key_f7:
    row = 1;
    col = 6;
    break;
  case k_keyboard_key_f8:
    row = 7;
    col = 6;
    break;
  case k_keyboard_key_f9:
    row = 7;
    col = 7;
    break;
  case k_keyboard_key_f0:
    row = 2;
    col = 0;
    break;
  case k_keyboard_key_arrow_up:
    row = 3;
    col = 9;
    break;
  case k_keyboard_key_arrow_left:
    row = 1;
    col = 9;
    break;
  case k_keyboard_key_arrow_right:
    row = 7;
    col = 9;
    break;
  case k_keyboard_key_arrow_down:
    row = 2;
    col = 9;
    break;
  default:
    break;
  }

  *p_row = row;
  *p_col = col;
}

int
keyboard_bbc_is_key_pressed(struct keyboard_struct* p_keyboard,
                            uint8_t row,
                            uint8_t col) {
  return p_keyboard->bbc_keys[row][col];
}

int
keyboard_bbc_is_key_column_pressed(struct keyboard_struct* p_keyboard,
                                   uint8_t col) {
  uint8_t count = p_keyboard->bbc_keys_count_col[col];
  return (count > 0);
}

int
keyboard_bbc_is_any_key_pressed(struct keyboard_struct* p_keyboard) {
  uint8_t count = p_keyboard->bbc_keys_count;
  return (count > 0);
}

int
keyboard_consume_key_press(struct keyboard_struct* p_keyboard, uint8_t key) {
  int ret = !!(p_keyboard->key_state[key] &
               k_keyboard_state_flag_unconsumed_press);
  p_keyboard->key_state[key] &= ~k_keyboard_state_flag_unconsumed_press;

  return ret;
}

int
keyboard_consume_alt_key_press(struct keyboard_struct* p_keyboard,
                               uint8_t key) {
  int ret = !!(p_keyboard->alt_key_state[key] &
               k_keyboard_state_flag_unconsumed_press);
  p_keyboard->alt_key_state[key] &= ~k_keyboard_state_flag_unconsumed_press;

  return ret;
}

static void
keyboard_put_key_in_queue(struct keyboard_struct* p_keyboard,
                          uint8_t key,
                          int is_down) {
  /* Called from the system thread.
   * Only the system thread puts keys in the queue and that's all it does.
   */
  os_lock_lock(p_keyboard->p_lock);

  if (p_keyboard->queue_pos == k_keyboard_queue_size) {
    printf("keyboard queue full");
    os_lock_unlock(p_keyboard->p_lock);
    return;
  }
  p_keyboard->queue_key[p_keyboard->queue_pos] = key;
  p_keyboard->queue_isdown[p_keyboard->queue_pos] = is_down;
  p_keyboard->queue_pos++;

  os_lock_unlock(p_keyboard->p_lock);
}

void
keyboard_system_key_pressed(struct keyboard_struct* p_keyboard, uint8_t key) {
  /* Called from the system thread. */
  keyboard_put_key_in_queue(p_keyboard, key, 1);
}

void
keyboard_system_key_released(struct keyboard_struct* p_keyboard, uint8_t key) {
  /* Called from the system thread. */
  keyboard_put_key_in_queue(p_keyboard, key, 0);
}

static void
keyboard_key_pressed(struct keyboard_struct* p_keyboard, uint8_t key) {
  int32_t row;
  int32_t col;

  p_keyboard->key_state[key] |= k_keyboard_state_flag_down;
  if (!(p_keyboard->key_state[key] &
        k_keyboard_state_flag_pressed_not_released)) {
    p_keyboard->key_state[key] |= (k_keyboard_state_flag_pressed_not_released |
                                   k_keyboard_state_flag_unconsumed_press);
  }

  if (p_keyboard->key_state[k_keyboard_key_alt_left] &
      k_keyboard_state_flag_down) {
    /* Alt + key combos are for the emulator shell only, not the BBC. */
    p_keyboard->alt_key_state[key] |= k_keyboard_state_flag_down;
    if (!(p_keyboard->alt_key_state[key] &
          k_keyboard_state_flag_pressed_not_released)) {
      p_keyboard->alt_key_state[key] |=
          (k_keyboard_state_flag_pressed_not_released |
           k_keyboard_state_flag_unconsumed_press);
    }
    return;
  }

  keyboard_bbc_key_to_rowcol(key, &row, &col);
  if (row == -1 && col == -1) {
    return;
  }
  assert(row >= 0);
  assert(row < 16);
  assert(col >= 0);
  assert(col < 16);
  if (p_keyboard->bbc_keys[row][col]) {
    return;
  }
  p_keyboard->bbc_keys[row][col] = 1;
  if (row == 0) {
    /* Row 0, notably including shift and ctrl, is not wired to interrupt. */
    return;
  }
  p_keyboard->bbc_keys_count_col[col]++;
  p_keyboard->bbc_keys_count++;
}

static void
keyboard_key_released(struct keyboard_struct* p_keyboard, uint8_t key) {
  int32_t row;
  int32_t col;
  int was_pressed;

  p_keyboard->key_state[key] &= ~(k_keyboard_state_flag_down |
                                  k_keyboard_state_flag_pressed_not_released);
  p_keyboard->alt_key_state[key] &=
      ~(k_keyboard_state_flag_down |
        k_keyboard_state_flag_pressed_not_released);

  keyboard_bbc_key_to_rowcol(key, &row, &col);
  if (row == -1 && col == -1) {
    return;
  }
  assert(row >= 0);
  assert(row < 16);
  assert(col >= 0);
  assert(col < 16);
  was_pressed = p_keyboard->bbc_keys[row][col];
  p_keyboard->bbc_keys[row][col] = 0;
  if (row == 0) {
    /* Row 0, notably including shift and ctrl, is not wired to interrupt. */
    return;
  }
  if (was_pressed) {
    assert(p_keyboard->bbc_keys_count_col[col] > 0);
    p_keyboard->bbc_keys_count_col[col]--;
    assert(p_keyboard->bbc_keys_count > 0);
    p_keyboard->bbc_keys_count--;
  }
}

void
keyboard_read_queue(struct keyboard_struct* p_keyboard) {
  /* Called from the BBC thread. */
  uint8_t keys[k_keyboard_queue_size];
  uint8_t isdown[k_keyboard_queue_size];
  uint8_t i;
  uint8_t num_keys = 0;
  uint64_t time = timing_get_total_timer_ticks(p_keyboard->p_timing);
  uint64_t replay_handle = p_keyboard->replay_handle;

  /* Where are the keys coming from -- a real keyboard or a replay? */
  if (replay_handle) {
    if (time >= p_keyboard->replay_next_time) {
      if (time > p_keyboard->replay_next_time) {
        errx(1, "incompatible replay timing");
      }
      num_keys = p_keyboard->replay_next_keys;
      uint64_t ret = util_file_handle_read(replay_handle, keys, num_keys);
      ret += util_file_handle_read(replay_handle, isdown, num_keys);
      if (ret != (num_keys * 2)) {
        errx(1, "replay file truncated");
      }
      /* This finishes with the replay handle if we're at the end. */
      keyboard_read_replay_frame(p_keyboard);
    }
  } else if (p_keyboard->queue_pos) {
    /* Checking p_keyboard->queue_pos unlocked should be safe as we'll recheck
     * any potential work with the lock.
     */
    os_lock_lock(p_keyboard->p_lock);

    num_keys = p_keyboard->queue_pos;
    (void) memcpy(keys, p_keyboard->queue_key, sizeof(keys));
    (void) memcpy(isdown, p_keyboard->queue_isdown, sizeof(isdown));

    p_keyboard->queue_pos = 0;

    os_lock_unlock(p_keyboard->p_lock);
  }

  if (!num_keys) {
    return;
  }

  if (p_keyboard->capture_handle) {
    util_file_handle_write(p_keyboard->capture_handle, &time, sizeof(time));
    util_file_handle_write(p_keyboard->capture_handle,
                           &num_keys,
                           sizeof(num_keys));
    util_file_handle_write(p_keyboard->capture_handle, keys, num_keys);
    util_file_handle_write(p_keyboard->capture_handle, isdown, num_keys);
  }

  for (i = 0; i < num_keys; ++i) {
    uint8_t key = keys[i];
    if (isdown[i]) {
      keyboard_key_pressed(p_keyboard, key);
    } else {
      keyboard_key_released(p_keyboard, key);
    }
  }
}
