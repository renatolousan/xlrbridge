/*
 * xlrbridge — config persistence (HANDOFF.md §3.2 #5, §4 Phase 4).
 *
 * The `setup` command writes the user's choices; `run` reads them back when
 * its flags are omitted (flags > config > built-in defaults). The file lives
 * at ~/.config/xlrbridge/config.json and uses a tiny, dependency-free flat
 * JSON object (no external JSON library), e.g.:
 *
 *   {
 *     "interface_uid": "AppleUSBAudioEngine:Topping:E2x2:...",
 *     "in_channel": 0,
 *     "gain_db": 2.0,
 *     "blackhole_uid": "BlackHole2ch_UID"
 *   }
 *
 * The parser handles only this flat shape but is forgiving: arbitrary
 * whitespace, key ordering, missing keys (defaults kept), and \" / \\ escapes
 * in string values are all tolerated, and it is robust to a missing file.
 */

#ifndef XLRBRIDGE_CONFIG_H
#define XLRBRIDGE_CONFIG_H

#include <stddef.h>

/* Defaults applied by xb_config_defaults() and on missing keys. */
#define XB_CONFIG_DEFAULT_BLACKHOLE_UID "BlackHole2ch_UID"
#define XB_CONFIG_DEFAULT_GAIN_DB        2.0
#define XB_CONFIG_DEFAULT_IN_CHANNEL     0

/* xb_config_load() return values. */
#define XB_CONFIG_OK         0  /* file existed and parsed */
#define XB_CONFIG_NOT_FOUND  1  /* no config file present (not an error) */
#define XB_CONFIG_ERROR     -1  /* I/O or unexpected failure */

/* A loaded/edited configuration. Strings are fixed-size owned buffers. */
typedef struct {
    char   interface_uid[256];  /* may be empty if never set */
    int    in_channel;          /* 0-based interface input channel */
    double gain_db;             /* digital gain in dB */
    char   blackhole_uid[256];  /* BlackHole 2ch UID */
} xb_config;

/* Fill *cfg with built-in defaults (empty interface_uid, gain 2.0, channel 0,
 * blackhole BlackHole2ch_UID). */
void xb_config_defaults(xb_config *cfg);

/*
 * Resolve the config file path into buf (size bufsz). Returns 0 on success,
 * -1 if HOME is unset or the path would overflow.
 */
int xb_config_path(char *buf, size_t bufsz);

/*
 * Load the config from ~/.config/xlrbridge/config.json.
 *
 * Always starts from defaults, then overlays any keys found in the file, so
 * partial/old files still yield a usable struct. Returns XB_CONFIG_OK if the
 * file existed and was read, XB_CONFIG_NOT_FOUND if there is no file (cfg holds
 * defaults), or XB_CONFIG_ERROR on an I/O failure.
 */
int xb_config_load(xb_config *cfg);

/*
 * Save *cfg, creating ~/.config/xlrbridge/ (mkdir -p semantics) as needed.
 * Returns 0 on success, -1 on failure.
 */
int xb_config_save(const xb_config *cfg);

#endif /* XLRBRIDGE_CONFIG_H */
