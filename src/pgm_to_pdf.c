/* Convert one or more raw 8-bit PGM pages to a US Letter PDF on macOS. */
#include <ApplicationServices/ApplicationServices.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
  TOKEN_CAPACITY = 64,
  BLANK_PIXEL_THRESHOLD = 245,
  BLANK_INK_PER_THOUSAND = 6,
  TEXT_STROKE_PIXEL_THRESHOLD = 180,
  TEXT_STROKE_MINIMUM_RUN = 4,
  TEXT_STROKE_MINIMUM_ROWS = 2,
};

struct pgm_image {
  size_t width;
  size_t height;
  size_t length;
  uint8_t *pixels;
};

struct byte_reader {
  const uint8_t *cursor;
  const uint8_t *end;
};

static void skip_layout(struct byte_reader *reader) {
  while (reader->cursor < reader->end) {
    if (isspace(*reader->cursor)) {
      ++reader->cursor;
    } else if (*reader->cursor == '#') {
      while (reader->cursor < reader->end && *reader->cursor != '\n') {
        ++reader->cursor;
      }
    } else {
      break;
    }
  }
}

static bool read_token(struct byte_reader *reader, char token[TOKEN_CAPACITY]) {
  skip_layout(reader);
  size_t length = 0;
  while (reader->cursor < reader->end && !isspace(*reader->cursor) &&
         *reader->cursor != '#') {
    if (length + 1 >= TOKEN_CAPACITY) {
      return false;
    }
    token[length++] = (char)*reader->cursor++;
  }
  token[length] = '\0';
  return length > 0;
}

static bool parse_size(const char *token, size_t *value) {
  char *end = NULL;
  errno = 0;
  unsigned long long parsed = strtoull(token, &end, 10);
  if (errno != 0 || end == token || *end != '\0' || parsed == 0 || parsed > SIZE_MAX) {
    return false;
  }
  *value = (size_t)parsed;
  return true;
}

