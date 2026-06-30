/*
 * xlrbridge — fixing Discord's problem with handling XLR mics with fancy interfaces.
 *
 * CLI dispatch. Implemented so far: `devices` (Phase 1), the private aggregate
 * layer behind `_aggtest` (Phase 2), `run` — the routing engine (Phase 3), and
 * `setup` + config persistence + `run --dry-run` (Phase 4). `status`/`fix`/
 * `gain`/`uninstall` arrive in later phases (HANDOFF §4).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_devices.h"
#include "aggregate.h"
#include "engine.h"
#include "config.h"
#include "service.h"

#define XLRBRIDGE_VERSION "0.5.0-phase5"

/* Default BlackHole UID; the standard install of blackhole-2ch uses this. */
#define XB_DEFAULT_BLACKHOLE_UID "BlackHole2ch_UID"
#define XB_DEFAULT_GAIN_DB       2.0
#define XB_DEFAULT_IN_CHANNEL    0u
#define XB_READINESS_TIMEOUT_S   8

static void print_usage(void) {
    fputs(
        "xlrbridge " XLRBRIDGE_VERSION "\n"
        "Route one input channel of an XLR/pro audio interface into BlackHole,\n"
        "so Discord stops chopping and auto-limiting your mic.\n"
        "\n"
        "Usage:\n"
        "  xlrbridge <command> [args]\n"
        "  xlrbridge --version\n"
        "\n"
        "Commands:\n"
        "  devices     List audio devices with input/output channels + UIDs; flag BlackHole.\n"
        "  setup       Interactive: pick interface/channel/gain, verify BlackHole, write config,\n"
        "                then install + load the dev.xlrbridge LaunchAgent (login service).\n"
        "                Non-interactive flags (skip prompts when --interface-uid given):\n"
        "                --interface-uid <uid>, --in-channel <n>, --gain-db <x>,\n"
        "                --blackhole-uid <uid>, --yes, --no-service (write config only).\n"
        "  run         Daemon: load config (flags override), wait for devices, start routing\n"
        "                IOProc, block until SIGINT/SIGTERM.\n"
        "                Flags: --interface-uid <uid> (default: config, else auto-pick first\n"
        "                non-BlackHole input device), --blackhole-uid <uid> (default config/"
            XB_DEFAULT_BLACKHOLE_UID "),\n"
        "                --in-channel <n> (0-based, default config/0), --gain-db <x>\n"
        "                (default config/2.0), --dry-run (load+resolve+validate+print plan, exit 0\n"
        "                WITHOUT grabbing devices or starting audio).\n"
        "  status      Report whether the dev.xlrbridge agent is loaded + engine running,\n"
        "                do a short BlackHole liveness check, and note if the Pd prototype\n"
        "                (com.scoobert.micbridge) is also loaded.\n"
        "  fix         Re-resolve devices and reload the dev.xlrbridge agent (unload+load).\n"
        "  gain <dB>   Update digital gain and reload the running engine. (not yet implemented)\n"
        "  uninstall   Unload + remove the dev.xlrbridge agent (engine destroys its aggregate).\n",
        stdout);
}

/* Print a table of all audio devices: name, UID, input/output channel counts,
 * and a marker for the BlackHole virtual device. Returns 0 on success, 1 on
 * enumeration failure. */
static int cmd_devices(void) {
    xb_device *devs = NULL;
    size_t n = 0;

    OSStatus st = xb_enumerate_devices(&devs, &n);
    if (st != noErr) {
        fprintf(stderr, "xlrbridge: failed to enumerate audio devices (OSStatus %d)\n",
                (int)st);
        return 1;
    }

    printf("%-28s %-40s %5s %5s\n", "NAME", "UID", "IN", "OUT");
    printf("%-28s %-40s %5s %5s\n",
           "----------------------------",
           "----------------------------------------",
           "-----", "-----");

    int blackhole_found = 0;
    for (size_t i = 0; i < n; i++) {
        const xb_device *d = &devs[i];
        printf("%-28s %-40s %5u %5u%s\n",
               d->name, d->uid, d->in_channels, d->out_channels,
               d->is_blackhole ? "  <- BlackHole" : "");
        if (d->is_blackhole) {
            blackhole_found = 1;
        }
    }

    printf("\n%zu device(s). BlackHole: %s\n", n,
           blackhole_found ? "installed" : "NOT FOUND (install blackhole-2ch)");

    free(devs);
    return 0;
}

/* Find the interface to bridge: prefer a device whose UID/name mentions
 * "Topping", otherwise the first non-BlackHole device that has input channels.
 * Returns a pointer into `devs` or NULL if none found. */
