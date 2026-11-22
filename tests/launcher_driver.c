#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif
#include "install.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int state, prepared, registered;
static const char *scenario;

static bool registration_missing(void) {
    struct stat st;
    return fstatat(state, "fixture-registration", &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT;
}

static int prepare(void) {
    prepared++;
    if (!strcmp(scenario, "unavailable")) {
        orbit_launcher_record_error("title-registration-unavailable", ENOSYS, 0);
        return -1;
    }
    return 0;
}

static int register_title(void) {
    registered++;
    if (!strcmp(scenario, "failed")) {
        orbit_launcher_record_error("register-title", 0, 7);
        return -1;
    }
    int fd = openat(state, "fixture-registration", O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 4);
    state = open(argv[1], O_RDONLY | O_DIRECTORY);
    assert(state >= 0);
    scenario = argv[3];
    int code = orbit_launcher_ensure_at(state, argv[2], registration_missing, prepare, register_title);
    printf("result %d %d %d\n%s\n", code, prepared, registered, orbit_launcher_last_error());
    close(state);
    return 0;
}
