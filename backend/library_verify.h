#ifndef ORBIT_LIBRARY_VERIFY_H
#define ORBIT_LIBRARY_VERIFY_H
#include "orbit.h"
#include <sys/stat.h>
typedef void (*LibraryVerifyProgress)(int64_t bytes, void *context);
/* Exact read-back comparison, no symlinks or cross-device traversal. Paths are
 * resolved only beneath currently attached Orbit storage roots. */
int library_verify_copy(const char *source, const char *destination, dev_t source_device,
                        ino_t source_inode, atomic_bool *cancel, LibraryVerifyProgress progress,
                        void *context, char *error, size_t cap);
int library_source_identity(const char *path, struct stat *identity);
bool library_source_unchanged(const char *path, const struct stat *expected);
#endif
