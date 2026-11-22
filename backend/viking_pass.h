#ifndef ORBIT_VIKING_PASS_H
#define ORBIT_VIKING_PASS_H
#include "viking_session.h"
/* A Vikingfile sign-in pass pasted from another device: the PHPSESSID cookie
 * of a browser that signed in on vikingfile.com. For consoles whose browser
 * can't run Vikingfile's sign-in page. Vikingfile's sign-in form includes
 * Cloudflare's human check, so Orbit never signs in itself: the pass only
 * reuses a sign-in the user made in their own browser. It is kept on the
 * console (viking-pass.json, mode 0600) and never returned or logged. */
#define VIKING_PASS_CAP 257
void viking_pass_load(void);
void viking_pass_clear(void);
/* {saved, state:"signed-in"|"signed-out"|"unknown", busy}. Never the pass. */
cJSON *viking_pass_status(void);
/* "none" when no pass is saved, else the last check's result. */
const char *viking_pass_state(void);
/* {action:"save", pass}, {action:"check"} or {action:"forget"}. Saving and
 * checking wait for vikingfile.com on the calling thread; never call it under
 * orbit.mutex. */
int viking_pass_action(const cJSON *input, char *error, size_t cap);
/* For viking_session.c: the saved pass, and the result of a check that used it. */
bool viking_pass_value(char *out, size_t cap);
void viking_pass_note(const char *value, VikingState state);
/* Accepts "value", "PHPSESSID=value", quotes and a trailing ";". */
bool viking_pass_normalise(const char *raw, char *out, size_t cap);
#endif
