#include "install.h"
#include <dlfcn.h>
#include <errno.h>
#include <ps5/kernel.h>
#include <stdint.h>
#include <stdio.h>
/* Register only Orbit's own icon. Never request a scan of other app entries. */
extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilTerminate(void);
typedef int (*install_title_fn)(const char *, const char *, void *);
static install_title_fn install_title;
static int appinst_active;
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
    registration_method = install_title ? "title-directory" : "unavailable";
    if (!install_title) {
        /* The optional launcher must not refresh unrelated home-screen entries.
         * Fail before creating/updating its files or initializing AppInst. The
         * running service and an already installed native app remain usable. */
        orbit_launcher_record_error("title-registration-unavailable", ENOSYS, 0);
        return -1;
    }
    result = sceAppInstUtilInitialize();
    if (result) {
        orbit_launcher_record_error("initialize-appinst", 0, result);
        return -1;
    }
    appinst_active = 1;
    return 0;
}

static int register_title(void) {
    int result = install_title("ORBT00001", "/user/app/", NULL);
    if (result)
        orbit_launcher_record_error("register-title", 0, result);
    printf("Orbit launcher registration (%s): 0x%x\n", registration_method, result);
    return result ? -1 : 0;
}

int orbit_launcher_ensure(int state_fd) {
    registration_method = "not-needed";
    int result = orbit_launcher_ensure_at(state_fd, "/user/app", prepare, register_title);
    /* Release AppInst even when an intervening filesystem operation fails. */
    if (appinst_active) {
        sceAppInstUtilTerminate();
        appinst_active = 0;
    }
    return result;
}