static bool load_pgm(const char *path, struct pgm_image *image) {
  int descriptor = open(path, O_RDONLY);
  if (descriptor < 0) {
    fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
    return false;
  }

  struct stat metadata;
  if (fstat(descriptor, &metadata) != 0 || metadata.st_size <= 0 ||
      (uintmax_t)metadata.st_size > SIZE_MAX) {
    fprintf(stderr, "invalid PGM file size: %s\n", path);
    close(descriptor);
    return false;
  }

  size_t file_length = (size_t)metadata.st_size;
  uint8_t *file_data = malloc(file_length);
  if (file_data == NULL) {
    fprintf(stderr, "cannot allocate %zu bytes for %s\n", file_length, path);
    close(descriptor);
    return false;
  }

  size_t offset = 0;
  while (offset < file_length) {
    ssize_t count = read(descriptor, file_data + offset, file_length - offset);
    if (count > 0) {
      offset += (size_t)count;
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  int close_result = close(descriptor);
  if (offset != file_length || close_result != 0) {
    fprintf(stderr, "cannot read PGM file: %s\n", path);
    free(file_data);
    return false;
  }

  struct byte_reader reader = {.cursor = file_data, .end = file_data + file_length};
  char token[TOKEN_CAPACITY];
  size_t maximum = 0;
  bool header_ok = read_token(&reader, token) && strcmp(token, "P5") == 0 &&
                   read_token(&reader, token) && parse_size(token, &image->width) &&
                   read_token(&reader, token) && parse_size(token, &image->height) &&
                   read_token(&reader, token) && parse_size(token, &maximum) && maximum == 255 &&
                   reader.cursor < reader.end && isspace(*reader.cursor);
  if (!header_ok || image->width > SIZE_MAX / image->height) {
    fprintf(stderr, "unsupported PGM file: %s (expected raw 8-bit P5)\n", path);
    free(file_data);
    return false;
  }

  uint8_t separator = *reader.cursor++;
  if (separator == '\r' && reader.cursor < reader.end && *reader.cursor == '\n') {
    ++reader.cursor;
  }
  image->length = image->width * image->height;
  if ((size_t)(reader.end - reader.cursor) < image->length) {
    fprintf(stderr, "truncated PGM image: %s\n", path);
    free(file_data);
    return false;
  }
  image->pixels = malloc(image->length);
  if (image->pixels == NULL) {
    fprintf(stderr, "cannot allocate %zu bytes for %s\n", image->length, path);
    free(file_data);
    return false;
  }
  memcpy(image->pixels, reader.cursor, image->length);
  free(file_data);
  return true;
}

static void release_provider_data(void *info, const void *data, size_t size) {
  (void)info;
  (void)size;
  free((void *)data);
}

static CGImageRef create_cg_image(struct pgm_image *image) {
  CGDataProviderRef provider =
      CGDataProviderCreateWithData(NULL, image->pixels, image->length, release_provider_data);
  if (provider == NULL) {
    return NULL;
  }
  image->pixels = NULL;

  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceGray();
  if (color_space == NULL) {
    CGDataProviderRelease(provider);
    return NULL;
  }

  CGImageRef result = CGImageCreate(image->width, image->height, 8, 8, image->width, color_space,
                                    kCGImageAlphaNone, provider, NULL, false,
                                    kCGRenderingIntentDefault);
  CGColorSpaceRelease(color_space);
  CGDataProviderRelease(provider);
  return result;
}

static CGRect fitted_rectangle(size_t pixel_width, size_t pixel_height, CGRect page) {
  double width_scale = page.size.width / (double)pixel_width;
  double height_scale = page.size.height / (double)pixel_height;
  double scale = width_scale < height_scale ? width_scale : height_scale;
  double width = (double)pixel_width * scale;
  double height = (double)pixel_height * scale;
  return CGRectMake((page.size.width - width) / 2.0, (page.size.height - height) / 2.0, width,
                    height);
}

static bool is_near_blank_back(const struct pgm_image *image, size_t raw_side_number) {
  if (raw_side_number % 2 != 0) {
    return false;
  }

  size_t dark_limit = image->length / 1000 * BLANK_INK_PER_THOUSAND;
  size_t dark_pixels = 0;
  for (size_t index = 0; index < image->length; ++index) {
    if (image->pixels[index] < BLANK_PIXEL_THRESHOLD && ++dark_pixels > dark_limit) {
      return false;
    }
  }

  size_t rows_with_strokes = 0;
  for (size_t row = 0; row < image->height; ++row) {
    size_t run = 0;
    for (size_t column = 0; column < image->width; ++column) {
      uint8_t pixel = image->pixels[row * image->width + column];
      if (pixel < TEXT_STROKE_PIXEL_THRESHOLD) {
        ++run;
        if (run >= TEXT_STROKE_MINIMUM_RUN) {
          ++rows_with_strokes;
          break;
        }
      } else {
        run = 0;
      }
    }
    if (rows_with_strokes >= TEXT_STROKE_MINIMUM_ROWS) {
      return false;
    }
  }
  return true;
}

int main(int argc, char **argv) {
  bool drop_blank_backs = false;
  int output_index = 1;
  if (output_index < argc && strcmp(argv[output_index], "--drop-blank-backs") == 0) {
    drop_blank_backs = true;
    ++output_index;
  }
  if (argc - output_index < 2) {
    fprintf(stderr,
            "usage: %s [--drop-blank-backs] <output.pdf> <page-001.pgm> "
            "[page-002.pgm ...]\n",
            argv[0]);
    return 2;
  }

  CFURLRef output_url = CFURLCreateFromFileSystemRepresentation(
      NULL, (const UInt8 *)argv[output_index], (CFIndex)strlen(argv[output_index]), false);
  if (output_url == NULL) {
    fprintf(stderr, "invalid output path: %s\n", argv[output_index]);
    return 2;
  }

  CGRect media_box = CGRectMake(0.0, 0.0, 612.0, 792.0);
  CGContextRef context = CGPDFContextCreateWithURL(output_url, &media_box, NULL);
  CFRelease(output_url);
  if (context == NULL) {
    fprintf(stderr, "cannot create PDF: %s\n", argv[output_index]);
    return 3;
  }

  int result = 0;
  size_t pages_written = 0;
  size_t blank_backs_dropped = 0;
  int first_page_index = output_index + 1;
  for (int index = first_page_index; index < argc; ++index) {
    struct pgm_image image = {0};
    if (!load_pgm(argv[index], &image)) {
      result = 4;
      break;
    }
    size_t raw_side_number = (size_t)(index - first_page_index + 1);
    if (drop_blank_backs && is_near_blank_back(&image, raw_side_number)) {
      ++blank_backs_dropped;
      free(image.pixels);
      continue;
    }
    CGImageRef cg_image = create_cg_image(&image);
    free(image.pixels);
    if (cg_image == NULL) {
      fprintf(stderr, "cannot create image for %s\n", argv[index]);
      result = 4;
      break;
    }

    CGRect draw_rectangle = fitted_rectangle(image.width, image.height, media_box);
    CGPDFContextBeginPage(context, NULL);
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(context, draw_rectangle, cg_image);
    CGPDFContextEndPage(context);
    CGImageRelease(cg_image);
    ++pages_written;
  }

  CGPDFContextClose(context);
  CGContextRelease(context);
  if (result == 0 && pages_written == 0) {
    fprintf(stderr, "all captured sides were blank; no PDF pages were written\n");
    result = 4;
  }
  if (result != 0) {
    (void)remove(argv[output_index]);
  } else {
    printf("pdf_pages=%zu\n", pages_written);
    printf("blank_backs_dropped=%zu\n", blank_backs_dropped);
  }
  return result;
}
