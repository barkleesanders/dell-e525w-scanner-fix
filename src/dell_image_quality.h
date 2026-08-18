#ifndef DELL_IMAGE_QUALITY_H
#define DELL_IMAGE_QUALITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The A05 driver can occasionally turn page content into long vertical
 * streaks. Flag only severe cases: more than two percent of the visible
 * columns must contain a dark run spanning at least forty percent of the
 * page. This is a warning, not a destructive filter.
 */
static inline bool dell_e525w_has_vertical_streaks(const uint8_t *page, size_t row_bytes,
                                                    size_t visible_width,
                                                    size_t visible_height,
                                                    size_t corrected_row_offset,
                                                    size_t *suspicious_columns_out) {
  const uint8_t dark_threshold = 220;
  size_t minimum_run = visible_height * 2 / 5;
  size_t column_margin = visible_width / 50;
  size_t suspicious_limit = visible_width / 50;
  size_t suspicious_columns = 0;

  if (page == NULL || row_bytes < visible_width || visible_width < 10 || visible_height < 10) {
    return false;
  }
  if (minimum_run == 0) {
    minimum_run = 1;
  }
  if (suspicious_limit == 0) {
    suspicious_limit = 1;
  }

  for (size_t column = column_margin; column + column_margin < visible_width; ++column) {
    size_t run = 0;
    size_t source_column = (column + corrected_row_offset) % visible_width;
    for (size_t row = 0; row < visible_height; ++row) {
      if (page[row * row_bytes + source_column] < dark_threshold) {
        ++run;
        if (run >= minimum_run) {
          ++suspicious_columns;
          break;
        }
      } else {
        run = 0;
      }
    }
  }

  if (suspicious_columns_out != NULL) {
    *suspicious_columns_out = suspicious_columns;
  }
  return suspicious_columns > suspicious_limit;
}

#endif
