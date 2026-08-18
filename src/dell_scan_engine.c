/* Native USB/Wi-Fi ADF scanner client for the Dell Color MFP E525w on macOS. */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dell_image_quality.h"
#include "dell_row_wrap.h"

enum {
  COLOR_MODE_GRAYSCALE = 2,
  SCAN_METHOD_ADF = 1,
  LETTER_WIDTH_AT_300_DPI = 2550,
  LETTER_HEIGHT_AT_300_DPI = 3300,
  MAX_PAGE_BYTES = 256 * 1024 * 1024,
};

struct __attribute__((packed)) scan_parameter {
  uint16_t reserved;
  uint16_t x_dpi;
  uint16_t y_dpi;
  uint16_t left;
  uint16_t top;
  uint16_t width;
  uint16_t height;
  uint16_t pixel_num;
  uint16_t line_num;
  uint8_t color_mode;
  uint8_t scan_method;
  uint8_t driver_private[28];
};

_Static_assert(sizeof(struct scan_parameter) == 48, "Dell scan parameter must be 48 bytes");
_Static_assert(offsetof(struct scan_parameter, x_dpi) == 2, "unexpected x_dpi offset");
_Static_assert(offsetof(struct scan_parameter, pixel_num) == 14, "unexpected pixel_num offset");
_Static_assert(offsetof(struct scan_parameter, color_mode) == 18, "unexpected color_mode offset");

typedef uint64_t (*location_fn)(uint32_t location_id);
typedef uint64_t (*network_fn)(bool network_device, void *address);
typedef uint64_t (*no_arg_fn)(void);
typedef uint64_t (*byte_out_fn)(uint8_t *value);
typedef uint64_t (*scan_parameter_fn)(struct scan_parameter *parameter);
typedef uint64_t (*read_scan_fn)(uint8_t *unused, uint8_t *buffer, uint32_t capacity,
                                 uint32_t *bytes_read);

struct swlld_api {
  location_fn find_scanner_by_location_pull;
  network_fn find_scanner_ex;
  no_arg_fn initialize_driver;
  no_arg_fn initialize_scanner;
  byte_out_fn get_adf_mode;
  byte_out_fn get_scanner_status;
  scan_parameter_fn set_scan_parameter;
  scan_parameter_fn get_scan_parameter;
  no_arg_fn start_scan;
  read_scan_fn read_scan;
  no_arg_fn stop_scan;
  no_arg_fn cancel_scan;
  no_arg_fn terminate_driver;
};

static void *required_symbol(void *library, const char *name) {
  dlerror();
  void *symbol = dlsym(library, name);
  const char *error = dlerror();
  if (error != NULL) {
    fprintf(stderr, "missing Dell driver symbol %s: %s\n", name, error);
    exit(2);
  }
  return symbol;
}

static uint32_t parse_u32(const char *value, const char *label, uint32_t minimum,
                          uint32_t maximum) {
  char *end = NULL;
  errno = 0;
  unsigned long parsed = strtoul(value, &end, 0);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum || parsed > maximum) {
    fprintf(stderr, "invalid %s: %s\n", label, value);
    exit(2);
  }
  return (uint32_t)parsed;
}

static bool write_pgm(const char *prefix, uint32_t page_number, const uint8_t *page,
                      uint16_t row_bytes, uint16_t visible_width, uint16_t visible_height) {
  char path[4096];
  int path_length = snprintf(path, sizeof(path), "%s-page-%03" PRIu32 ".pgm", prefix,
                             page_number);
  if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
    fprintf(stderr, "output path is too long\n");
    return false;
  }

  int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (descriptor < 0) {
    fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
    return false;
  }

  FILE *output = fdopen(descriptor, "wb");
  if (output == NULL) {
    fprintf(stderr, "cannot open output stream for %s: %s\n", path, strerror(errno));
    close(descriptor);
    unlink(path);
    return false;
  }

  size_t row_offset = dell_e525w_front_row_offset(page_number, visible_width);
  bool ok = fprintf(output, "P5\n%u %u\n255\n", visible_width, visible_height) > 0;
  for (uint16_t row = 0; ok && row < visible_height; ++row) {
    const uint8_t *row_start = page + (size_t)row * row_bytes;
    size_t tail_length = (size_t)visible_width - row_offset;
    ok = fwrite(row_start + row_offset, 1, tail_length, output) == tail_length;
    if (ok && row_offset > 0) {
      ok = fwrite(row_start, 1, row_offset, output) == row_offset;
    }
  }
  if (fclose(output) != 0) {
    ok = false;
  }
  if (!ok) {
    fprintf(stderr, "failed while writing %s\n", path);
    unlink(path);
    return false;
  }

  if (row_offset > 0) {
    printf("row_wrap_offset=%zu\n", row_offset);
  }
  size_t suspicious_columns = 0;
  if (dell_e525w_has_vertical_streaks(page, row_bytes, visible_width, visible_height,
                                      row_offset, &suspicious_columns)) {
    fprintf(stderr,
            "quality_warning=vertical_streaks page=%" PRIu32
            " suspicious_columns=%zu action=rescan_this_side\n",
            page_number, suspicious_columns);
  }
  printf("output=%s\n", path);
  return true;
}

