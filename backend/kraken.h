#ifndef ORBIT_KRAKEN_H
#define ORBIT_KRAKEN_H
/* Kraken Mount (github.com/quer3q/kraken-mount): a payload that runs FPKG files and game
 * folders without installing them, Orbit's only game mounter. Orbit only reads its loopback
 * web API and asks it to scan; it never changes Kraken Mount's settings or games. */
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>

int kraken_start(void);
void kraken_stop(void);
/* Kraken Mount answers and is adding games: its state reports no "waiting" (for kstuff,
 * or for another game mounter to stop). */
bool kraken_active(void);
/* Orbit has asked Kraken Mount at least once since it started, so kraken_active() is
 * Kraken Mount's answer and not just Orbit starting up. Also true when Orbit couldn't
 * start asking at all, so nothing waits for an answer that never comes. */
bool kraken_checked(void);
/* Kraken Mount answers but is waiting before it adds games (for kstuff, or for another
 * game mounter to stop): it adds them by itself once it starts. */
bool kraken_waiting(void);
/* Its version while active, otherwise "". */
void kraken_version(char *out, size_t cap);
/* A copy of the games Kraken Mount listed last, or NULL while it isn't active:
 * [{titleId, title, version, path, kind: "pkg"|"folder", status, error, sizeBytes}].
 * status is Kraken Mount's: pending, checking, building, ready, error, or duplicate (a
 * second copy of a title it already runs from another path; listed after the first). */
cJSON *kraken_games(void);
/* abs_path is, or is inside, a game (.pkg file or game folder) Kraken Mount listed last,
 * whatever its status; that status goes to status ("" when false). Files uses it to keep
 * a running game's files in place. */
bool kraken_path_in_use(const char *abs_path, char *status, size_t cap);
/* Ask Kraken Mount to scan now, and read its list every few seconds for a while, so a
 * finished download shows up quickly. */
void kraken_scan_soon(void);
/* Kraken Mount's default scan folders (kraken-mount src/storage.rs default_scan_roots):
 * homebrew or etaHEN/games on any drive, or the top of an M.2 or USB drive. A PS5 package
 * Orbit follows sits directly inside one. root is a drive's mount point, folder the folder
 * below it ("" for the drive's top). */
bool kraken_scan_folder(const char *root, const char *folder);
/* Kraken Mount finds a game folder (eboot.bin and sce_sys/param.json) directly inside
 * folder: a scan folder, or one folder below homebrew or etaHEN/games (src/scan.rs
 * candidates: never below a drive's top, never inside backports or a dot folder). */
bool kraken_scan_place(const char *root, const char *folder);

#endif
