/* A Vikingfile sign-in pass pasted from another device (see viking_pass.h). */
#include "viking_pass.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#define PASS_FILE "viking-pass.json"
/* Its own lock: downloads read the pass without taking orbit.mutex. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool busy;
    char value[VIKING_PASS_CAP];
    VikingState state;
} pass;

bool viking_pass_normalise(const char *raw, char *out, size_t cap) {
    if (!raw || !out || cap < 2) return false;
    const char *s = raw;
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n && s[n - 1] == ';') n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n > 10 && !strncasecmp(s, "PHPSESSID=", 10)) { s += 10; n -= 10; }
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }
    /* PHP session IDs: letters, digits, "," and "-". */
    if (n < 16 || n >= cap || n >= VIKING_PASS_CAP) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != ',' && s[i] != '-') return false;
    memcpy(out, s, n);
    out[n] = 0;
    return true;
}
static int persist(void) {
    if (!pass.value[0]) {
        if (unlinkat(orbit.state_fd, PASS_FILE, 0) && errno != ENOENT) return -1;
        return fsync(orbit.state_fd);
    }
    char json[VIKING_PASS_CAP + 64];
    int size = snprintf(json, sizeof json, "{\"schema\":1,\"session\":\"%s\"}", pass.value);
    if (size <= 0 || size >= (int)sizeof json) return -1;
    char name[64], random[17];
    random_hex(random, 8);
    snprintf(name, sizeof name, "viking-pass-%s.tmp", random);
    int fd = openat(orbit.state_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600), rc = -1;
    if (fd >= 0) {
        size_t done = 0;
        while (done < (size_t)size) {
            ssize_t n = write(fd, json + done, (size_t)size - done);
            if (n <= 0) break;
            done += (size_t)n;
        }
        if (done == (size_t)size && !fsync(fd)) rc = 0;
        if (close(fd)) rc = -1;
        if (!rc) rc = renameat(orbit.state_fd, name, orbit.state_fd, PASS_FILE);
        if (!rc) rc = fsync(orbit.state_fd);
        unlinkat(orbit.state_fd, name, 0);
    }
    OPENSSL_cleanse(json, sizeof json);
    return rc;
}
void viking_pass_load(void) {
    int fd = openat(orbit.state_fd, PASS_FILE, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat st;
    char buf[1024] = {0};
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size > 0 &&
        st.st_size < (off_t)sizeof buf && read(fd, buf, (size_t)st.st_size) == st.st_size) {
        cJSON *o = cJSON_Parse(buf);
        pthread_mutex_lock(&lock);
        if (json_int(o, "schema") != 1 || !viking_pass_normalise(json_text(o, "session"), pass.value, sizeof pass.value))
            OPENSSL_cleanse(pass.value, sizeof pass.value);
        /* Not checked since Orbit started: shown as unconfirmed until it is. */
        pass.state = VIKING_UNKNOWN;
        pthread_mutex_unlock(&lock);
        cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "session");
        if (v && v->valuestring) OPENSSL_cleanse(v->valuestring, strlen(v->valuestring));
        cJSON_Delete(o);
    }
    OPENSSL_cleanse(buf, sizeof buf);
    close(fd);
}
void viking_pass_clear(void) {
    pthread_mutex_lock(&lock);
    OPENSSL_cleanse(&pass, sizeof pass);
    pthread_mutex_unlock(&lock);
}
static const char *state_name(VikingState state) {
    return state == VIKING_SIGNED_IN ? "signed-in" : state == VIKING_SIGNED_OUT ? "signed-out" : "unknown";
}
const char *viking_pass_state(void) {
    pthread_mutex_lock(&lock);
    const char *name = pass.value[0] ? state_name(pass.state) : "none";
    pthread_mutex_unlock(&lock);
    return name;
}
cJSON *viking_pass_status(void) {
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&lock);
    cJSON_AddBoolToObject(o, "saved", pass.value[0] != 0);
    cJSON_AddStringToObject(o, "state", pass.value[0] ? state_name(pass.state) : "none");
    cJSON_AddBoolToObject(o, "busy", pass.busy);
    pthread_mutex_unlock(&lock);
    return o;
}
bool viking_pass_value(char *out, size_t cap) {
    pthread_mutex_lock(&lock);
    bool ok = pass.value[0] && strlen(pass.value) < cap;
    if (ok) memcpy(out, pass.value, strlen(pass.value) + 1);
    pthread_mutex_unlock(&lock);
    return ok;
}
void viking_pass_note(const char *value, VikingState state) {
    pthread_mutex_lock(&lock);
    /* A check of a pass that has since been replaced or forgotten says nothing. */
    if (pass.value[0] && value && !strcmp(value, pass.value)) pass.state = state;
    pthread_mutex_unlock(&lock);
}
static bool begin(char *error, size_t cap) {
    pthread_mutex_lock(&lock);
    bool busy = pass.busy;
    pass.busy = true;
    pthread_mutex_unlock(&lock);
    if (busy) copy_text(error, cap, "Wait for the Vikingfile pass check to finish.");
    return !busy;
}
static int unreachable(char *error, size_t cap) {
    copy_text(error, cap, "Orbit couldn't check the pass with Vikingfile. Check your connection and try again.");
    return 502;
}
int viking_pass_action(const cJSON *input, char *error, size_t cap) {
    const char *action = json_text(input, "action");
    if (!strcmp(action, "forget")) {
        pthread_mutex_lock(&lock);
        int code = 200;
        if (pass.busy) {
            copy_text(error, cap, "Wait for the Vikingfile pass check to finish.");
            code = 409;
        } else {
            char old[VIKING_PASS_CAP];
            memcpy(old, pass.value, sizeof old);
            OPENSSL_cleanse(pass.value, sizeof pass.value);
            if (persist()) {
                memcpy(pass.value, old, sizeof old);
                copy_text(error, cap, "Could not remove the saved Vikingfile pass.");
                code = 503;
            } else pass.state = VIKING_UNKNOWN;
            OPENSSL_cleanse(old, sizeof old);
        }
        pthread_mutex_unlock(&lock);
        return code;
    }
    if (!strcmp(action, "check")) {
        char value[VIKING_PASS_CAP];
        if (!viking_pass_value(value, sizeof value)) {
            copy_text(error, cap, "No Vikingfile pass is saved.");
            return 409;
        }
        if (!begin(error, cap)) { OPENSSL_cleanse(value, sizeof value); return 409; }
        VikingState state = viking_pass_probe(value, NULL);
        viking_pass_note(value, state);
        OPENSSL_cleanse(value, sizeof value);
        pthread_mutex_lock(&lock);
        pass.busy = false;
        pthread_mutex_unlock(&lock);
        return 200;
    }
    if (strcmp(action, "save")) {
        copy_text(error, cap, "Choose save, check or forget.");
        return 400;
    }
    char value[VIKING_PASS_CAP];
    if (!viking_pass_normalise(json_text(input, "pass"), value, sizeof value)) {
        copy_text(error, cap, "Paste the PHPSESSID value from a browser signed in to Vikingfile.");
        return 400;
    }
    if (!begin(error, cap)) { OPENSSL_cleanse(value, sizeof value); return 409; }
    /* Only a pass Vikingfile confirms as signed in replaces the saved one. */
    VikingState state = viking_pass_probe(value, NULL);
    int code = 200;
    pthread_mutex_lock(&lock);
    if (state == VIKING_SIGNED_OUT) {
        copy_text(error, cap, "Vikingfile didn't accept this pass. Sign in on vikingfile.com again, then copy a new one.");
        code = 409;
    } else if (state != VIKING_SIGNED_IN) {
        code = unreachable(error, cap);
    } else {
        char old[VIKING_PASS_CAP];
        VikingState old_state = pass.state;
        memcpy(old, pass.value, sizeof old);
        copy_text(pass.value, sizeof pass.value, value);
        pass.state = VIKING_SIGNED_IN;
        if (persist()) {
            memcpy(pass.value, old, sizeof old);
            pass.state = old_state;
            copy_text(error, cap, "Could not save the Vikingfile pass on this console.");
            code = 503;
        }
        OPENSSL_cleanse(old, sizeof old);
    }
    pass.busy = false;
    pthread_mutex_unlock(&lock);
    OPENSSL_cleanse(value, sizeof value);
    return code;
}
