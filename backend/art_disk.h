#ifndef ORBIT_ART_DISK_H
#define ORBIT_ART_DISK_H
#include <stdbool.h>
#include <stddef.h>

/* Optional, bounded artwork storage. Failure never prevents network loading. */
void art_disk_open(const char *state_dir);
void art_disk_close(void);
bool art_disk_load(const char *url, unsigned char **data, size_t *size);
void art_disk_save(const char *url, const unsigned char *data, size_t size);
void art_disk_key(const char *text, char out[65]);
#endif
