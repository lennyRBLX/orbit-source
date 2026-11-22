#ifndef ORBIT_FIRMWARE_H
#define ORBIT_FIRMWARE_H
#include "orbit.h"
#include <stddef.h>
#include <stdint.h>
bool firmware_known(uint32_t raw);
/* "12.60", or empty when the firmware is unknown. */
void firmware_text(uint32_t raw, char *out, size_t cap);
/* The newest firmware PS5 fake packages run on, as text. */
const char *firmware_package_limit(void);
/* The oldest firmware major version a filename names, or -1. */
int firmware_required(const char *name);
/* Adds requiredFirmware, maxFirmware and firmwareIssue to a catalogue entry for this console. */
void firmware_describe(cJSON *entry, uint32_t raw);
/* Read only bounded, regular sce_sys/param.json files below an extracted folder.
 * Returns the highest declared PS5 requirement, or zero when it is unknown. */
uint32_t firmware_extracted(int parent, const char *name);
/* Whether an extracted folder holds a backport: a fakelib or fakelib2 folder at any depth. */
bool firmware_backport(int parent, const char *name);
void firmware_describe_download(cJSON *entry, const Job *job, uint32_t raw);
#endif
