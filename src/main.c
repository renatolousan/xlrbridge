/*
 * xlrbridge — fixing Discord's problem with handling XLR mics with fancy interfaces.
 *
 * Phase 0: scaffold only. This is a stub CLI that prints usage and a version.
 * No CoreAudio calls yet — device enumeration, aggregate creation, and the
 * routing engine arrive in later phases (see HANDOFF.md §4).
 */

#include <stdio.h>
#include <string.h>

#define XLRBRIDGE_VERSION "0.0.0-phase0"

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
        "  devices     List interfaces with input channels + UIDs; flag BlackHole. (not yet implemented)\n"
        "  setup       Interactive: pick interface/channel/gain, create aggregate, install agent. (not yet implemented)\n"
        "  run         Daemon: wait for devices, start routing IOProc, block. (not yet implemented)\n"
        "  status      Report whether the agent is loaded and signal is flowing. (not yet implemented)\n"
        "  fix         Re-resolve devices, recreate the aggregate, reload the agent. (not yet implemented)\n"
        "  gain <dB>   Update digital gain and reload the running engine. (not yet implemented)\n"
        "  uninstall   Unload/remove the agent, destroy the aggregate. (not yet implemented)\n",
        stdout);
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

    if (strcmp(cmd, "devices") == 0 || strcmp(cmd, "setup") == 0 ||
        strcmp(cmd, "run") == 0 || strcmp(cmd, "status") == 0 ||
        strcmp(cmd, "fix") == 0 || strcmp(cmd, "gain") == 0 ||
        strcmp(cmd, "uninstall") == 0) {
        printf("xlrbridge: '%s' is not yet implemented (Phase 0 scaffold).\n", cmd);
        return 0;
    }

    fprintf(stderr, "xlrbridge: unknown command '%s'\n\n", cmd);
    print_usage();
    return 0;
}
