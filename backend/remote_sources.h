#ifndef ORBIT_REMOTE_SOURCES_H
#define ORBIT_REMOTE_SOURCES_H

#define PADRINO_SOURCE "padrino"
#define PSPKG_PS5_SOURCE "pspkg-ps5"
#define PSPKG_PS4_SOURCE "pspkg-ps4"
#define PADRINO_OPTION_LIMIT 16384

#include "orbit.h"

/* Startup loads the durable settings and cache before queue state is read. */
int remote_sources_load(void);
#ifdef ORBIT_TEST
cJSON *remote_pspkg_preview(const char *source_id, const char *url,
                            unsigned listing_limit, size_t game_limit);
cJSON *remote_pspkg_links_test(const char *html);
#endif
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
bool remote_link_host(const Release *release, char *host, size_t cap);
bool remote_next_mirror_locked(const Release *release, const char *after, Release *mirror);
bool remote_mirror_locked(const Release *release, const char *id, Release *mirror);
bool remote_mirror_file(const Release *selected, const Release *file);
bool remote_metadata_url(const char *url);
bool remote_url_filename(const char *url, char *name, size_t cap);
int remote_file_format(Release *release, const char *name);
int remote_file_detected_format(Release *release, const char *name, const char *suffix);
int direct_file_format(Release *release, const char *name);
bool direct_release_init(Release *release, const char *url);
int direct_release_parse(const cJSON *entry, Release *release);
bool download_file_magic(const Release *release, const unsigned char *sample, size_t length);
int remote_download_resolve(Release *release, Release *locator, atomic_bool *cancel,
                            char *error, size_t cap);
int remote_download_verify(Release *release, const char *url, atomic_bool *cancel,
                           char *error, size_t cap);
typedef struct {
    char filename[160];
    char format[24], compression[16];
    int64_t size;
    bool exact;
} RemoteFileMetadata;
bool remote_download_metadata(const Release *release, RemoteFileMetadata *metadata, atomic_bool *cancel);
cJSON *remote_metadata_json_locked(const char *ids);
int remote_release_parse(const cJSON *entry, Release *release);
int remote_catalog_parse(const char *body, size_t length, const char *source_id,
                         Release **releases, size_t *count, size_t *skipped);
int padrino_catalog_load(Release **releases, size_t *count, time_t *snapshot_at);
const cJSON *padrino_game(const char *title_id);
void padrino_catalog_clear(void);

#endif
