#ifndef ORBIT_DEBRID_H
#define ORBIT_DEBRID_H
#include "orbit.h"
/* Only the backend sees account credentials or temporary CDN addresses. */
int debrid_start(void);
void debrid_stop(void);
cJSON *debrid_status(void);
int debrid_action(const cJSON *input, char *error, size_t cap);
/* Called under orbit.mutex; does not perform network requests. */
int debrid_prepare_locked(const Release *release, Job *job, char *error, size_t cap);
/* Suggested route only. Reported host limits never prevent an explicit TorBox request. */
bool debrid_default_locked(const Release *release);
bool debrid_link_covered_locked(const Release *release);
/* Runs on the download worker. 1 = ready, 0 = waiting, -1 = failed/stopped. */
int debrid_resolve(Job *job, const Release *route, Release *resolved, time_t *retry_at,
                   char *error, size_t cap);
bool debrid_transfer_url_allowed(const char *url);
#endif
