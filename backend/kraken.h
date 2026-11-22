#ifndef ORBIT_KRAKEN_H
#define ORBIT_KRAKEN_H
/* Kraken Mount (github.com/quer3q/kraken-mount): a payload that runs FPKG files and game
 * folders without installing them, in ShadowMount+'s place. Orbit only reads its loopback
 * web API and asks it to scan; it never changes Kraken Mount's settings or games. */
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>

int kraken_start(void);
void kraken_stop(void);
/* Kraken Mount answers and is adding games: not waiting for kstuff, and ShadowMount+ was
 * not running when it started (it waits until then). */
bool kraken_active(void);
/* Orbit has asked Kraken Mount at least once since it started, so kraken_active() is
 * Kraken Mount's answer and not just Orbit starting up. */
bool kraken_checked(void);
/* Its version while active, otherwise "". */
void kraken_version(char *out, size_t cap);
/* A copy of the games Kraken Mount listed last, or NULL while it isn't active:
 * [{titleId, title, version, path, kind: "pkg"|"folder", status, error, sizeBytes}].
 * status is Kraken Mount's: pending, checking, building, ready or error. */
cJSON *kraken_games(void);
/* Ask Kraken Mount to scan now, and read its list every few seconds for a while, so a
 * finished download shows up quickly. */
void kraken_scan_soon(void);
/* Kraken Mount only finds a game directly inside one of its scan folders: homebrew or
 * etaHEN/games on any drive, or the top of an M.2 or USB drive. root is a drive's mount
 * point, folder the folder below it ("" for the drive's top). */
bool kraken_scan_folder(const char *root, const char *folder);

#endif
