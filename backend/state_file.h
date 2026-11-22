#ifndef ORBIT_STATE_FILE_H
#define ORBIT_STATE_FILE_H
#include <stddef.h>
#include <stdint.h>
/* Replaces <directory>/state.json with data: a temporary file, fsync, rename
 * and a directory fsync, so a crash leaves the old or the new file whole.
 * Writes run one at a time and in snapshot order: generation numbers
 * snapshots in the order they were taken, and a snapshot older than the
 * newest one written is dropped, returning 0, because that file already holds
 * everything it had. Returns nonzero when this snapshot could not be saved.
 * Never takes orbit.mutex; callers may hold it, but nothing may take orbit.mutex
 * while inside (lock order: orbit.mutex, then this file's lock). */
int state_file_write(int directory, uint64_t generation, const char *data, size_t length);
/* Atomic replacement for an independently serialized file in a directory. */
int state_file_replace(int directory, const char *name, const char *temporary,
                       const char *data, size_t length);
#endif
