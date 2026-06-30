/*
 * xlrbridge — launchd service integration (HANDOFF.md §3.2 #6, §4 Phase 5).
 *
 * Installs/loads/unloads a per-user LaunchAgent that runs `xlrbridge run`, so
 * the routing engine survives login/reboot. KeepAlive + the engine's own
 * readiness-wait cover the boot race (HANDOFF §2 "Two fragilities"). Matching
 * is by UID inside the engine, so a plist that just points at the binary is
 * enough — no device indices baked into the service.
 *
 * The Pd prototype (label com.scoobert.micbridge) is the currently-LIVE bridge.
 * Status reporting also reports whether THAT agent is loaded, so the user can
 * always see which bridge is active and the two never silently contend.
 */

#ifndef XLRBRIDGE_SERVICE_H
#define XLRBRIDGE_SERVICE_H

#include <stddef.h>

/* Our LaunchAgent label and the Pd prototype's label (for cross-reporting). */
#define XB_AGENT_LABEL    "dev.xlrbridge"
#define XB_PD_AGENT_LABEL "com.scoobert.micbridge"

/* Log file the agent writes stdout+stderr to. */
#define XB_AGENT_LOG_PATH "/tmp/xlrbridge.log"

/*
 * Resolve the absolute path to ~/Library/LaunchAgents/dev.xlrbridge.plist into
 * buf (size bufsz). Returns 0 on success, -1 if HOME is unset or path overflows.
 */
int xb_service_plist_path(char *buf, size_t bufsz);

/*
 * Resolve the absolute path of the currently-running executable into buf
 * (size bufsz). Phase 5 uses /Users/scoobert/xlrbridge/xlrbridge; Phase 6 will
 * handle a /usr/local/bin install. Returns 0 on success, -1 on failure.
 */
int xb_service_self_path(char *buf, size_t bufsz);

/*
 * Write ~/Library/LaunchAgents/dev.xlrbridge.plist running `<binary> run`,
 * RunAtLoad=true, KeepAlive=true, ThrottleInterval=10, std out/err to the log.
 * binary_path: absolute path to the xlrbridge binary (NULL => self path).
 * Returns 0 on success, -1 on failure.
 */
int xb_service_write_plist(const char *binary_path);

/*
 * launchctl load -w / unload of our plist. load tolerates "already loaded";
 * unload tolerates "not loaded". Returns 0 on success, -1 on failure.
 */
int xb_service_load(void);
int xb_service_unload(void);

/* Is a given LaunchAgent label currently loaded? (launchctl list | label). */
int xb_service_is_loaded(const char *label);

/*
 * Get the PID of a loaded agent's process, or -1 if not running / not loaded
 * (a loaded-but-idle agent shows '-' for PID). On success returns 0 and sets
 * *out_pid; returns -1 otherwise.
 */
int xb_service_agent_pid(const char *label, long *out_pid);

/* Remove the plist file from disk. Returns 0 on success (incl. already gone). */
int xb_service_remove_plist(void);

#endif /* XLRBRIDGE_SERVICE_H */
