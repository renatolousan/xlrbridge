/*
 * xlrbridge — fixing Discord's problem with handling XLR mics with fancy interfaces.
 *
 * CLI dispatch. Implemented so far: `devices` (Phase 1), the private aggregate
 * layer behind `_aggtest` (Phase 2), and `run` — the routing engine (Phase 3).
 * `setup`/`status`/`fix`/`gain`/`uninstall` arrive in later phases (HANDOFF §4).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_devices.h"
#include "aggregate.h"
#include "engine.h"

#define XLRBRIDGE_VERSION "0.3.0-phase3"

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
        "  setup       Interactive: pick interface/channel/gain, create aggregate, install agent. (not yet implemented)\n"
        "  run         Daemon: wait for devices, start routing IOProc, block until SIGINT/SIGTERM.\n"
        "                Flags: --interface-uid <uid> (default: auto-pick first non-BlackHole\n"
        "                input device), --blackhole-uid <uid> (default " XB_DEFAULT_BLACKHOLE_UID "),\n"
        "                --in-channel <n> (0-based, default 0), --gain-db <x> (default 2.0).\n"
        "  status      Report whether the agent is loaded and signal is flowing. (not yet implemented)\n"
        "  fix         Re-resolve devices, recreate the aggregate, reload the agent. (not yet implemented)\n"
        "  gain <dB>   Update digital gain and reload the running engine. (not yet implemented)\n"
        "  uninstall   Unload/remove the agent, destroy the aggregate. (not yet implemented)\n",
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

/* `run`: parse flags, resolve the interface (auto-pick if unset), and hand off
 * to the routing engine, which waits for devices, creates the aggregate, runs
 * the IOProc, and blocks until SIGINT/SIGTERM. */
static int cmd_run(int argc, char **argv) {
    const char *interface_uid = NULL;
    const char *blackhole_uid = XB_DEFAULT_BLACKHOLE_UID;
    unsigned int in_channel   = XB_DEFAULT_IN_CHANNEL;
    double gain_db            = XB_DEFAULT_GAIN_DB;

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
        } else {
            fprintf(stderr, "xlrbridge: run: unknown/incomplete option '%s'\n", a);
            return 2;
        }
    }

    char auto_uid[256];
    if (interface_uid == NULL) {
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
    return xb_engine_run(&p);
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

    if (strcmp(cmd, "setup") == 0 ||
        strcmp(cmd, "status") == 0 || strcmp(cmd, "fix") == 0 ||
        strcmp(cmd, "gain") == 0 || strcmp(cmd, "uninstall") == 0) {
        printf("xlrbridge: '%s' is not yet implemented.\n", cmd);
        return 0;
    }

    fprintf(stderr, "xlrbridge: unknown command '%s'\n\n", cmd);
    print_usage();
    return 2;
}
