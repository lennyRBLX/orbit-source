#ifndef ORBIT_BUNDLE_H
#define ORBIT_BUNDLE_H
#include "orbit.h"
/* Get everything: a game and its add-ons, added to Downloads one item at a time. */
int bundle_start(void);
void bundle_stop(void);
/* POST /api/v1/bundles: {"action":"start"|"cancel", ...}. Returns an HTTP status. */
int bundle_action(const cJSON *input, char *error, size_t cap);
cJSON *bundle_status(void);
#endif
