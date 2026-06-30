/*
 * xlrbridge — routing engine.
 *
 * The core of the tool (HANDOFF.md §3.2/§3.3 `run`, §4 Phase 3). It:
 *   1. waits (bounded) for the interface + BlackHole to be present by UID
 *      (handles the reboot race where devices aren't enumerated yet),
 *   2. creates the private aggregate via the aggregate layer (one clock domain,
 *      drift-corrected — what makes routing glitch-free),
 *   3. installs an IOProc on the aggregate that, per frame, copies the chosen
 *      interface input channel × gain into BlackHole's output channels
 *      (mono source → both → dual-mono),
 *   4. starts the device, blocks until SIGINT/SIGTERM,
 *   5. stops cleanly and ALWAYS destroys the aggregate (no leaks).
 *
 * Everything that runs inside the IOProc callback is real-time-safe: no malloc,
 * no printf, no locks. The layout decisions (interleaved vs per-stream buffers,
 * channel offsets) are resolved ONCE before AudioDeviceStart and cached.
 */

#ifndef XLRBRIDGE_ENGINE_H
#define XLRBRIDGE_ENGINE_H

#include <CoreAudio/CoreAudio.h>

/* Parameters for a routing run. */
typedef struct {
    const char *interface_uid;  /* pro/XLR interface UID (clock master) */
    const char *blackhole_uid;  /* BlackHole 2ch UID (drift-compensated) */
    unsigned int in_channel;    /* 0-based interface input channel to route */
    double gain_db;             /* digital gain in dB (factor = 10^(dB/20)) */
    double sample_rate;         /* nominal rate (Hz); <= 0 => default 48 kHz */
    int readiness_timeout_s;    /* how long to wait for devices before bailing */
} xb_engine_params;

/*
 * Run the routing engine in the foreground until SIGINT/SIGTERM.
 *
 * Returns 0 on a clean shutdown (signal received, engine stopped, aggregate
 * destroyed). Returns non-zero if the required devices never appeared within
 * the readiness window, or if the audio device failed to open/start — both are
 * RETRY conditions for launchd (KeepAlive restarts the process). The aggregate
 * is always destroyed before return, including on every error path.
 */
int xb_engine_run(const xb_engine_params *params);

#endif /* XLRBRIDGE_ENGINE_H */
