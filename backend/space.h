#ifndef ORBIT_SPACE_H
#define ORBIT_SPACE_H
#include "orbit.h"
/* Free up space (docs/api.md#free-up-space): what Orbit can delete on one drive to make
 * room, and deleting the items the user chose, one at a time, on a worker thread. */
#define SPACE_MAX_ITEMS 128
/* A delete request's body: every item's ID (under 112 bytes each), quoted, and the drive. */
#define SPACE_BODY (SPACE_MAX_ITEMS * 128 + 512)
/* POST /api/v1/space {"action":"list","storageId"}: the drive's space, Orbit's
 * leftovers and the games it can delete, each biggest first. NULL with *code set. */
cJSON *space_list(const char *storage_id, int *code, char *error, size_t cap);
/* POST /api/v1/space {"action":"delete","storageId","ids":[...]}: 202 when started. */
int space_delete(const cJSON *input, char *error, size_t cap);
/* GET /api/v1/space: the last deletion, {state: idle|deleting|done|failed, ...}. */
cJSON *space_status(void);
bool space_busy(void);
void space_stop(void);
#endif
