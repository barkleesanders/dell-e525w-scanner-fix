#include <assert.h>
#include <stddef.h>

#include "../src/dell_row_wrap.h"

int main(void) {
  assert(dell_e525w_front_row_offset(1, 2550) == 0);
  assert(dell_e525w_front_row_offset(3, 2550) == 2048);
  assert(dell_e525w_front_row_offset(5, 2550) == 512);
  assert(dell_e525w_front_row_offset(7, 2550) == 1536);
  assert(dell_e525w_front_row_offset(9, 2550) == 0);
  assert(dell_e525w_front_row_offset(2, 2550) == 0);
  assert(dell_e525w_front_row_offset(3, 1024) == 0);
  return 0;
}
