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
/* Files: install the package, game image or game folder at root/folder/output
 * (folder "." is the drive's top). */
int packages_install_path(const Job *pin, const char *folder, const char *output, const char *origin,
                          char *error, size_t cap);
/* AppInstUtil boundary. Only the installation worker calls these functions. */
typedef struct {
    char state[24], message[256];
    uint64_t received, total;
    int error;
} PackageProgress;
bool package_platform_available(void);
int package_platform_submit(const char *path, const char *name, const char *content_id,
                            char result_id[49], char *error, size_t cap);
int package_platform_poll(const char *content_id, PackageProgress *result);
void package_platform_close(void);
#endif
