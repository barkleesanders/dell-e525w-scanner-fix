/*
 * Single-session loopback bridge for Dell's native scanner protocol.
 *
 * Recent macOS releases can deny unsigned helper binaries direct access to the
 * local network while allowing Apple's /usr/bin/nc. This process accepts one
 * loopback connection from the scan engine and relays it through /usr/bin/nc
 * to the printer's TCP 23010 service.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
  DELL_SCAN_PORT = 23010,
  RELAY_BUFFER_SIZE = 64 * 1024,
};

static void terminate_bridge(int signal_number) {
  (void)signal_number;
  _exit(128 + SIGTERM);
}

static bool write_all(int descriptor, const uint8_t *buffer, size_t length) {
  size_t written = 0;
  while (written < length) {
    ssize_t result = write(descriptor, buffer + written, length - written);
    if (result > 0) {
      written += (size_t)result;
    } else if (result < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

static int create_listener(void) {
  int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener < 0) {
    return -1;
  }
  int enabled = 1;
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  struct sockaddr_in address = {
      .sin_len = sizeof(address),
      .sin_family = AF_INET,
      .sin_port = htons(DELL_SCAN_PORT),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(listener, 1) != 0) {
    int bind_errno = errno;
    close(listener);
    errno = bind_errno;
    return -1;
  }
  return listener;
}

/*
 * Relay until the scan engine closes its end, which is what ends a session.
 * Byte counts are accumulated and reported once by the caller: logging every
 * relayed chunk emitted ~130 lines per page (8,519,680 bytes / 64 KiB), so a
 * 29-sheet batch produced over 7,000 lines that buried the real error when
 * dell-scan cat's this log on failure.
 */
static bool relay_session(int client, int *to_nc, int from_nc, uint64_t *to_printer_bytes,
                          uint64_t *from_printer_bytes) {
  uint8_t buffer[RELAY_BUFFER_SIZE];
  bool nc_open = true;

  for (;;) {
    struct pollfd descriptors[2] = {
        {.fd = client, .events = POLLIN},
        {.fd = nc_open ? from_nc : -1, .events = POLLIN},
    };
    int ready = poll(descriptors, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }

    if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      ssize_t count = read(client, buffer, sizeof(buffer));
      if (count > 0) {
        *to_printer_bytes += (uint64_t)count;
        if (!write_all(*to_nc, buffer, (size_t)count)) {
          return false;
        }
      } else if (count == 0) {
        /* Clean EOF from the scan engine ends the session successfully. */
        close(*to_nc);
        *to_nc = -1;
        return true;
      } else if (errno != EINTR) {
        /* A read error is a failed relay, not a completed one. Conflating it
         * with EOF reported a broken transfer to dell-scan as success. */
        return false;
      }
    }

    if (nc_open && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      ssize_t count = read(from_nc, buffer, sizeof(buffer));
      if (count > 0) {
        *from_printer_bytes += (uint64_t)count;
        if (!write_all(client, buffer, (size_t)count)) {
          return false;
        }
      } else if (count == 0) {
        nc_open = false;
        shutdown(client, SHUT_WR);
      } else if (errno != EINTR) {
        return false;
      }
    }
  }
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, terminate_bridge);
  signal(SIGTERM, terminate_bridge);

  if (argc != 4) {
    fprintf(stderr, "usage: %s <printer-ipv4-address> <to-printer-fifo> <from-printer-fifo>\n",
            argv[0]);
    return 2;
  }
  struct in_addr printer_address;
  if (inet_pton(AF_INET, argv[1], &printer_address) != 1) {
    fprintf(stderr, "invalid printer IPv4 address: %s\n", argv[1]);
    return 2;
  }

  int listener = create_listener();
  if (listener < 0) {
    fprintf(stderr, "cannot listen on 127.0.0.1:%d: %s\n", DELL_SCAN_PORT, strerror(errno));
    return 3;
  }

  int to_nc = -1;
  int from_nc = -1;
  to_nc = open(argv[2], O_WRONLY);
  if (to_nc < 0) {
    fprintf(stderr, "cannot open printer-input FIFO: %s\n", strerror(errno));
    close(listener);
    return 4;
  }
  from_nc = open(argv[3], O_RDONLY);
  if (from_nc < 0) {
    fprintf(stderr, "cannot open printer-output FIFO: %s\n", strerror(errno));
    close(to_nc);
    close(listener);
    return 4;
  }
  printf("bridge_ready=127.0.0.1:%d remote=%s:%d\n", DELL_SCAN_PORT, argv[1],
         DELL_SCAN_PORT);

  int client = accept(listener, NULL, NULL);
  close(listener);
  if (client < 0) {
    fprintf(stderr, "accept failed: %s\n", strerror(errno));
    close(to_nc);
    close(from_nc);
    return 5;
  }

  uint64_t to_printer_bytes = 0;
  uint64_t from_printer_bytes = 0;
  bool relay_ok = relay_session(client, &to_nc, from_nc, &to_printer_bytes, &from_printer_bytes);
  int relay_errno = errno;
  if (to_nc >= 0) {
    close(to_nc);
  }
  close(from_nc);
  close(client);

  fprintf(stderr, "bridge_to_printer_bytes=%llu bridge_from_printer_bytes=%llu\n",
          (unsigned long long)to_printer_bytes, (unsigned long long)from_printer_bytes);
  if (!relay_ok) {
    /* Report the transferred totals alongside the failure: a relay that moved
     * zero bytes from the printer is a different problem than one that stalled
     * mid-page, and the previous per-chunk log made that impossible to see. */
    fprintf(stderr, "bridge relay failed after %llu bytes from the printer: %s\n",
            (unsigned long long)from_printer_bytes, strerror(relay_errno));
    return 6;
  }
  return 0;
}