static const xb_device *pick_interface(const xb_device *devs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (strstr(devs[i].uid, "Topping") != NULL ||
            strstr(devs[i].name, "Topping") != NULL) {
            return &devs[i];
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (!devs[i].is_blackhole && devs[i].in_channels > 0) {
            return &devs[i];
        }
    }
    return NULL;
}

/* Internal/debug self-test for the aggregate layer (Phase 2): detect the
 * interface + BlackHole, create the private aggregate, print channel counts
 * and computed offsets, verify the expected layout, then destroy it and
 * confirm it's gone. Always destroys what it creates, even on error paths. */
static int cmd_aggtest(void) {
    xb_device *devs = NULL;
    size_t n = 0;
    if (xb_enumerate_devices(&devs, &n) != noErr) {
        fprintf(stderr, "xlrbridge: _aggtest: failed to enumerate devices\n");
        return 1;
    }

    /* Find the interface. */
    const xb_device *iface = pick_interface(devs, n);
    if (iface == NULL) {
        fprintf(stderr, "xlrbridge: _aggtest: no input interface found\n");
        free(devs);
        return 1;
    }

    /* Find BlackHole by UID. */
    const xb_device *bh = NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(devs[i].uid, "BlackHole2ch_UID") == 0) {
            bh = &devs[i];
            break;
        }
    }
    if (bh == NULL) {
        for (size_t i = 0; i < n; i++) {
            if (devs[i].is_blackhole) { bh = &devs[i]; break; }
        }
    }
    if (bh == NULL) {
        fprintf(stderr, "xlrbridge: _aggtest: BlackHole not found "
                        "(install blackhole-2ch)\n");
        free(devs);
        return 1;
    }

    /* Snapshot what we need before the array is freed. */
    char iface_uid[256], iface_name[256], bh_uid[256];
    snprintf(iface_uid, sizeof(iface_uid), "%s", iface->uid);
    snprintf(iface_name, sizeof(iface_name), "%s", iface->name);
    snprintf(bh_uid, sizeof(bh_uid), "%s", bh->uid);
    unsigned int iface_out_ch = iface->out_channels;

    printf("interface : %s  (%s)\n", iface_name, iface_uid);
    printf("blackhole : %s  (%s)\n", bh->name, bh_uid);
    printf("creating private aggregate \"%s\" (UID %s) at %.0f Hz...\n",
           XB_AGGREGATE_NAME, XB_AGGREGATE_UID, XB_AGGREGATE_DEFAULT_SAMPLE_RATE);

    free(devs);

    /* Create. */
    xb_aggregate agg;
    OSStatus st = xb_aggregate_create(iface_uid, bh_uid,
                                      XB_AGGREGATE_DEFAULT_SAMPLE_RATE, &agg);
    if (st != noErr) {
        fprintf(stderr, "xlrbridge: _aggtest: create failed (OSStatus %d)\n",
                (int)st);
        return 1;
    }

    printf("\naggregate created: AudioDeviceID %u\n", (unsigned int)agg.id);
    printf("  total channels        : %u in / %u out\n",
           agg.in_channels, agg.out_channels);
    printf("  interface input offset: %u\n", agg.interface_input_offset);
    printf("  blackhole out offset  : %u  (%u channels)\n",
           agg.blackhole_output_offset, agg.blackhole_output_channels);

    /* Verify the expected layout for this machine: 8 in / 8 out, BlackHole's
     * outputs starting right after the interface's outputs. */
    int ok = 1;
    if (agg.in_channels != 8 || agg.out_channels != 8) {
        fprintf(stderr, "  WARN: expected 8 in / 8 out, got %u / %u\n",
                agg.in_channels, agg.out_channels);
        ok = 0;
    }
    if (agg.blackhole_output_offset != iface_out_ch) {
        fprintf(stderr, "  WARN: blackhole out offset %u != interface out "
                        "channels %u\n",
                agg.blackhole_output_offset, iface_out_ch);
        ok = 0;
    }
    if (ok) {
        printf("  layout OK: 8/8, BlackHole outputs start at offset %u "
               "(after %u interface outputs)\n",
               agg.blackhole_output_offset, iface_out_ch);
    }

    /* Destroy. */
    AudioDeviceID agg_id = agg.id;
    st = xb_aggregate_destroy(agg_id);
    if (st != noErr) {
        fprintf(stderr, "xlrbridge: _aggtest: destroy failed (OSStatus %d)\n",
                (int)st);
        return 1;
    }

    /* Confirm it's gone. The HAL's published device list can lag the destroy
     * call by a moment, so poll briefly rather than checking exactly once. */
    AudioDeviceID leftover = XB_DEVICE_UNKNOWN;
    for (int tries = 0; tries < 20; tries++) {
        leftover = xb_resolve_device_by_uid(XB_AGGREGATE_UID);
        if (leftover == XB_DEVICE_UNKNOWN) {
            break;
        }
        usleep(50 * 1000); /* 50 ms */
    }
    if (leftover != XB_DEVICE_UNKNOWN) {
        fprintf(stderr, "xlrbridge: _aggtest: aggregate still present after "
                        "destroy (id %u)\n", (unsigned int)leftover);
        return 1;
    }

    printf("\naggregate destroyed; no leftover with UID %s. teardown clean.\n",
           XB_AGGREGATE_UID);
    return ok ? 0 : 1;
}

