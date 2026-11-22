#include "install.h"
#include <dlfcn.h>
#include <errno.h>
#include <ps5/kernel.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
/* Prefer registering Orbit's title. Some firmware only exposes the system app refresh. */
extern int sceUserServiceInitialize(void *);
extern int sceUserServiceTerminate(void);
extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilTerminate(void);
#ifndef ORBIT_LAUNCHER_PLATFORM_TEST
extern int sceAppInstUtilAppInstallAll(void *) __attribute__((weak));
#endif
typedef int (*install_title_fn)(const char *, const char *, void *);
typedef int (*install_all_fn)(void *);
static install_title_fn install_title;
static install_all_fn install_all;
static bool appinst_active, user_service_active;
static const char *registration_method = "not-needed";

const char *orbit_launcher_registration_method(void) {
    return registration_method;
}

static int prepare(void) {
    uint32_t handle = 0;
    int result = kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle);
    install_title = result ? NULL : (install_title_fn)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
    /* SDK v0.43 also retains the loaded modules' symbol tables in its runtime
     * linker. Use the exact NID there when the kernel lookup cannot resolve it.
     * A missing optional installer function must not prevent server startup. */
    if (!install_title)
        install_title = (install_title_fn)dlsym(RTLD_DEFAULT, "Wudg3Xe3heE");
    if (!install_title)
        install_title = (install_title_fn)dlsym(RTLD_DEFAULT, "sceAppInstUtilAppInstallTitleDir");
    install_all = NULL;
    if (!install_title) {
#ifndef ORBIT_LAUNCHER_PLATFORM_TEST
        install_all = sceAppInstUtilAppInstallAll;
#endif
        if (!install_all)
            install_all = (install_all_fn)dlsym(RTLD_DEFAULT, "sceAppInstUtilAppInstallAll");
    }
    registration_method = install_title ? "title-directory" : install_all ? "system-refresh" : "unavailable";
    if (!install_title && !install_all) {
        orbit_launcher_record_error("title-registration-unavailable", ENOSYS, 0);
        return -1;
    }
    /* App registration uses the current user. The user service may already
     * be initialized, in which case we must leave its lifetime to its caller. */
    result = sceUserServiceInitialize(NULL);
    user_service_active = result == 0;
    if (result)
        printf("Orbit launcher user service: 0x%x\n", result);
    result = sceAppInstUtilInitialize();
    if (result) {
        orbit_launcher_record_error("initialize-appinst", 0, result);
        return -1;
    }
    appinst_active = true;
    return 0;
}

static bool registration_missing(void) {
    /* The system creates this copy when a title is registered. The staged
     * files under /user/app cannot certify a Media entry. */
    struct stat metadata;
    if (!lstat("/user/appmeta/ORBT00001/param.json", &metadata))
        return !S_ISREG(metadata.st_mode) || metadata.st_size == 0;
    return errno == ENOENT && !lstat("/user/appmeta", &metadata) && S_ISDIR(metadata.st_mode);
}

static bool check_refreshed_title(void) {
    for (int attempt = 0; attempt < 20 && registration_missing(); attempt++) {
        struct timespec delay = {.tv_nsec = 250000000};
        nanosleep(&delay, NULL);
    }
    if (!registration_missing()) return true;
    /* Some firmware may register a deeplink without copying its metadata.
     * Query the title through AppInst when the filesystem check is inconclusive. */
    typedef int (*app_exists_fn)(const char *, int *);
    app_exists_fn exists = (app_exists_fn)dlsym(RTLD_DEFAULT, "sceAppInstUtilAppExists");
    if (!exists) return false;
    int registered = 0;
    int result = exists("ORBT00001", &registered);
    printf("Orbit launcher title status: 0x%08x, registered=%d\n", (unsigned)result, registered != 0);
    return !result && registered != 0;
}

#define APPINST_ALREADY_INSTALLED ((int)0x80990002u)

static int register_title(void) {
    int result = install_title ? install_title("ORBT00001", "/user/app/", NULL) : install_all(NULL);
    printf("Orbit launcher registration (%s): 0x%08x\n", registration_method, (unsigned)result);
    if (!result) {
        if (!install_title && !check_refreshed_title()) {
            orbit_launcher_record_error("check-refreshed-title", ENOENT, 0);
            return -1;
        }
        return 0;
    }
    if (install_title && result == APPINST_ALREADY_INSTALLED && !registration_missing())
        return 0;
    /* An existing database entry alone does not establish that its metadata
     * was restored. Preserve the native error so the next run retries. */
    const char *step = !install_title ? "refresh-system-apps" :
                       result == APPINST_ALREADY_INSTALLED ? "check-registered-title" : "register-title";
    orbit_launcher_record_error(step, 0, result);
    return -1;
}

int orbit_launcher_ensure(int state_fd) {
    registration_method = "not-needed";
    int result = orbit_launcher_ensure_at(state_fd, "/user/app", registration_missing, prepare, register_title);
    /* Release AppInst even when an intervening filesystem operation fails. */
    if (appinst_active) {
        sceAppInstUtilTerminate();
        appinst_active = false;
    }
    if (user_service_active) {
        sceUserServiceTerminate();
        user_service_active = false;
    }
    if (result > 0) printf("Orbit launcher ready (%s).\n", registration_method);
    return result;
}
