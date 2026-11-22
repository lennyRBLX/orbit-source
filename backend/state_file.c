/* state.json on disk. Kept apart from state.c so a download checkpoint can
 * write it without holding orbit.mutex (issue 185), and so the ordering and
 * crash-safety rules can be tested on their own (tests/state_file_driver.c). */
#ifdef ORBIT_DESKTOP
#define _POSIX_C_SOURCE 200809L
#endif
#include "state_file.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h> /* renameat */
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
/* save_lock: the newest snapshot renamed into state.json, and the newest
 * whose rename a directory fsync has made durable. */
static uint64_t renamed, durable;

#ifdef ORBIT_TEST
/* ORBIT_TEST_STATE_SAVE_MS: storage that takes this long to save state. */
static void slow_storage(void) {
    const char *value = getenv("ORBIT_TEST_STATE_SAVE_MS");
    long ms = value ? strtol(value, NULL, 10) : 0;
    if (ms <= 0 || ms > 10000) return;
    struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}
#endif

static int replace(int directory, const char *name, const char *temporary,
                   const char *data, size_t length) {
    int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t p = 0;
    while (p < length) {
        ssize_t w = write(fd, data + p, length - p);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        p += (size_t)w;
    }
    int rc = p == length ? fsync(fd) : -1;
    if (close(fd) != 0) rc = -1;
    if (rc == 0) rc = renameat(directory, temporary, directory, name);
    return rc;
}

int state_file_replace(int directory, const char *name, const char *temporary,
                       const char *data, size_t length) {
    if (replace(directory, name, temporary, data, length)) return -1;
    return fsync(directory);
}

int state_file_write(int directory, uint64_t generation, const char *data, size_t length) {
    int rc = -1;
    pthread_mutex_lock(&save_lock);
    if (generation <= durable) {
        rc = 0;
    } else if (generation <= renamed) {
        /* A newer file is in place but its rename may not be durable yet:
         * never put older content back, finish making the newer one durable. */
        rc = fsync(directory);
        if (rc == 0) durable = renamed;
    } else {
#ifdef ORBIT_TEST
        slow_storage();
#endif
        rc = replace(directory, "state.json", "state.tmp", data, length);
        if (rc == 0) {
            renamed = generation;
            rc = fsync(directory);
            if (rc == 0) durable = generation;
        }
    }
    pthread_mutex_unlock(&save_lock);
    return rc;
}
