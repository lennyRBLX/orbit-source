#ifndef ORBIT_REMOTE_SOURCES_H
#define ORBIT_REMOTE_SOURCES_H

#include "orbit.h"

/* Startup loads the durable settings and cache before queue state is read. */
int remote_sources_load(void);
int remote_sources_start(void);
void remote_sources_stop(void);
void remote_sources_changed_locked(void);
void remote_sources_tick(void);
unsigned remote_sources_retry_after_locked(void);
bool remote_source_enabled_locked(const Release *release);
cJSON *remote_sources_json_locked(void);
cJSON *remote_sources_diagnostics_locked(void);
unsigned long remote_sources_generation_locked(void);
size_t remote_sources_entry_count_locked(void);
int remote_sources_change_locked(const char *method, const char *id, const cJSON *input,
                                 char *error, size_t cap);

/* Pegasus package adapter and validation of queued remote snapshots. */
int pegasus_entry_to_release(const cJSON *entry, const char *source_id, Release *release);
bool remote_page_pending(const Release *release);
const char *remote_browser_provider(const char *url);
bool remote_url_filename(const char *url, char *name, size_t cap);
int remote_file_format(Release *release, const char *name);
int remote_download_resolve(Release *release, Release *locator, atomic_bool *cancel,
                            char *error, size_t cap);
int remote_download_verify(Release *release, const char *url, atomic_bool *cancel,
                           char *error, size_t cap);
int remote_release_parse(const cJSON *entry, Release *release);
int remote_catalog_parse(const char *body, size_t length, const char *source_id,
                         Release **releases, size_t *count, size_t *skipped);

#endif
