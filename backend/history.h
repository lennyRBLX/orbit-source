#ifndef ORBIT_HISTORY_H
#define ORBIT_HISTORY_H
#include "orbit.h"
cJSON *history_status(void);
cJSON *history_preview(const cJSON *input, int *code, char *error, size_t cap);
int history_action(const cJSON *input, char *error, size_t cap);
void history_stop(void);
bool history_stopping(void);
#endif
