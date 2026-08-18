#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t (*location_fn)(uint32_t location_id);
typedef uint64_t (*no_arg_fn)(void);
typedef uint64_t (*byte_out_fn)(uint8_t *value);

struct swlld_api {
  location_fn find_scanner_by_location_pull;
  no_arg_fn initialize_driver;
  no_arg_fn initialize_scanner;
  byte_out_fn get_adf_mode;
  byte_out_fn get_scanner_status;
  byte_out_fn check_scanner_ready;
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

static uint32_t parse_location_id(const char *value) {
  char *end = NULL;
  errno = 0;
  unsigned long parsed = strtoul(value, &end, 0);
  if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
    fprintf(stderr, "invalid USB location ID: %s\n", value);
    exit(2);
  }
  return (uint32_t)parsed;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  if (argc != 3) {
    fprintf(stderr, "usage: %s <SWLLD.dylib> <USB-location-id>\n", argv[0]);
    return 2;
  }

  uint32_t location_id = parse_location_id(argv[2]);
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
  LOAD_SYMBOL(initialize_driver, "InitializeDriver");
  LOAD_SYMBOL(initialize_scanner, "InitializeScanner");
  LOAD_SYMBOL(get_adf_mode, "GetADFMode");
  LOAD_SYMBOL(get_scanner_status, "GetScannerStatus");
  LOAD_SYMBOL(check_scanner_ready, "CheckScannerReady");
  LOAD_SYMBOL(terminate_driver, "TerminateDriver");
#undef LOAD_SYMBOL

  printf("library=%s\n", argv[1]);
  printf("usb_location_id=0x%08" PRIx32 "\n", location_id);

  fprintf(stderr, "calling=find_scanner_by_location_pull\n");
  uint64_t found = api.find_scanner_by_location_pull(location_id);
  printf("find_scanner=%" PRIu64 "\n", found);
  if ((found & 1U) == 0) {
    dlclose(library);
    return 10;
  }

  fprintf(stderr, "calling=initialize_driver\n");
  uint64_t driver_initialized = api.initialize_driver();
  fprintf(stderr, "calling=initialize_scanner\n");
  uint64_t scanner_initialized = api.initialize_scanner();
  printf("initialize_driver=%" PRIu64 "\n", driver_initialized);
  printf("initialize_scanner=%" PRIu64 "\n", scanner_initialized);

  uint8_t adf[2] = {0xff, 0xff};
  uint8_t scanner_status = 0xff;
  uint8_t ready = 0xff;
  fprintf(stderr, "calling=get_adf_mode\n");
  uint64_t adf_ok = api.get_adf_mode(adf);
  fprintf(stderr, "calling=get_scanner_status\n");
  uint64_t status_ok = api.get_scanner_status(&scanner_status);
  fprintf(stderr, "calling=check_scanner_ready\n");
  uint64_t ready_ok = api.check_scanner_ready(&ready);
  printf("get_adf_mode=%" PRIu64 " connect=%u status=%u\n", adf_ok, adf[0], adf[1]);
  printf("get_scanner_status=%" PRIu64 " status=%u\n", status_ok, scanner_status);
  printf("check_scanner_ready=%" PRIu64 " ready=%u\n", ready_ok, ready);

  fprintf(stderr, "calling=terminate_driver\n");
  uint64_t terminated = api.terminate_driver();
  printf("terminate_driver=%" PRIu64 "\n", terminated);
  dlclose(library);

  if ((driver_initialized & scanner_initialized & adf_ok & status_ok & ready_ok & terminated & 1U) ==
      0) {
    return 11;
  }
  return 0;
}
