#include "catalog_view.h"
#include "firmware.h"
#include "links.h"
#include "orbit.h"
#include "packages.h"
#include "remote_sources.h"

#include <assert.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The fixture uses the real catalogue, URL policies and queue
 * snapshots. Console firmware, receipts and browser sessions are absent. */
bool orbit_is_stopping(void) { return orbit.stop; }
bool firmware_known(uint32_t raw) { return raw != 0; }
void firmware_describe(cJSON *row, uint32_t raw) { (void)row; (void)raw; }
void firmware_describe_download(cJSON *row, const Job *job, uint32_t raw) {
    (void)row; (void)job; (void)raw;
}
cJSON *packages_for_download(const char *origin) { (void)origin; return NULL; }
bool job_active(const Job *j) { return !strcmp(j->status, "downloading"); }
bool job_can_refresh_link(const Job *j) { (void)j; return false; }
const char *notify_event_name(unsigned index) { (void)index; return NULL; }
/* User-pasted links are covered by the full service integration test. */
bool link_release_valid(const Release *r) { (void)r; return false; }
bool link_url_allowed(const char *url) { (void)url; return false; }
void link_host(const char *url, char *out, size_t cap) { (void)url; if (cap) out[0] = 0; }
bool job_can_replace_link(const Job *j) { (void)j; return false; }

static atomic_llong scheduler_clock;
int64_t orbit_remote_test_clock(void) { return atomic_load(&scheduler_clock); }

static void print_json(cJSON *value) {
    assert(value);
    char *json = cJSON_PrintUnformatted(value);
    assert(json);
    puts(json);
    free(json);
    cJSON_Delete(value);
}

static bool equal_catalogue(const CatalogBody *a, const CatalogBody *b) {
    return catalog_body_size(a) == catalog_body_size(b) &&
           !memcmp(catalog_body_data(a), catalog_body_data(b), catalog_body_size(a));
}

