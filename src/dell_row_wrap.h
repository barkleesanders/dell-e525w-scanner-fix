#ifndef DELL_ROW_WRAP_H
#define DELL_ROW_WRAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Dell's A05 300-DPI grayscale ADF stream reproducibly rotates front-side
 * scan rows on a four-sheet cycle. Returning the byte offset here lets the
 * writer restore row order without resizing, interpolation, or pixel loss.
 * Back-side offsets have not been observed with nonblank test pages, so they
 * remain unchanged.
 */
static inline size_t dell_e525w_front_row_offset(uint32_t side_number, size_t width) {
  if (width != 2550 || side_number % 2 == 0) {
    return 0;
  }

  switch (side_number % 8) {
    case 3:
      return 2048;
    case 5:
      return 512;
    case 7:
      return 1536;
    default:
      return 0;
  }
}

#endif