/* Auto-pick an interface UID when --interface-uid wasn't given: the E2x2, i.e.
 * the first non-BlackHole device with input channels. Writes into out_uid
 * (size out_sz). Returns 0 on success, 1 if none found / enumeration failed. */
static int autopick_interface_uid(char *out_uid, size_t out_sz) {
    xb_device *devs = NULL;
    size_t n = 0;
    if (xb_enumerate_devices(&devs, &n) != noErr) {
        fprintf(stderr, "xlrbridge: run: failed to enumerate devices\n");
        return 1;
    }
    const xb_device *iface = pick_interface(devs, n);
    if (iface == NULL) {
        fprintf(stderr, "xlrbridge: run: no input interface found "
                        "(plug in your interface, or pass --interface-uid)\n");
        free(devs);
        return 1;
    }
    snprintf(out_uid, out_sz, "%s", iface->uid);
    free(devs);
    return 0;
}

/* Locate BlackHole among enumerated devices: prefer an exact UID match, then
 * any device flagged is_blackhole. Returns a pointer into `devs` or NULL. */
static const xb_device *find_blackhole(const xb_device *devs, size_t n,
                                       const char *preferred_uid) {
    if (preferred_uid != NULL && preferred_uid[0] != '\0') {
        for (size_t i = 0; i < n; i++) {
            if (strcmp(devs[i].uid, preferred_uid) == 0) {
                return &devs[i];
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (devs[i].is_blackhole) {
            return &devs[i];
        }
    }
    return NULL;
}

/* Print the standard "install BlackHole" instructions. */
static void print_blackhole_instructions(void) {
    fputs(
        "\nxlrbridge: BlackHole 2ch is not installed.\n"
        "It is the clean virtual device Discord will read; xlrbridge routes your\n"
        "mic into it. Install it with Homebrew:\n"
        "\n"
        "    brew install blackhole-2ch\n"
        "\n"
        "or download it from https://existential.audio/blackhole/\n"
        "Then re-run `xlrbridge setup`.\n",
        stderr);
}

/* Read a line from stdin into buf (size bufsz), trimming trailing newline and
 * surrounding whitespace. Returns 0 on success, -1 on EOF/error. */
static int read_line(char *buf, size_t bufsz) {
    if (fgets(buf, (int)bufsz, stdin) == NULL) {
        return -1;
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                       buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
        buf[--len] = '\0';
    }
    /* Trim leading whitespace. */
    size_t start = 0;
    while (buf[start] == ' ' || buf[start] == '\t') {
        start++;
    }
    if (start > 0) {
        memmove(buf, buf + start, len - start + 1);
    }
    return 0;
}

/* Interactive setup: enumerate candidate interfaces, prompt for interface /
 * channel / gain, verify BlackHole, write config. Returns 0 on success. */
static int setup_interactive(xb_config *cfg) {
    xb_device *devs = NULL;
    size_t n = 0;
    if (xb_enumerate_devices(&devs, &n) != noErr) {
        fprintf(stderr, "xlrbridge: setup: failed to enumerate devices\n");
        return 1;
    }

    /* Build the list of candidate interfaces: >= 1 input channel, not BlackHole. */
    size_t idx[64];
    size_t cand = 0;
    for (size_t i = 0; i < n && cand < 64; i++) {
        if (!devs[i].is_blackhole && devs[i].in_channels > 0) {
            idx[cand++] = i;
        }
    }
    if (cand == 0) {
        fprintf(stderr, "xlrbridge: setup: no audio interface with input "
                        "channels found. Plug in your interface and retry.\n");
        free(devs);
        return 1;
    }

    printf("Available input interfaces:\n");
    for (size_t k = 0; k < cand; k++) {
        const xb_device *d = &devs[idx[k]];
        printf("  [%zu] %-28s %u in  (UID %s)\n", k + 1, d->name,
               d->in_channels, d->uid);
    }

    /* Prompt for interface choice. */
    size_t chosen = 0;
    char line[256];
    for (;;) {
        printf("Pick an interface [1-%zu] (default 1): ", cand);
        fflush(stdout);
        if (read_line(line, sizeof(line)) != 0) {
            fprintf(stderr, "\nxlrbridge: setup: no input; aborting.\n");
            free(devs);
            return 1;
        }
        if (line[0] == '\0') {
            chosen = 0;
            break;
        }
        char *end = NULL;
        long v = strtol(line, &end, 10);
        if (end != line && *end == '\0' && v >= 1 && (size_t)v <= cand) {
            chosen = (size_t)v - 1;
            break;
        }
        printf("  please enter a number between 1 and %zu.\n", cand);
    }
    const xb_device *iface = &devs[idx[chosen]];
    unsigned int iface_in = iface->in_channels;
    printf("Selected: %s (%u input channels)\n", iface->name, iface_in);

    /* Prompt for the mic's input channel (0-based). */
    int channel = 0;
    for (;;) {
        printf("Which input channel is the mic? [0-%u] (default 0): ",
               iface_in - 1);
        fflush(stdout);
        if (read_line(line, sizeof(line)) != 0) {
            fprintf(stderr, "\nxlrbridge: setup: no input; aborting.\n");
            free(devs);
            return 1;
        }
        if (line[0] == '\0') {
            channel = 0;
            break;
        }
        char *end = NULL;
        long v = strtol(line, &end, 10);
        if (end != line && *end == '\0' && v >= 0 && (unsigned int)v < iface_in) {
            channel = (int)v;
            break;
        }
        printf("  please enter a channel between 0 and %u.\n", iface_in - 1);
    }

    /* Prompt for gain. */
    double gain = XB_CONFIG_DEFAULT_GAIN_DB;
    for (;;) {
        printf("Digital gain in dB (default %+g): ", XB_CONFIG_DEFAULT_GAIN_DB);
        fflush(stdout);
        if (read_line(line, sizeof(line)) != 0) {
            fprintf(stderr, "\nxlrbridge: setup: no input; aborting.\n");
            free(devs);
            return 1;
        }
        if (line[0] == '\0') {
            gain = XB_CONFIG_DEFAULT_GAIN_DB;
            break;
        }
        char *end = NULL;
        double v = strtod(line, &end);
        if (end != line && *end == '\0') {
            gain = v;
            break;
        }
        printf("  please enter a number (e.g. 2 or 1.5).\n");
    }

    /* Verify BlackHole is present. */
    const xb_device *bh = find_blackhole(devs, n, cfg->blackhole_uid);
    if (bh == NULL) {
        print_blackhole_instructions();
        free(devs);
        return 3;
    }

    /* Commit choices into cfg. */
    snprintf(cfg->interface_uid, sizeof(cfg->interface_uid), "%s", iface->uid);
    cfg->in_channel = channel;
    cfg->gain_db    = gain;
    snprintf(cfg->blackhole_uid, sizeof(cfg->blackhole_uid), "%s", bh->uid);

    free(devs);
    return 0;
}

/* Non-interactive setup: caller already populated cfg from flags. Verify the
 * interface UID resolves to an in-range channel and BlackHole is present.
 * Returns 0 on success. */
static int setup_noninteractive(xb_config *cfg) {
    xb_device *devs = NULL;
    size_t n = 0;
    if (xb_enumerate_devices(&devs, &n) != noErr) {
        fprintf(stderr, "xlrbridge: setup: failed to enumerate devices\n");
        return 1;
    }

    /* Locate the named interface so we can validate the channel. */
    const xb_device *iface = NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(devs[i].uid, cfg->interface_uid) == 0) {
            iface = &devs[i];
            break;
        }
    }
    if (iface == NULL) {
        fprintf(stderr, "xlrbridge: setup: interface UID '%s' not found among "
                        "current devices.\n", cfg->interface_uid);
        free(devs);
        return 1;
    }
    if (cfg->in_channel < 0 ||
        (unsigned int)cfg->in_channel >= iface->in_channels) {
        fprintf(stderr, "xlrbridge: setup: in-channel %d out of range for '%s' "
                        "(has %u input channels, valid 0-%u).\n",
                cfg->in_channel, iface->name, iface->in_channels,
                iface->in_channels > 0 ? iface->in_channels - 1 : 0);
        free(devs);
        return 1;
    }

    const xb_device *bh = find_blackhole(devs, n, cfg->blackhole_uid);
    if (bh == NULL) {
        print_blackhole_instructions();
        free(devs);
        return 3;
    }
    /* Normalize to the resolved BlackHole UID. */
    snprintf(cfg->blackhole_uid, sizeof(cfg->blackhole_uid), "%s", bh->uid);

    free(devs);
    return 0;
}

