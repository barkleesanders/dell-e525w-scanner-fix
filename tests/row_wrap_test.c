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

  /*
   * write_pgm computes `visible_width - offset` in size_t arithmetic, so an
   * offset at or past the row width wraps to a huge length and reads outside
   * the page buffer. The exact-value assertions above only cover that
   * incidentally -- they say nothing about a case added later. Assert the
   * contract itself across the whole cycle and both a supported and an
   * unsupported width.
   */
  for (uint32_t side = 1; side <= 64; ++side) {
    assert(dell_e525w_front_row_offset(side, 2550) < 2550);
    assert(dell_e525w_front_row_offset(side, 2480) == 0);
    assert(dell_e525w_front_row_offset(side, 10) < 10);
  }
  return 0;
}
