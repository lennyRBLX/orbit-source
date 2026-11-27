#ifndef ORBIT_PACKAGES_H
#define ORBIT_PACKAGES_H
#include "orbit.h"
/* Receipts are created only after a download/receiver has published its files. */
void packages_init(void);
int packages_start(void);
void packages_stop(void);
void packages_verification_changed(const char *path, const cJSON *report);
void packages_completed(const Job *pin, const char *origin, const char *output, bool scan);
/* Get everything: install this download's packages once checked, after `after`'s download
 * is in place ("" for no wait). */
void packages_auto_install(const char *origin, const char *after);
cJSON *packages_for_download(const char *origin);
cJSON *packages_status(void);
cJSON *packages_remove_files(const cJSON *input, int *code, char *error, size_t cap);
int packages_stream(const cJSON *input, const char *peer, char *error, size_t cap);
int packages_action(const cJSON *input, char *error, size_t cap);
int packages_download_action(const char *origin, const char *action, char *error, size_t cap);
bool packages_path_busy(const char *path);
/* Free up space: installed packages still on a drive ([{receipt, package, title, name,
 * sizeBytes}]), and deleting one of them when it is unchanged. */
cJSON *packages_space_leftovers(const char *storage_id);
int packages_space_delete(const char *receipt, const char *package, int64_t *freed, char *error, size_t cap);
/* Files: install the package, game image or game folder at root/folder/output
 * (folder "." is the drive's top). */
int packages_install_path(const Job *pin, const char *folder, const char *output, const char *origin,
                          char *error, size_t cap);
/* ShadowMountPlus installation and loopback API. */
typedef struct {
    char state[24], message[256];
    uint64_t received, total;
    int error;
} PackageProgress;
bool package_platform_available(bool stream);
int package_platform_submit(const char *source, const char *name, int64_t size,
                            int64_t *job, char token[33], char *error, size_t cap);
int package_platform_poll(int64_t job, bool stream, const char *source, PackageProgress *result);
void package_platform_stream(const char *url, int64_t total, const char token[33]);
bool shadowmount_package_support(bool stream);
int shadowmount_port(void);
cJSON *shadowmount_request(int port, const char *route, const char *body, size_t bytes,
                           char *error, size_t cap);
#endif