/* `setup`: interactive by default; non-interactive when --interface-uid (and
 * friends) are given. Detects+instructs BlackHole (never auto-installs), writes
 * the config, prints a confirmation + next step. */
static int cmd_setup(int argc, char **argv) {
    xb_config cfg;
    xb_config_defaults(&cfg);

    const char *interface_uid = NULL;
    int have_in_channel = 0, have_gain = 0, no_service = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--interface-uid") == 0 && i + 1 < argc) {
            interface_uid = argv[++i];
        } else if (strcmp(a, "--in-channel") == 0 && i + 1 < argc) {
            cfg.in_channel = (int)strtol(argv[++i], NULL, 10);
            have_in_channel = 1;
        } else if (strcmp(a, "--gain-db") == 0 && i + 1 < argc) {
            cfg.gain_db = strtod(argv[++i], NULL);
            have_gain = 1;
        } else if (strcmp(a, "--blackhole-uid") == 0 && i + 1 < argc) {
            snprintf(cfg.blackhole_uid, sizeof(cfg.blackhole_uid), "%s",
                     argv[++i]);
        } else if (strcmp(a, "--no-service") == 0) {
            no_service = 1;
        } else if (strcmp(a, "--yes") == 0 || strcmp(a, "-y") == 0) {
            /* accepted for scripting; setup writes config either way */
        } else {
            fprintf(stderr, "xlrbridge: setup: unknown/incomplete option '%s'\n",
                    a);
            return 2;
        }
    }
    (void)have_in_channel;
    (void)have_gain;

    int rc;
    if (interface_uid != NULL) {
        /* Non-interactive path: --interface-uid drives it. */
        snprintf(cfg.interface_uid, sizeof(cfg.interface_uid), "%s",
                 interface_uid);
        rc = setup_noninteractive(&cfg);
    } else {
        rc = setup_interactive(&cfg);
    }
    if (rc != 0) {
        return rc;
    }

    if (xb_config_save(&cfg) != 0) {
        fprintf(stderr, "xlrbridge: setup: failed to write config\n");
        return 1;
    }

    char path[1024];
    xb_config_path(path, sizeof(path));
    printf("\nConfig written to %s\n", path);
    printf("  interface_uid : %s\n", cfg.interface_uid);
    printf("  in_channel    : %d\n", cfg.in_channel);
    printf("  gain_db       : %+g\n", cfg.gain_db);
    printf("  blackhole_uid : %s\n", cfg.blackhole_uid);

    if (no_service) {
        printf("\nConfig only (--no-service). Run `xlrbridge run` to start "
               "routing now,\nor `xlrbridge setup` (without --no-service) to "
               "install the login service.\n");
        return 0;
    }

    /* CUTOVER SAFETY: never quietly steal the mic from another live bridge.
     * If the Pd prototype agent is loaded, refuse to auto-install our agent
     * (the two would contend for the interface). The operator must stop Pd
     * first (or pass --no-service). */
    if (xb_service_is_loaded(XB_PD_AGENT_LABEL)) {
        fprintf(stderr,
            "\nxlrbridge: setup: another bridge (%s) is currently loaded.\n"
            "  Not installing the dev.xlrbridge agent — the two would contend\n"
            "  for the interface. Config IS written. To cut over: stop the Pd\n"
            "  bridge (`launchctl unload ~/Library/LaunchAgents/%s.plist`) then\n"
            "  re-run setup, or run setup with --no-service and switch manually.\n",
            XB_PD_AGENT_LABEL, XB_PD_AGENT_LABEL);
        return 0;
    }

    printf("\nInstalling login service (LaunchAgent %s)...\n", XB_AGENT_LABEL);
    if (xb_service_write_plist(NULL) != 0) {
        fprintf(stderr, "xlrbridge: setup: failed to write the LaunchAgent "
                        "plist\n");
        return 1;
    }
    /* Reload cleanly in case a stale instance is loaded. */
    xb_service_unload();
    if (xb_service_load() != 0) {
        fprintf(stderr, "xlrbridge: setup: failed to load the LaunchAgent\n");
        return 1;
    }
    char plist[1024];
    xb_service_plist_path(plist, sizeof(plist));
    printf("Installed + loaded %s\n  -> %s\n", XB_AGENT_LABEL, plist);
    printf("\nThe engine now starts at login and is running. Check it with "
           "`xlrbridge status`.\n");
    return 0;
}