static CatalogBody *view(bool addons) {
    bool gzip = false;
    CatalogBody *body = catalog_view_get(addons, &gzip);
    assert(body && !gzip);
    return body;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    orbit.state_fd = open(argv[1], O_RDONLY | O_DIRECTORY);
    assert(orbit.state_fd >= 0);
    assert(!curl_global_init(CURL_GLOBAL_DEFAULT));
    assert(!catalog_init());
    assert(!remote_sources_load());
    assert(!state_load());

    CatalogBody *base = view(false), *base_addons = view(true);
    const cJSON *first = cJSON_GetArrayItem(orbit.catalog, 0);
    printf("{\"titleId\":\"%s\",\"gameId\":\"%s\"}\n", json_text(first, "titleId"), json_text(first, "gameId"));
    fflush(stdout);
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        cJSON *request = cJSON_Parse(line);
        assert(request);
        const char *op = json_text(request, "op");
        if (!strcmp(op, "view")) {
            CatalogBody *body = view(false), *addons = view(true);
            const unsigned char *data = catalog_body_data(body);
            cJSON *result = cJSON_CreateObject(), *rows = cJSON_AddArrayToObject(result, "remote");
            cJSON_AddBoolToObject(result, "sameGames", equal_catalogue(base, body));
            cJSON_AddBoolToObject(result, "sameAddons", equal_catalogue(base_addons, addons));
            cJSON *catalogue = cJSON_ParseWithLength((const char *)data, catalog_body_size(body)), *row;
            assert(cJSON_IsArray(catalogue));
            const char *prefix = json_text(request, "sourcePrefix");
            size_t curated = 0;
            cJSON_ArrayForEach(row, catalogue) {
                if (strcmp(json_text(row, "sourceId"), "remote")) curated++;
                else if (!strncmp(json_text(row, "id"), prefix, strlen(prefix)))
                    cJSON_AddItemToArray(rows, cJSON_Duplicate(row, true));
            }
            cJSON_AddNumberToObject(result, "curatedCount", curated);
            print_json(result);
            cJSON_Delete(catalogue);
            catalog_body_release(addons);
            catalog_body_release(body);
        } else if (!strcmp(op, "start")) {
            printf("%d\n", remote_sources_start());
        } else if (!strcmp(op, "advance")) {
            atomic_fetch_add(&scheduler_clock, json_int(request, "milliseconds"));
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "tick"))) remote_sources_tick();
            puts("true");
        } else if (!strcmp(op, "provider-page")) {
            print_json(cJSON_CreateString(remote_browser_provider(json_text(request, "url"))));
        } else if (!strcmp(op, "metadata-page")) {
            Release release = {.remote_page = true};
            copy_text(release.browser_url, sizeof release.browser_url, json_text(request, "url"));
            RemoteFileMetadata file;
            cJSON *links = NULL;
            remote_download_metadata(&release, &file, &links);
            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "status", remote_metadata_status(file.status));
            cJSON_AddNumberToObject(result, "sizeBytes", (double)file.size);
            cJSON_AddBoolToObject(result, "exact", file.exact);
            if (links) cJSON_AddItemToObject(result, "links", links);
            print_json(result);
        } else if (!strcmp(op, "replay")) {
            pthread_mutex_lock(&orbit.mutex);
            print_json(remote_catalog_diagnostics_locked(cJSON_GetObjectItemCaseSensitive(request, "entry"),
                                                         json_text(request, "source")));
            pthread_mutex_unlock(&orbit.mutex);
        } else if (!strcmp(op, "pspkg-scan")) {
            print_json(remote_pspkg_preview(json_text(request, "source"), json_text(request, "url"),
                                            (size_t)json_int(request, "gameLimit")));
        } else if (!strcmp(op, "pspkg-links")) {
            print_json(remote_pspkg_links(json_text(request, "html")));
        } else if (!strcmp(op, "pspkg-job-page")) {
            Release release, saved;
            const char *source = json_text(request, "source");
            const cJSON *entry = cJSON_GetObjectItemCaseSensitive(request, "entry");
            assert(!pegasus_entry_to_release(entry, source, &release));
            copy_text(release.remote_provider_page, sizeof release.remote_provider_page, json_text(request, "url"));
            cJSON *snapshot = release_json(&release);
            bool valid = !remote_release_parse(snapshot, &saved);
            cJSON_AddBoolToObject(snapshot, "valid", valid);
            if (valid) cJSON_AddStringToObject(snapshot, "torboxUrl", provider_torbox_url(&saved));
            print_json(snapshot);
        } else if (!strcmp(op, "pspkg-candidate")) {
            Release release = {0};
            copy_text(release.source, sizeof release.source, "pspkg");
            copy_text(release.remote_catalog_id, sizeof release.remote_catalog_id, PSPKG_PS5_SOURCE);
            copy_text(release.title_id, sizeof release.title_id, "P5012345abcdef");
            release.remote_page = true;
            cJSON *answer = cJSON_CreateBool(provider_capture_url(&release, json_text(request, "url")));
            print_json(answer);
        } else if (!strcmp(op, "parse")) {
            const cJSON *feed = cJSON_GetObjectItemCaseSensitive(request, "feed");
            char *body = cJSON_PrintUnformatted(feed);
            Release *releases = NULL;
            size_t count = 0, skipped = 0;
            const char *source = json_text(request, "source");
            int code = body ? remote_catalog_parse(body, strlen(body), source,
                                                    &releases, &count, &skipped) : -1;
            cJSON *result = cJSON_CreateObject();
            cJSON_AddNumberToObject(result, "code", code);
            cJSON_AddNumberToObject(result, "skipped", skipped);
            cJSON *rows = cJSON_AddArrayToObject(result, "releases");
            for (size_t i = 0; !code && i < count; i++)
                cJSON_AddItemToArray(rows, release_json(&releases[i]));
            print_json(result);
            free(releases);
            free(body);
        } else if (!strcmp(op, "map") || !strcmp(op, "resolve")) {
            Release release, saved;
            const cJSON *row = cJSON_GetObjectItemCaseSensitive(request, "entry");
            const char *source = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "padrino"))
                ? PADRINO_SOURCE : "evox-coreos";
            int code = pegasus_entry_to_release(row, source, &release);
            if (!code && !strcmp(op, "resolve")) {
                Release locator;
                char error[256] = "";
                assert(remote_page_pending(&release));
                code = remote_download_resolve(&release, &locator, NULL, error, sizeof error);
            }
            if (code) printf("%d\n", code);
            else {
                cJSON *snapshot = release_json(&release);
                if (!remote_page_pending(&release)) {
                    assert(!remote_release_parse(snapshot, &saved));
                    assert(!memcmp(&release, &saved, sizeof release));
                }
                print_json(snapshot);
            }
        } else if (!strcmp(op, "save-job")) {
            pthread_mutex_lock(&orbit.mutex);
            const Release *release = release_find(json_text(request, "id"));
            assert(release);
            Job *j = &orbit.jobs[orbit.job_count++];
            j->release = *release;
            copy_text(j->id, sizeof j->id, "0123456789abcdef");
            copy_text(j->release_id, sizeof j->release_id, release->id);
            copy_text(j->status, sizeof j->status, "paused");
            copy_text(j->delivery, sizeof j->delivery, "direct");
            copy_text(j->filename, sizeof j->filename, release->filename);
            j->inspected_size = 41;
            copy_text(j->inspected_format, sizeof j->inspected_format, "Game folder");
            printf("%d\n", state_save_locked());
            pthread_mutex_unlock(&orbit.mutex);
        } else {
            pthread_mutex_lock(&orbit.mutex);
            if (!strcmp(op, "refresh")) {
                remote_sources_test_refresh_locked();
                puts("true");
            } else if (!strcmp(op, "selection")) {
                print_json(sources_json_locked());
            } else if (!strcmp(op, "show")) {
                print_json(remote_sources_test_status_locked());
            } else if (!strcmp(op, "jobs")) {
                print_json(jobs_json_locked());
            } else if (!strcmp(op, "metadata")) {
                print_json(remote_metadata_json_locked(json_text(request, "ids")));
            } else if (!strcmp(op, "diagnostics")) {
                print_json(remote_sources_diagnostics_locked());
            } else {
                assert(!"Unknown test command");
            }
            pthread_mutex_unlock(&orbit.mutex);
        }
        cJSON_Delete(request);
        fflush(stdout);
    }
    orbit.stop = true;
    remote_sources_stop();
    catalog_body_release(base);
    catalog_body_release(base_addons);
    catalog_view_clear();
    cJSON_Delete(orbit.catalog);
    cJSON_Delete(orbit.addons);
    close(orbit.state_fd);
    curl_global_cleanup();
    return 0;
}
