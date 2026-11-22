#ifndef ORBIT_UNRAR_H
#define ORBIT_UNRAR_H
#include "sevenzip.h"
/* RAR archives through RARLAB's UnRAR, with multithreaded unpacking. The same
 * contract as seven_inspect/seven_extract: UnRAR reads only fd, opens no paths
 * or volumes and writes nothing; every entry goes through the callbacks.
 * 0 success, 1 cancelled, -1 damaged/unsupported, -2 password missing/wrong. */
#ifdef __cplusplus
extern "C" {
#endif
int unrar_inspect(int fd, const char *password, const SevenCallbacks *callbacks, void *context,
                  char *error, size_t error_cap);
int unrar_extract(int fd, const char *password, unsigned threads, const SevenCallbacks *callbacks,
                  void *context, char *error, size_t error_cap);
/* Threads to unpack with: the online CPUs, at most 4, at least 1. */
unsigned unrar_threads(void);
#ifdef __cplusplus
}
#endif
#endif
