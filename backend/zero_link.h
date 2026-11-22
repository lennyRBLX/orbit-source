#ifndef ORBIT_ZERO_LINK_H
#define ORBIT_ZERO_LINK_H
#include "orbit.h"
/* A download link Orbit Zero caught in its own provider window, for this console
 * to download itself (see zero_link.c). */
int zero_link_start(void);
void zero_link_stop(void);
bool zero_link_busy(void);
/* {releaseId, storageId, url}: 202 when the check has started; 400, 409 or 503 with
 * a message otherwise. Never call it under orbit.mutex. */
int zero_link_request(const cJSON *input, char *error, size_t cap);
/* {id, releaseId, state:"idle"|"checking"|"queued"|"failed", message, jobId}. Never the link. */
cJSON *zero_link_status(void);
#endif