/* `run --dry-run`: load config + flag overrides, resolve both devices by UID,
 * validate the channel is in range, print the routing plan, and exit 0 WITHOUT
 * creating the aggregate or starting audio. Non-disruptive: grabs nothing.
 * Returns 0 on a valid plan, non-zero on a resolution/validation failure. */
static int run_dry(const xb_engine_params *p) {
    printf("xlrbridge run --dry-run: validating config (no devices grabbed)\n\n");

    AudioDeviceID iface_id = xb_resolve_device_by_uid(p->interface_uid);
    AudioDeviceID bh_id    = xb_resolve_device_by_uid(p->blackhole_uid);

    int ok = 1;

    if (iface_id == XB_DEVICE_UNKNOWN) {
        fprintf(stderr, "  interface : UID '%s' -> NOT FOUND\n",
                p->interface_uid);
        ok = 0;
    }
    if (bh_id == XB_DEVICE_UNKNOWN) {
        fprintf(stderr, "  blackhole : UID '%s' -> NOT FOUND\n",
                p->blackhole_uid);
        ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "\nxlrbridge: run --dry-run: required device(s) not "
                        "present.\n");
        return 1;
    }

    /* Validate the chosen input channel against the interface's input count. */
    xb_device *devs = NULL;
    size_t n = 0;
    unsigned int iface_in = 0;
    char iface_name[256] = "?", bh_name[256] = "?";
    if (xb_enumerate_devices(&devs, &n) == noErr) {
        for (size_t i = 0; i < n; i++) {
            if (devs[i].id == iface_id) {
                iface_in = devs[i].in_channels;
                snprintf(iface_name, sizeof(iface_name), "%s", devs[i].name);
            }
            if (devs[i].id == bh_id) {
                snprintf(bh_name, sizeof(bh_name), "%s", devs[i].name);
            }
        }
        free(devs);
    }

    if (iface_in == 0) {
        fprintf(stderr, "  interface '%s' reports 0 input channels.\n",
                iface_name);
        return 1;
    }
    if (p->in_channel >= iface_in) {
        fprintf(stderr, "  in-channel %u out of range for '%s' (valid 0-%u).\n",
                p->in_channel, iface_name, iface_in - 1);
        return 1;
    }

    printf("  interface : %-24s (UID %s)\n", iface_name, p->interface_uid);
    printf("              %u input channels; routing input channel %u\n",
           iface_in, p->in_channel);
    printf("  blackhole : %-24s (UID %s)\n", bh_name, p->blackhole_uid);
    printf("  gain      : %+g dB\n", p->gain_db);
    printf("  rate      : %.0f Hz\n",
           p->sample_rate > 0 ? p->sample_rate
                              : (double)XB_AGGREGATE_DEFAULT_SAMPLE_RATE);
    printf("\nPlan: aggregate [%s + %s], copy interface input ch%u (x gain)\n"
           "      into both BlackHole output channels (dual-mono). Discord reads\n"
           "      '%s' as its mic.\n",
           iface_name, bh_name, p->in_channel, bh_name);
    printf("\nDry run OK. (Aggregate not created; no audio started.)\n");
    return 0;
}

