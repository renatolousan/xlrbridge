/*
 * xlrbridge — fixing Discord's problem with handling XLR mics with fancy interfaces.
 *
 * Phase 0: scaffold only. This is a stub CLI that prints usage and a version.
 * No CoreAudio calls yet — device enumeration, aggregate creation, and the
 * routing engine arrive in later phases (see HANDOFF.md §4).
 */

#include <stdio.h>
#include <string.h>

#include "audio_devices.h"

#define XLRBRIDGE_VERSION "0.1.0-phase1"

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
