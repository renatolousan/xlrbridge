/*
 * xlrbridge — aggregate device layer.
 *
 * Creates and destroys a PRIVATE CoreAudio Aggregate Device composed of
 * [interface + BlackHole], with the interface as clock master and drift
 * compensation enabled on BlackHole. A single aggregate is one clock domain,
 * which is what makes the input->virtual routing glitch-free (see HANDOFF.md
 * §1.4).
 *
 * The aggregate is PRIVATE (kAudioAggregateDeviceIsPrivateKey = 1): it only
 * exists within the process that creates it. Phase 3's `run` daemon will
 * create it at startup and destroy it on exit. Nothing here persists across
 * process exit.
 *
 * Phase 3's routing IOProc needs to know WHERE each sub-device's channels sit
 * within the aggregate's combined streams: read the interface's input channel
 * at `interface_input_offset` and write BlackHole's output channels starting
 * at `blackhole_output_offset`. These offsets are computed by reading the
 * aggregate's actual layout, never hardcoded.
 */

#ifndef XLRBRIDGE_AGGREGATE_H
#define XLRBRIDGE_AGGREGATE_H

#include <CoreAudio/CoreAudio.h>

/* The stable UID we give our aggregate. Lets us find/reuse/replace a leftover
 * one from a previous run instead of piling up duplicates. */
#define XB_AGGREGATE_UID  "dev.xlrbridge.aggregate"
#define XB_AGGREGATE_NAME "xlrbridge engine"

/* Default nominal sample rate (Hz). The prototype runs at 48 kHz. */
#define XB_AGGREGATE_DEFAULT_SAMPLE_RATE 48000.0

/* The result of creating the aggregate. */
typedef struct {
    AudioDeviceID id;                 /* the aggregate's AudioDeviceID */
    unsigned int  in_channels;        /* total input channels  (iface + BH) */
    unsigned int  out_channels;       /* total output channels (iface + BH) */

    /* Where the interface's input channels start within the aggregate's
     * combined input stream. Phase 3 reads the chosen mic channel relative to
     * this offset. */
    unsigned int  interface_input_offset;

    /* Where BlackHole's output channels start within the aggregate's combined
     * output stream. Phase 3 writes the mic into these. */
    unsigned int  blackhole_output_offset;
    unsigned int  blackhole_output_channels; /* how many BH output channels */
} xb_aggregate;

/*
 * Create the private aggregate from two device UIDs.
 *
 * interface_uid : the pro/XLR interface (clock master).
 * blackhole_uid : BlackHole 2ch (drift-compensated sub-device).
 * sample_rate   : nominal rate in Hz; pass <= 0 for XB_AGGREGATE_DEFAULT_SAMPLE_RATE.
 * out           : filled in on success.
 *
 * Any pre-existing aggregate with XB_AGGREGATE_UID is destroyed first so runs
 * don't accumulate duplicates. Returns noErr on success; on failure returns a
 * non-zero OSStatus and does not leak a partially-created aggregate.
 */
OSStatus xb_aggregate_create(const char *interface_uid,
                             const char *blackhole_uid,
                             double sample_rate,
                             xb_aggregate *out);

/*
 * Destroy an aggregate created by xb_aggregate_create(). Safe to call with
 * kAudioObjectUnknown (no-op). Returns the OSStatus of the destroy call.
 */
OSStatus xb_aggregate_destroy(AudioDeviceID agg);

#endif /* XLRBRIDGE_AGGREGATE_H */