/* `run`: load config (flags override config; config overrides built-in
 * defaults), resolve the interface (auto-pick if still unset), then either
 * print the plan (--dry-run) or hand off to the routing engine. */
static int cmd_run(int argc, char **argv) {
    /* 1. Start from config (which itself starts from built-in defaults). */
    xb_config cfg;
    int cfg_status = xb_config_load(&cfg);
    if (cfg_status == XB_CONFIG_ERROR) {
        fprintf(stderr, "xlrbridge: run: failed to read config\n");
        return 1;
    }

    const char *interface_uid = cfg.interface_uid[0] ? cfg.interface_uid : NULL;
    char blackhole_buf[256];
    snprintf(blackhole_buf, sizeof(blackhole_buf), "%s", cfg.blackhole_uid);
    const char *blackhole_uid = blackhole_buf;
    unsigned int in_channel   = (unsigned int)cfg.in_channel;
    double gain_db            = cfg.gain_db;
    int dry_run               = 0;

    /* 2. Flags override config. */
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--interface-uid") == 0 && i + 1 < argc) {
            interface_uid = argv[++i];
        } else if (strcmp(a, "--blackhole-uid") == 0 && i + 1 < argc) {
            blackhole_uid = argv[++i];
        } else if (strcmp(a, "--in-channel") == 0 && i + 1 < argc) {
            in_channel = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "--gain-db") == 0 && i + 1 < argc) {
            gain_db = strtod(argv[++i], NULL);
        } else if (strcmp(a, "--dry-run") == 0) {
            dry_run = 1;
        } else {
            fprintf(stderr, "xlrbridge: run: unknown/incomplete option '%s'\n", a);
            return 2;
        }
    }

    /* 3. Auto-pick the interface only if neither config nor flags supplied one.
     * In --dry-run we do NOT auto-pick: a missing interface should surface as a
     * clear config error, not be silently substituted. */
    char auto_uid[256];
    if (interface_uid == NULL) {
        if (dry_run) {
            fprintf(stderr, "xlrbridge: run --dry-run: no interface configured "
                            "(run `xlrbridge setup`, or pass --interface-uid).\n");
            if (cfg_status == XB_CONFIG_NOT_FOUND) {
                fprintf(stderr, "  (no config file present)\n");
            }
            return 1;
        }
        if (autopick_interface_uid(auto_uid, sizeof(auto_uid)) != 0) {
            return 1;
        }
        interface_uid = auto_uid;
        fprintf(stderr, "xlrbridge: run: auto-picked interface UID %s\n",
                interface_uid);
    }

    xb_engine_params p = {
        .interface_uid      = interface_uid,
        .blackhole_uid      = blackhole_uid,
        .in_channel         = in_channel,
        .gain_db            = gain_db,
        .sample_rate        = XB_AGGREGATE_DEFAULT_SAMPLE_RATE,
        .readiness_timeout_s = XB_READINESS_TIMEOUT_S,
    };

    if (dry_run) {
        return run_dry(&p);
    }
    return xb_engine_run(&p);
}