static bool read_one_page(const struct swlld_api *api, uint8_t *page, size_t page_size,
                          uint16_t row_bytes, size_t *bytes_transferred) {
  size_t total = 0;
  size_t last_reported_rows = 0;
  uint8_t unused = 0;
  *bytes_transferred = 0;

  while (total < page_size) {
    size_t remaining = page_size - total;
    size_t requested = (size_t)row_bytes * 32;
    if (requested > remaining) {
      requested = remaining;
    }
    if (requested > UINT32_MAX) {
      fprintf(stderr, "internal read size overflow\n");
      return false;
    }

    uint32_t bytes_read = 0;
    uint64_t read_ok = api->read_scan(&unused, page + total, (uint32_t)requested, &bytes_read);
    if ((read_ok & 1U) == 0 || bytes_read == 0 || bytes_read > requested) {
      uint8_t scanner_status = 0xff;
      uint64_t status_ok = api->get_scanner_status(&scanner_status);
      fprintf(stderr, "ReadScan stopped at %zu/%zu bytes (ok=%" PRIu64 ", read=%" PRIu32
                      ", status_ok=%" PRIu64 ", status=%u)\n",
              total, page_size, read_ok, bytes_read, status_ok, scanner_status);
      return false;
    }

    total += bytes_read;
    *bytes_transferred = total;
    size_t rows = total / row_bytes;
    if (rows >= last_reported_rows + 320 || total == page_size) {
      fprintf(stderr, "scan_progress=%zu/%zu_rows\n", rows, page_size / row_bytes);
      last_reported_rows = rows;
    }
  }
  return true;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  if (argc != 6) {
    fprintf(stderr,
            "usage: %s <SWLLD.dylib> <usb|wifi> <location-id|ip> <output-prefix> "
            "<max-pages>\n",
            argv[0]);
    return 2;
  }

  bool use_wifi = false;
  uint32_t location_id = 0;
  if (strcmp(argv[2], "usb") == 0) {
    location_id = parse_u32(argv[3], "USB location ID", 1, UINT32_MAX);
  } else if (strcmp(argv[2], "wifi") == 0) {
    struct in_addr ipv4;
    struct in6_addr ipv6;
    if (inet_pton(AF_INET, argv[3], &ipv4) != 1 && inet_pton(AF_INET6, argv[3], &ipv6) != 1) {
      fprintf(stderr, "invalid Wi-Fi scanner IP address: %s\n", argv[3]);
      return 2;
    }
    use_wifi = true;
  } else {
    fprintf(stderr, "invalid transport: %s (expected usb or wifi)\n", argv[2]);
    return 2;
  }
  uint32_t max_pages = parse_u32(argv[5], "page count", 1, 100);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (library == NULL) {
    fprintf(stderr, "cannot load Dell driver: %s\n", dlerror());
    return 2;
  }

  struct swlld_api api = {0};
#define LOAD_SYMBOL(field, symbol_name)                                                   \
  do {                                                                                    \
    void *symbol_address = required_symbol(library, symbol_name);                          \
    _Static_assert(sizeof(api.field) == sizeof(symbol_address), "function pointer size"); \
    memcpy(&api.field, &symbol_address, sizeof(symbol_address));                           \
  } while (0)
  LOAD_SYMBOL(find_scanner_by_location_pull, "FindScannerByLocation_pull");
  LOAD_SYMBOL(find_scanner_ex, "FindScannerEx");
  LOAD_SYMBOL(initialize_driver, "InitializeDriver");
  LOAD_SYMBOL(initialize_scanner, "InitializeScanner");
  LOAD_SYMBOL(get_adf_mode, "GetADFMode");
  LOAD_SYMBOL(get_scanner_status, "GetScannerStatus");
  LOAD_SYMBOL(set_scan_parameter, "SetScanParameter");
  LOAD_SYMBOL(get_scan_parameter, "GetScanParameter");
  LOAD_SYMBOL(start_scan, "StartScan");
  LOAD_SYMBOL(read_scan, "ReadScan");
  LOAD_SYMBOL(stop_scan, "StopScan");
  LOAD_SYMBOL(cancel_scan, "CancelScan");
  LOAD_SYMBOL(terminate_driver, "TerminateDriver");
#undef LOAD_SYMBOL

  int result = 1;
  bool driver_active = false;
  bool scan_active = false;
  uint8_t *page = NULL;

  printf("library=%s\n", argv[1]);
  if (use_wifi) {
    printf("transport=wifi ip=%s\n", argv[3]);
  } else {
    printf("transport=usb location_id=0x%08" PRIx32 "\n", location_id);
  }
  printf("scan=letter,300dpi,grayscale,adf max_pages=%" PRIu32 "\n", max_pages);

  uint64_t found = use_wifi ? api.find_scanner_ex(true, argv[3])
                            : api.find_scanner_by_location_pull(location_id);
  if ((found & 1U) == 0) {
    fprintf(stderr, "Dell driver could not find the scanner over %s\n",
            use_wifi ? "Wi-Fi" : "USB");
    result = 10;
    goto cleanup;
  }
  if ((api.initialize_driver() & 1U) == 0) {
    fprintf(stderr, "Dell driver initialization failed\n");
    result = 11;
    goto cleanup;
  }
  driver_active = true;
  if ((api.initialize_scanner() & 1U) == 0) {
    fprintf(stderr, "Dell scanner initialization failed\n");
    result = 12;
    goto cleanup;
  }

  uint8_t adf[2] = {0, 0};
  if ((api.get_adf_mode(adf) & 1U) == 0 || adf[0] != 1 || adf[1] != 1) {
    fprintf(stderr, "ADF is not ready (connected=%u, paper_loaded=%u)\n", adf[0], adf[1]);
    result = 13;
    goto cleanup;
  }
  printf("adf_connected=%u paper_loaded=%u\n", adf[0], adf[1]);

  struct scan_parameter parameter = {
      .x_dpi = 300,
      .y_dpi = 300,
      .width = LETTER_WIDTH_AT_300_DPI,
      .height = LETTER_HEIGHT_AT_300_DPI,
      .color_mode = COLOR_MODE_GRAYSCALE,
      .scan_method = SCAN_METHOD_ADF,
  };
  if ((api.set_scan_parameter(&parameter) & 1U) == 0 ||
      (api.get_scan_parameter(&parameter) & 1U) == 0) {
    fprintf(stderr, "Dell driver rejected the scan parameters\n");
    result = 14;
    goto cleanup;
  }

  if (parameter.pixel_num < parameter.width || parameter.line_num < parameter.height) {
    fprintf(stderr, "driver returned invalid page geometry: %ux%u for requested %ux%u\n",
            parameter.pixel_num, parameter.line_num, parameter.width, parameter.height);
    result = 15;
    goto cleanup;
  }
  size_t page_size = (size_t)parameter.pixel_num * parameter.line_num;
  if (page_size == 0 || page_size > MAX_PAGE_BYTES) {
    fprintf(stderr, "driver returned unsafe page allocation size: %zu\n", page_size);
    result = 15;
    goto cleanup;
  }
  printf("driver_geometry=%ux%u visible_geometry=%ux%u bytes_per_page=%zu\n",
         parameter.pixel_num, parameter.line_num, parameter.width, parameter.height, page_size);

  page = malloc(page_size);
  if (page == NULL) {
    fprintf(stderr, "cannot allocate %zu-byte page buffer\n", page_size);
    result = 16;
    goto cleanup;
  }

  for (uint32_t page_number = 1; page_number <= max_pages; ++page_number) {
    memset(page, 0xff, page_size);
    fprintf(stderr, "starting_page=%" PRIu32 "\n", page_number);
    if ((api.start_scan() & 1U) == 0) {
      if (page_number == 1) {
        fprintf(stderr, "StartScan failed before the first page\n");
        result = 17;
      } else {
        printf("pages_complete=%" PRIu32 "\n", page_number - 1);
        result = 0;
      }
      goto cleanup;
    }
    scan_active = true;

    size_t bytes_transferred = 0;
    if (!read_one_page(&api, page, page_size, parameter.pixel_num, &bytes_transferred)) {
      if (bytes_transferred > 0) {
        fprintf(stderr,
                "incomplete_side_discarded=%" PRIu32 " bytes=%zu/%zu "
                "action=keep_completed_sides_and_use_a_new_output_for_remaining_sheets\n",
                page_number, bytes_transferred, page_size);
      }
      if (page_number == 1) {
        result = 18;
      } else {
        printf("pages_complete=%" PRIu32 "\n", page_number - 1);
        result = 0;
      }
      goto cleanup;
    }
    if (!write_pgm(argv[4], page_number, page, parameter.pixel_num, parameter.width,
                   parameter.height)) {
      result = 19;
      goto cleanup;
    }
    printf("page_complete=%" PRIu32 "\n", page_number);

    if (page_number < max_pages) {
      adf[0] = 0;
      adf[1] = 0;
      (void)api.get_adf_mode(adf);
      adf[0] = 0;
      adf[1] = 0;
      (void)api.get_adf_mode(adf);
    }
  }

  printf("pages_complete=%" PRIu32 "\n", max_pages);
  result = 0;

cleanup:
  if (scan_active) {
    uint64_t cancel_result = api.cancel_scan();
    fprintf(stderr, "cancel_scan=%" PRIu64 "\n", cancel_result);
    if ((cancel_result & 1U) == 0) {
      uint64_t stop_result = api.stop_scan();
      fprintf(stderr, "stop_scan=%" PRIu64 "\n", stop_result);
    }
  }
  if (driver_active) {
    uint64_t terminate_result = api.terminate_driver();
    fprintf(stderr, "terminate_driver=%" PRIu64 "\n", terminate_result);
    if ((terminate_result & 1U) == 0 && result == 0) {
      result = 20;
    }
  }
  free(page);
  dlclose(library);
  return result;
}
