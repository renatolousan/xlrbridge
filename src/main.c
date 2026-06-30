/*
 * xlrbridge — fixing Discord's problem with handling XLR mics with fancy interfaces.
 *
 * Phase 0: scaffold only. This is a stub CLI that prints usage and a version.
 * No CoreAudio calls yet — device enumeration, aggregate creation, and the
 * routing engine arrive in later phases (see HANDOFF.md §4).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "audio_devices.h"
#include "aggregate.h"

#define XLRBRIDGE_VERSION "0.2.0-phase2"

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
        "  run         Daemon: wait for devices, start routing IOProc, block. (not yet implemented)\n"
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

    if (strcmp(cmd, "setup") == 0 || strcmp(cmd, "run") == 0 ||
        strcmp(cmd, "status") == 0 || strcmp(cmd, "fix") == 0 ||
        strcmp(cmd, "gain") == 0 || strcmp(cmd, "uninstall") == 0) {
        printf("xlrbridge: '%s' is not yet implemented.\n", cmd);
        return 0;
    }

    fprintf(stderr, "xlrbridge: unknown command '%s'\n\n", cmd);
    print_usage();
    return 2;
}