/* Short, non-grabbing liveness check of BlackHole: record 2 s via ffmpeg and
 * report mean_volume. Records by name (":BlackHole 2ch"), which opens BlackHole
 * directly as an INPUT — it does not touch the interface or the aggregate, so
 * it doesn't disturb the running engine. Prints the result; returns 0 if it
 * could measure a level, -1 if ffmpeg is missing or the capture failed. */
static int blackhole_liveness(void) {
    if (system("command -v ffmpeg >/dev/null 2>&1") != 0) {
        printf("  liveness  : (ffmpeg not installed; skipping signal check)\n");
        return -1;
    }
    /* Record 2 s, then read mean_volume. The avfoundation capture can wedge if
     * CoreAudio's capture path is in a bad state, so hard-kill it after a bound
     * (well past the 2 s record) — `status` must never hang. */
    const char *cmd =
        "w=$(mktemp -t xlrbridge_live).wav; "
        "( ffmpeg -nostdin -hide_banner -loglevel error -f avfoundation "
        "    -i ':BlackHole 2ch' -t 2 -y \"$w\" </dev/null 2>/dev/null ) & "
        "cpid=$!; "
        "( sleep 8; kill -9 $cpid 2>/dev/null ) & wpid=$!; "
        "wait $cpid 2>/dev/null; rc=$?; kill $wpid 2>/dev/null; "
        "if [ $rc -eq 0 ] && [ -s \"$w\" ]; then "
        "  ffmpeg -hide_banner -i \"$w\" -af volumedetect -f null /dev/null 2>&1 "
        "    | grep mean_volume; "
        "else echo 'CAPTURE_FAILED'; fi; "
        "rm -f \"$w\"";
    char out[512];
    FILE *fp = popen(cmd, "r");
    if (fp == NULL) {
        printf("  liveness  : (could not run capture)\n");
        return -1;
    }
    size_t total = 0;
    size_t r;
    while (total + 1 < sizeof(out) &&
           (r = fread(out + total, 1, sizeof(out) - 1 - total, fp)) > 0) {
        total += r;
    }
    out[total] = '\0';
    int rc = pclose(fp);

    /* Parse "mean_volume: -XX.X dB". */
    const char *m = strstr(out, "mean_volume:");
    if (m == NULL) {
        if (strstr(out, "CAPTURE_FAILED") != NULL) {
            printf("  liveness  : capture FAILED (BlackHole could not be opened "
                   "for recording —\n              mic/TCC permission, or "
                   "CoreAudio capture path wedged)\n");
        } else {
            printf("  liveness  : (no signal measured%s)\n",
                   rc != 0 ? "; capture failed" : "");
        }
        return -1;
    }
    double mean = strtod(m + strlen("mean_volume:"), NULL);
    printf("  liveness  : mean_volume %.1f dBFS  ->  %s\n", mean,
           mean > -91.0 ? "SIGNAL FLOWING"
                        : "SILENT (<= -91; not flowing / TCC-denied?)");
    return 0;
}

/* `status`: report whether the dev.xlrbridge agent is loaded + its engine
 * process is running, do a short BlackHole liveness check, and note whether the
 * Pd prototype agent is also loaded (so the user sees which bridge is active).
 * Always exits 0 (it's a report). */
