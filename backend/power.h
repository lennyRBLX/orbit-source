#ifndef ORBIT_POWER_H
#define ORBIT_POWER_H
#include "orbit.h"
bool power_available(void);
cJSON *power_status(void);
int power_action(const cJSON *input, char *error, size_t cap);
int power_client_work(const cJSON *input, const char *owner, char *error, size_t cap);
void power_download_completed(void);
void power_installation_failed(void);
bool power_request_begin(void);
void power_request_end(void);
void power_tick(void);
void power_finish(void);
#endif
