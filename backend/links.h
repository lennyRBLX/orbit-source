#ifndef ORBIT_LINKS_H
#define ORBIT_LINKS_H
#include "orbit.h"
/* Download links the user pastes, for files outside the catalogue (see links.c). */
#define ORBIT_MAX_LINKS 20
/* The request body allowance for POST /api/v1/links: 20 links of up to 2 KB each. */
#define ORBIT_LINKS_BODY (64 * 1024)
int links_start(void);
void links_stop(void);
bool links_busy(void);
/* {urls:[...], storageId}: 202 when checking has started; 400, 409 or 503 with a
 * message otherwise. Never call it under orbit.mutex. */
int links_request(const cJSON *input, char *error, size_t cap);
/* POST /api/v1/downloads/<id>/replace-link {url}: a fresh link for a paused or
 * failed link download, which keeps its partial file. Never under orbit.mutex. */
int links_replace_request(const char *job_id, const cJSON *input, char *error, size_t cap);
/* {id, state:"idle"|"checking"|"done", replaceJobId, items:[{host, filename, state,
 * message, jobId}]}. Never a link itself: only its host. */
cJSON *links_status(void);

/* Helpers, also used by the tests and the saved queue. */
/* An https link with a host, and no user name, password or #fragment. */
bool link_url_allowed(const char *url);
/* The host of a link, for display; "" when it has none. */
void link_host(const char *url, char *out, size_t cap);
/* A safe file name from the server's Content-Disposition, else the link's path. */
void link_filename(const char *disposition, const char *final_url, const char *url, char *out, size_t cap);
/* Fills compression, format and archive_unverified from the file name: one-part
 * ZIP, 7z and RAR archives unpack; parts of a split archive stay as they are. */
void link_kind(Release *release);
/* Whether a saved link download's release is one links.c could have made. */
bool link_release_valid(const Release *release);
#endif