static int cmd_status(void) {
    printf("xlrbridge status\n\n");

    int xb_loaded = xb_service_is_loaded(XB_AGENT_LABEL);
    long xb_pid = -1;
    int xb_running = (xb_service_agent_pid(XB_AGENT_LABEL, &xb_pid) == 0 &&
                      xb_pid > 0);

    printf("  agent     : %s (%s)\n", XB_AGENT_LABEL,
           xb_loaded ? "loaded" : "NOT loaded");
    if (xb_loaded) {
        if (xb_running) {
            printf("  engine    : running (pid %ld)\n", xb_pid);
        } else {
            printf("  engine    : loaded but no process (idle/throttled/"
                   "waiting for devices)\n");
        }
    } else {
        printf("  engine    : not running (agent not loaded)\n");
    }

    blackhole_liveness();

    int pd_loaded = xb_service_is_loaded(XB_PD_AGENT_LABEL);
    printf("  pd bridge : %s (%s)\n", XB_PD_AGENT_LABEL,
           pd_loaded ? "LOADED — this is the active bridge" : "not loaded");

    if (xb_loaded && pd_loaded) {
        printf("\n  WARNING: both bridges are loaded — they will contend for "
               "the interface.\n           Unload one.\n");
    }
    if (!xb_loaded && !pd_loaded) {
        printf("\n  No bridge is loaded. Run `xlrbridge setup` (or load the Pd "
               "bridge) to route your mic.\n");
    }
    return 0;
}

/* `fix`: re-resolve devices and reload the dev.xlrbridge agent (unload+load).
 * The engine matches by UID and waits for devices, so a reload is all that's
 * normally needed (panic button). Returns 0 on success. */
static int cmd_fix(void) {
    char plist[1024];
    if (xb_service_plist_path(plist, sizeof(plist)) != 0) {
        fprintf(stderr, "xlrbridge: fix: cannot resolve plist path\n");
        return 1;
    }

    /* Re-resolve the configured devices so we report what fix is targeting. */
    xb_config cfg;
    if (xb_config_load(&cfg) != XB_CONFIG_ERROR && cfg.interface_uid[0]) {
        AudioDeviceID iface = xb_resolve_device_by_uid(cfg.interface_uid);
        AudioDeviceID bh    = xb_resolve_device_by_uid(cfg.blackhole_uid);
        printf("xlrbridge fix: re-resolving devices...\n");
        printf("  interface %s -> %s\n", cfg.interface_uid,
               iface == XB_DEVICE_UNKNOWN ? "NOT present (engine will wait)"
                                          : "present");
        printf("  blackhole %s -> %s\n", cfg.blackhole_uid,
               bh == XB_DEVICE_UNKNOWN ? "NOT present (engine will wait)"
                                       : "present");
    }

    /* Ensure the plist exists (re-write it if missing), then reload. */
    if (access(plist, F_OK) != 0) {
        printf("  plist missing; re-writing %s\n", plist);
        if (xb_service_write_plist(NULL) != 0) {
            fprintf(stderr, "xlrbridge: fix: failed to write plist\n");
            return 1;
        }
    }

    printf("  reloading agent %s ...\n", XB_AGENT_LABEL);
    xb_service_unload();
    if (xb_service_load() != 0) {
        fprintf(stderr, "xlrbridge: fix: failed to reload the agent\n");
        return 1;
    }
    printf("Reloaded. Check with `xlrbridge status`.\n");
    return 0;
}

/* `uninstall`: unload + remove the dev.xlrbridge plist. The engine destroys its
 * own private aggregate when it stops, so there's nothing else to clean up.
 * Returns 0 on success. */
static int cmd_uninstall(void) {
    char plist[1024];
    xb_service_plist_path(plist, sizeof(plist));

    printf("xlrbridge uninstall: unloading + removing %s\n", XB_AGENT_LABEL);
    xb_service_unload(); /* tolerate not-loaded */
    if (xb_service_remove_plist() != 0) {
        fprintf(stderr, "xlrbridge: uninstall: failed to remove plist\n");
        return 1;
    }
    printf("Removed %s\n", plist);
    printf("The engine destroys its private aggregate on stop; nothing else "
           "to clean up.\n");
    printf("(Config left at ~/.config/xlrbridge/config.json.)\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        puts("xlrbridge " XLRBRIDGE_VERSION);
        return 0;
    }

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "help") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(cmd, "devices") == 0) {
        return cmd_devices();
    }

    if (strcmp(cmd, "_aggtest") == 0) {
        return cmd_aggtest();
    }

    if (strcmp(cmd, "run") == 0) {
        return cmd_run(argc - 2, argv + 2);
    }

    if (strcmp(cmd, "setup") == 0) {
        return cmd_setup(argc - 2, argv + 2);
    }

    if (strcmp(cmd, "status") == 0) {
        return cmd_status();
    }

    if (strcmp(cmd, "fix") == 0) {
        return cmd_fix();
    }

    if (strcmp(cmd, "uninstall") == 0) {
        return cmd_uninstall();
    }

    if (strcmp(cmd, "gain") == 0) {
        printf("xlrbridge: 'gain' is not yet implemented (Phase 6).\n");
        return 0;
    }

    fprintf(stderr, "xlrbridge: unknown command '%s'\n\n", cmd);
    print_usage();
    return 2;
}
