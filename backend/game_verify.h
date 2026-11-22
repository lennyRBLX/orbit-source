#ifndef ORBIT_GAME_VERIFY_H
#define ORBIT_GAME_VERIFY_H
#include "orbit.h"
/* root is an already-pinned game directory. Checks never execute game code,
 * follow links or alter files. Applying requires the exact preview token. */
cJSON *game_verify(int root, atomic_bool *cancel, char *error, size_t cap);
int game_repair(int root, const char *preview, atomic_bool *cancel,
                cJSON **report, char *error, size_t cap);
#endif
