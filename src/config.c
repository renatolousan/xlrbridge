/*
 * xlrbridge — config persistence implementation.
 *
 * Hand-rolled, dependency-free. The on-disk format is a minimal flat JSON
 * object (the file is named config.json):
 *
 *   {
 *     "interface_uid": "AppleUSBAudioEngine:Topping:E2x2:...",
 *     "in_channel": 0,
 *     "gain_db": 2.0,
 *     "blackhole_uid": "BlackHole2ch_UID"
 *   }
 *
 * The parser is intentionally tiny and forgiving: it scans for each known key
 * as a quoted string and reads the value token after the following ':'. It
 * does NOT implement general JSON — only this flat shape — but tolerates
 * arbitrary whitespace, key ordering, missing keys (defaults kept), a trailing
 * newline, and \" / \\ escapes inside string values (which is all our serializer
 * ever emits). Anything it can't make sense of falls back to the default.
 */

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

void xb_config_defaults(xb_config *cfg) {
    cfg->interface_uid[0] = '\0';
    cfg->in_channel       = XB_CONFIG_DEFAULT_IN_CHANNEL;
    cfg->gain_db          = XB_CONFIG_DEFAULT_GAIN_DB;
    snprintf(cfg->blackhole_uid, sizeof(cfg->blackhole_uid), "%s",
             XB_CONFIG_DEFAULT_BLACKHOLE_UID);
}

int xb_config_path(char *buf, size_t bufsz) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return -1;
    }
    int written = snprintf(buf, bufsz, "%s/.config/xlrbridge/config.json", home);
    if (written < 0 || (size_t)written >= bufsz) {
        return -1;
    }
    return 0;
}

/* Resolve the directory holding the config (for mkdir -p). */
static int xb_config_dir(char *buf, size_t bufsz) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return -1;
    }
    int written = snprintf(buf, bufsz, "%s/.config/xlrbridge", home);
    if (written < 0 || (size_t)written >= bufsz) {
        return -1;
    }
    return 0;
}

/* mkdir -p: create every component of `path` that's missing. */
static int mkdir_p(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1);
    /* Strip a trailing slash so the loop doesn't try to mkdir "". */
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/*
 * Find the value token for `key` in the JSON text `buf`.
 *
 * Looks for the substring "key" (with quotes), then the next ':', then the
 * first non-space value char. If the value is a quoted string, copies the
 * unescaped contents into out (size outsz) and sets *is_string=1. Otherwise
 * copies the bare token (number/literal, stopped at , } whitespace) and sets
 * *is_string=0. Returns 1 if the key was found, 0 otherwise.
 */
static int json_find_value(const char *buf, const char *key,
                           char *out, size_t outsz, int *is_string) {
    char needle[280];
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    const char *p = strstr(buf, needle);
    if (p == NULL) {
        return 0;
    }
    p += strlen(needle);

    /* Advance to ':'. */
    while (*p && *p != ':') {
        p++;
    }
    if (*p != ':') {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }

    size_t o = 0;
    if (*p == '"') {
        /* Quoted string: copy until the closing quote, honoring \" and \\. */
        p++;
        *is_string = 1;
        while (*p && *p != '"') {
            char c = *p;
            if (c == '\\' && p[1] != '\0') {
                p++;
                c = *p;
            }
            if (o + 1 < outsz) {
                out[o++] = c;
            }
            p++;
        }
        out[o < outsz ? o : outsz - 1] = '\0';
        return 1;
    }

    /* Bare token (number/true/false/null). */
    *is_string = 0;
    while (*p && *p != ',' && *p != '}' && *p != '\n' && *p != '\r' &&
           *p != ' ' && *p != '\t') {
        if (o + 1 < outsz) {
            out[o++] = *p;
        }
        p++;
    }
    out[o < outsz ? o : outsz - 1] = '\0';
    return 1;
}

int xb_config_load(xb_config *cfg) {
    xb_config_defaults(cfg);

    char path[1024];
    if (xb_config_path(path, sizeof(path)) != 0) {
        return XB_CONFIG_ERROR;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (errno == ENOENT) {
            return XB_CONFIG_NOT_FOUND;
        }
        return XB_CONFIG_ERROR;
    }

    /* Slurp the whole (small) file. */
    char buf[8192];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    int read_err = ferror(f);
    fclose(f);
    if (read_err) {
        return XB_CONFIG_ERROR;
    }
    buf[got] = '\0';

    char val[512];
    int is_string;

    if (json_find_value(buf, "interface_uid", val, sizeof(val), &is_string)) {
        snprintf(cfg->interface_uid, sizeof(cfg->interface_uid), "%s", val);
    }
    if (json_find_value(buf, "blackhole_uid", val, sizeof(val), &is_string)) {
        if (val[0] != '\0') {
            snprintf(cfg->blackhole_uid, sizeof(cfg->blackhole_uid), "%s", val);
        }
    }
    if (json_find_value(buf, "in_channel", val, sizeof(val), &is_string)) {
        cfg->in_channel = (int)strtol(val, NULL, 10);
    }
    if (json_find_value(buf, "gain_db", val, sizeof(val), &is_string)) {
        cfg->gain_db = strtod(val, NULL);
    }

    return XB_CONFIG_OK;
}

/* Write `s` into out (size outsz) with JSON string escaping for " and \. */
static void json_escape(const char *s, char *out, size_t outsz) {
    size_t o = 0;
    for (const char *p = s; *p && o + 2 < outsz; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o < outsz ? o : outsz - 1] = '\0';
}

int xb_config_save(const xb_config *cfg) {
    char dir[1024];
    if (xb_config_dir(dir, sizeof(dir)) != 0) {
        return -1;
    }
    if (mkdir_p(dir) != 0) {
        return -1;
    }

    char path[1024];
    if (xb_config_path(path, sizeof(path)) != 0) {
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }

    char iface_esc[512], bh_esc[512];
    json_escape(cfg->interface_uid, iface_esc, sizeof(iface_esc));
    json_escape(cfg->blackhole_uid, bh_esc, sizeof(bh_esc));

    int rc = fprintf(f,
        "{\n"
        "  \"interface_uid\": \"%s\",\n"
        "  \"in_channel\": %d,\n"
        "  \"gain_db\": %g,\n"
        "  \"blackhole_uid\": \"%s\"\n"
        "}\n",
        iface_esc, cfg->in_channel, cfg->gain_db, bh_esc);

    int close_err = (fclose(f) != 0);
    if (rc < 0 || close_err) {
        return -1;
    }
    return 0;
}
