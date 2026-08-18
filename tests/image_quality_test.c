#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/dell_image_quality.h"

enum {
  WIDTH = 100,
  HEIGHT = 120,
};

int main(void) {
  uint8_t page[WIDTH * HEIGHT];
  size_t suspicious_columns = 0;

  memset(page, 0xff, sizeof(page));
  assert(!dell_e525w_has_vertical_streaks(page, WIDTH, WIDTH, HEIGHT, 0,
                                          &suspicious_columns));
  assert(suspicious_columns == 0);

  for (size_t row = 55; row < 60; ++row) {
    memset(page + row * WIDTH + 10, 0, 80);
  }
  assert(!dell_e525w_has_vertical_streaks(page, WIDTH, WIDTH, HEIGHT, 0,
                                          &suspicious_columns));

  memset(page, 0xff, sizeof(page));
  for (size_t column = 10; column < 15; ++column) {
    for (size_t row = 30; row < 100; ++row) {
      page[row * WIDTH + column] = 0;
    }
  }
  assert(dell_e525w_has_vertical_streaks(page, WIDTH, WIDTH, HEIGHT, 0,
                                         &suspicious_columns));
  assert(suspicious_columns == 5);

  puts("image_quality_tests=passed");
  return 0;
}
