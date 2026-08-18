#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef void (*object_fn)(void *object);
typedef uint64_t (*open_ex_fn)(void *object, const char *address, int port, int scope_id);
typedef uint64_t (*close_fn)(void *object);
typedef int (*tcp_connect_fn)(const char *address, const char *service, int scope_id);

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

#define LOAD_FUNCTION(destination, library, symbol_name)                                  \
  do {                                                                                    \
    void *symbol_address = required_symbol(library, symbol_name);                          \
    _Static_assert(sizeof(destination) == sizeof(symbol_address), "function pointer size"); \
    memcpy(&destination, &symbol_address, sizeof(symbol_address));                         \
  } while (0)

static int plain_connect(const char *address) {
  int descriptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (descriptor < 0) {
    return -1;
  }
  struct sockaddr_in target = {
      .sin_len = sizeof(target),
      .sin_family = AF_INET,
      .sin_port = htons(23010),
  };
  if (inet_pton(AF_INET, address, &target.sin_addr) != 1 ||
      connect(descriptor, (const struct sockaddr *)&target, sizeof(target)) != 0) {
    int connect_errno = errno;
    close(descriptor);
    errno = connect_errno;
    return -1;
  }
  return descriptor;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <SWLLD.dylib> <ip-address>\n", argv[0]);
    return 2;
  }

  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (library == NULL) {
    fprintf(stderr, "cannot load Dell driver: %s\n", dlerror());
    return 2;
  }

  object_fn construct = NULL;
  tcp_connect_fn tcp_connect = NULL;
  open_ex_fn open_ex = NULL;
  close_fn close_device = NULL;
  object_fn destruct = NULL;
  LOAD_FUNCTION(construct, library, "_ZN10CNetDeviceC1Ev");
  LOAD_FUNCTION(tcp_connect, library, "_Z11tcp_connectPKcS0_i");
  LOAD_FUNCTION(open_ex, library, "_ZN10CNetDevice6OpenExEPcii");
  LOAD_FUNCTION(close_device, library, "_ZN10CNetDevice5CloseEv");
  LOAD_FUNCTION(destruct, library, "_ZN10CNetDeviceD1Ev");

  void *device = calloc(1, 0x1c0);
  if (device == NULL) {
    fprintf(stderr, "allocation failed\n");
    dlclose(library);
    return 2;
  }

  int plain_descriptor = plain_connect(argv[2]);
  int plain_errno = errno;
  printf("plain_connect=%d errno=%d (%s)\n", plain_descriptor, plain_errno,
         strerror(plain_errno));
  if (plain_descriptor >= 0) {
    close(plain_descriptor);
  }

  int socket_descriptor = tcp_connect(argv[2], "23010", -1000);
  int tcp_errno = errno;
  printf("tcp_connect=%d errno=%d (%s)\n", socket_descriptor, tcp_errno, strerror(tcp_errno));
  if (socket_descriptor >= 0) {
    close(socket_descriptor);
  }

  construct(device);
  uint64_t open_result = open_ex(device, argv[2], 23010, -1000);
  printf("open_ex=%" PRIu64 " low32=0x%08" PRIx32 "\n", open_result,
         (uint32_t)open_result);
  uint64_t close_result = close_device(device);
  printf("close=%" PRIu64 "\n", close_result);
  destruct(device);
  free(device);
  dlclose(library);
  return (uint32_t)open_result == 0 ? 0 : 1;
}
