#pragma once
#include "cJSON.h"
#include <time.h>
/* The announcements in a feed that show now, for this version, with only the fields the apps
 * use (docs/announcements.md). Entries that break a rule are left out on their own. */
cJSON *announcements_parse(const char *json, time_t now, const char *version);
cJSON *announcements_status(void);
void announcements_stop(void);
