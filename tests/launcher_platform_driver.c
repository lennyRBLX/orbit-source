#include "install.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <ps5/kernel.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef enum {
    NativeNid, NativeName, SymbolMissing, UserAlreadyActive, AppInstError, StaleRegistration,
    SystemRefresh, SystemRefreshError, SystemRefreshNoMetadata, SystemRefreshDelayed, SystemRefreshRegistered
} Scenario;
static Scenario scenario;
static int stage, error_code, waits;
static bool refreshed;

static int install_title(const char *title, const char *parent, void *options) {
    assert(stage++ == 2);
    assert(!strcmp(title, "ORBT00001") && !strcmp(parent, "/user/app/") && !options);
    return scenario == NativeName || scenario == StaleRegistration ? (int)0x80990002u : 0;
}

static int refresh_system(void *options) {
    assert(!options && stage++ == 2);
    refreshed = scenario == SystemRefresh;
    return scenario == SystemRefreshError ? 29 : 0;
}

int orbit_test_nanosleep(const struct timespec *delay, struct timespec *remaining) {
    assert(delay->tv_sec == 0 && delay->tv_nsec == 250000000 && !remaining);
    if (++waits == 3 && scenario == SystemRefreshDelayed) refreshed = true;
    return 0;
}

static int title_exists(const char *id, int *registered) {
    assert(!strcmp(id, "ORBT00001") && waits == 20);
    *registered = scenario == SystemRefreshRegistered;
    return 0;
}

int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle) {
    assert(pid == -1 && !strcmp(basename, "libSceAppInstUtil.sprx"));
    *handle = 2;
    return 0;
}

intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid) {
    assert(pid == -1 && handle == 2 && !strcmp(nid, "Wudg3Xe3heE"));
    return scenario == NativeName || scenario == SymbolMissing || scenario >= SystemRefresh ? 0 : (intptr_t)install_title;
}

void *orbit_test_dlsym(void *handle, const char *symbol) {
    assert(handle == RTLD_DEFAULT);
    if (scenario == NativeName && !strcmp(symbol, "sceAppInstUtilAppInstallTitleDir"))
        return (void *)install_title;
    if (scenario >= SystemRefresh && !strcmp(symbol, "sceAppInstUtilAppInstallAll"))
        return (void *)refresh_system;
    if (scenario >= SystemRefresh && !strcmp(symbol, "sceAppInstUtilAppExists"))
        return (void *)title_exists;
    return NULL;
}

int orbit_test_lstat(const char *path, struct stat *out) {
    if (!strcmp(path, "/user/appmeta")) {
        out->st_mode = S_IFDIR;
        return 0;
    }
    assert(!strcmp(path, "/user/appmeta/ORBT00001/param.json"));
    if (scenario == NativeName || refreshed) {
        out->st_mode = S_IFREG;
        out->st_size = 200;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

int sceUserServiceInitialize(void *options) {
    assert(!options && stage++ == 0);
    return scenario == UserAlreadyActive ? 1 : 0;
}

int sceAppInstUtilInitialize(void) {
    assert(stage++ == 1);
    return scenario == AppInstError ? 17 : 0;
}

int sceAppInstUtilTerminate(void) {
    assert(stage++ == 3);
    return 0;
}

int sceUserServiceTerminate(void) {
    assert(stage++ == (scenario == AppInstError ? 2 : 4));
    return 0;
}

void orbit_launcher_record_error(const char *step, int system_error, int platform_error) {
    if (scenario == SymbolMissing)
        assert(!strcmp(step, "title-registration-unavailable") && system_error == ENOSYS);
    else if (scenario == StaleRegistration)
        assert(!strcmp(step, "check-registered-title") && platform_error == (int)0x80990002u);
    else if (scenario == SystemRefreshError)
        assert(!strcmp(step, "refresh-system-apps") && platform_error == 29);
    else if (scenario == SystemRefreshNoMetadata)
        assert(!strcmp(step, "check-refreshed-title") && system_error == ENOENT);
    else
        assert(scenario == AppInstError && !strcmp(step, "initialize-appinst") && platform_error == 17);
    error_code++;
}

int orbit_launcher_ensure_at(int state, const char *parent, bool (*registration_missing)(void),
                             int (*prepare)(void), int (*register_title)(void)) {
    assert(state == 42 && !strcmp(parent, "/user/app"));
    assert(registration_missing() == (scenario != NativeName));
    if (prepare())
        return -1;
    return register_title() ? -1 : 1;
}

int main(void) {
    static const int final_stage[] = {5, 5, 0, 4, 3, 5, 5, 5, 5, 5, 5};
    for (scenario = NativeNid; scenario <= SystemRefreshRegistered; scenario++) {
        stage = error_code = waits = 0;
        refreshed = false;
        bool failed = scenario == SymbolMissing || scenario == AppInstError || scenario == StaleRegistration ||
                      scenario == SystemRefreshError || scenario == SystemRefreshNoMetadata;
        assert(orbit_launcher_ensure(42) == (failed ? -1 : 1));
        assert(stage == final_stage[scenario] && error_code == failed);
        assert(waits == (scenario == SystemRefreshDelayed ? 3 :
                         scenario == SystemRefreshNoMetadata || scenario == SystemRefreshRegistered ? 20 : 0));
        assert(!strcmp(orbit_launcher_registration_method(),
                       scenario == SymbolMissing ? "unavailable" : scenario >= SystemRefresh ? "system-refresh" : "title-directory"));
    }
}
