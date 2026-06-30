/*
 * xlrbridge — routing engine (implementation). See engine.h for the contract.
 *
 * AudioBufferList layout
 * ----------------------
 * An IOProc receives the device's input and output as AudioBufferLists. A
 * device (or aggregate) may present its channels EITHER as one interleaved
 * buffer of N channels, OR as several buffers (commonly one per stream, each
 * possibly carrying several channels). We must not assume; we locate a global
 * channel index within whichever layout the aggregate presents.
 *
 * We turn a global channel index (0-based across the whole scope, the same
 * index space the aggregate-layer offsets live in) into a (buffer, stride,
 * lane) triple ONCE, before AudioDeviceStart, by walking the buffer list and
 * accumulating mNumberChannels. That triple is cached in the context; the
 * IOProc just does pointer arithmetic — no allocation, no branchy discovery.
 *
 * Because the buffer list's pointers/counts are stable for the life of an
 * opened IOProc (CoreAudio re-uses the same stream layout per cycle), resolving
 * once against the first callback's lists and caching is the RT-safe approach.
 * We resolve lazily on the first callback (the IOProc has the real lists then),
 * guarded by an atomic flag, doing no work that isn't trivial arithmetic.
 */

#include "engine.h"
#include "aggregate.h"
#include "audio_devices.h"

#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- signal handling -------------------------------------------------- */

/* Set by the SIGINT/SIGTERM handler; the main loop polls it. Only async-signal-
 * safe writes happen in the handler. */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ---- per-channel location within an AudioBufferList ------------------- */

/* A resolved location of one global channel index inside a buffer list:
 * which buffer it lives in, the interleave stride (channels per frame in that
 * buffer), and the lane (channel index within that buffer's frame). */
typedef struct {
    int buffer;   /* index into AudioBufferList.mBuffers, or -1 if not found */
    int stride;   /* mNumberChannels of that buffer (interleave stride) */
    int lane;     /* channel within the buffer's interleaved frame */
} xb_chan_loc;

/* Walk a buffer list and resolve global channel `want` to a (buffer,stride,
 * lane). Pure arithmetic, RT-safe. Sets buffer=-1 if out of range. */
static inline xb_chan_loc locate_channel(const AudioBufferList *bl,
                                         unsigned int want) {
    xb_chan_loc loc = { -1, 0, 0 };
    unsigned int base = 0;
    for (UInt32 b = 0; b < bl->mNumberBuffers; b++) {
        unsigned int nch = bl->mBuffers[b].mNumberChannels;
        if (want < base + nch) {
            loc.buffer = (int)b;
            loc.stride = (int)nch;
            loc.lane   = (int)(want - base);
            return loc;
        }
        base += nch;
    }
    return loc;
}

/* ---- IOProc context --------------------------------------------------- */

typedef struct {
    float gain;                 /* linear gain factor */
    unsigned int src_channel;   /* global input channel index (iface + in_ch) */
    unsigned int dst_l;         /* global output channel index, BlackHole L */
    unsigned int dst_r;         /* global output channel index, BlackHole R */

    /* Cached channel locations, resolved on the first callback. */
    _Atomic int resolved;       /* 0 until locations are valid */
    xb_chan_loc src;
    xb_chan_loc dl;
    xb_chan_loc dr;

    /* Set if the first callback couldn't resolve a needed channel (bad layout);
     * the main thread checks this and treats it as a fatal/retry condition. */
    _Atomic int resolve_failed;
} xb_ioproc_ctx;

/* The realtime IOProc: copy one input channel × gain into two output channels.
 * RT-safe: no malloc / printf / locks. The only first-call work is trivial
 * arithmetic in locate_channel(), then an atomic store. */
static OSStatus xb_ioproc(AudioObjectID inDevice,
                          const AudioTimeStamp *inNow,
                          const AudioBufferList *inInputData,
                          const AudioTimeStamp *inInputTime,
                          AudioBufferList *outOutputData,
                          const AudioTimeStamp *inOutputTime,
                          void *inClientData) {
    (void)inDevice; (void)inNow; (void)inInputTime; (void)inOutputTime;
    xb_ioproc_ctx *ctx = (xb_ioproc_ctx *)inClientData;

    if (inInputData == NULL || outOutputData == NULL) {
        return noErr;
    }

    /* Resolve channel locations once, against the live lists. */
    if (!atomic_load_explicit(&ctx->resolved, memory_order_acquire)) {
        ctx->src = locate_channel(inInputData, ctx->src_channel);
        ctx->dl  = locate_channel(outOutputData, ctx->dst_l);
        ctx->dr  = locate_channel(outOutputData, ctx->dst_r);
        if (ctx->src.buffer < 0 || ctx->dl.buffer < 0 || ctx->dr.buffer < 0) {
            atomic_store_explicit(&ctx->resolve_failed, 1, memory_order_release);
            atomic_store_explicit(&ctx->resolved, 1, memory_order_release);
            return noErr; /* leave outputs as the HAL gave them (silence) */
        }
        atomic_store_explicit(&ctx->resolved, 1, memory_order_release);
    }
    if (atomic_load_explicit(&ctx->resolve_failed, memory_order_acquire)) {
        return noErr;
    }

    /* Defensive output zeroing: we only intentionally write the two BlackHole
     * output lanes. Every other output channel (the interface's own playback
     * channels, which the HAL may hand us holding stale/uninitialised samples)
     * must be silenced so nothing leaks to the interface outputs. memset over
     * the buffer storage is RT-safe (no malloc/locks); we zero all output
     * buffers up front, then write our two lanes on top. */
    for (UInt32 b = 0; b < outOutputData->mNumberBuffers; b++) {
        AudioBuffer *ob = &outOutputData->mBuffers[b];
        if (ob->mData != NULL && ob->mDataByteSize > 0) {
            memset(ob->mData, 0, ob->mDataByteSize);
        }
    }

    const AudioBuffer *sbuf = &inInputData->mBuffers[ctx->src.buffer];
    AudioBuffer *lbuf = &outOutputData->mBuffers[ctx->dl.buffer];
    AudioBuffer *rbuf = &outOutputData->mBuffers[ctx->dr.buffer];

    const float *in  = (const float *)sbuf->mData;
    float *outL = (float *)lbuf->mData;
    float *outR = (float *)rbuf->mData;
    if (in == NULL || outL == NULL || outR == NULL) {
        return noErr;
    }

    /* Frame counts can differ per buffer if strides differ; iterate by the
     * number of frames available in each interleaved buffer. */
    const int s_stride = ctx->src.stride;
    const int l_stride = ctx->dl.stride;
    const int r_stride = ctx->dr.stride;

    UInt32 s_frames = (s_stride > 0)
        ? sbuf->mDataByteSize / (UInt32)(s_stride * (int)sizeof(float)) : 0;
    UInt32 l_frames = (l_stride > 0)
        ? lbuf->mDataByteSize / (UInt32)(l_stride * (int)sizeof(float)) : 0;
    UInt32 r_frames = (r_stride > 0)
        ? rbuf->mDataByteSize / (UInt32)(r_stride * (int)sizeof(float)) : 0;

    UInt32 frames = s_frames;
    if (l_frames < frames) frames = l_frames;
    if (r_frames < frames) frames = r_frames;

    const float g = ctx->gain;
    const int s_lane = ctx->src.lane;
    const int l_lane = ctx->dl.lane;
    const int r_lane = ctx->dr.lane;

    for (UInt32 f = 0; f < frames; f++) {
        float v = in[(size_t)f * s_stride + s_lane] * g;
        outL[(size_t)f * l_stride + l_lane] = v;
        outR[(size_t)f * r_stride + r_lane] = v;
    }
    return noErr;
}

/* ---- readiness wait --------------------------------------------------- */

/* Poll until both UIDs resolve to live devices, or the timeout elapses.
 * Returns 0 when both are present, non-zero on timeout. */
static int wait_for_devices(const char *interface_uid,
                            const char *blackhole_uid,
                            int timeout_s) {
    const int interval_ms = 500;
    int waited_ms = 0;
    for (;;) {
        if (g_stop) {
            return 1;
        }
        AudioDeviceID i = xb_resolve_device_by_uid(interface_uid);
        AudioDeviceID b = xb_resolve_device_by_uid(blackhole_uid);
        if (i != XB_DEVICE_UNKNOWN && b != XB_DEVICE_UNKNOWN) {
            return 0;
        }
        if (waited_ms >= timeout_s * 1000) {
            fprintf(stderr,
                    "xlrbridge: devices not ready after %ds "
                    "(interface %s, blackhole %s) — exiting for launchd retry\n",
                    timeout_s,
                    (i != XB_DEVICE_UNKNOWN) ? "ok" : "MISSING",
                    (b != XB_DEVICE_UNKNOWN) ? "ok" : "MISSING");
            return 1;
        }
        usleep(interval_ms * 1000);
        waited_ms += interval_ms;
    }
}

/* ---- run -------------------------------------------------------------- */

int xb_engine_run(const xb_engine_params *params) {
    if (params == NULL || params->interface_uid == NULL ||
        params->blackhole_uid == NULL) {
        return 2;
    }

    /* Install signal handlers so we can shut down cleanly and destroy the
     * aggregate (no leaks). SA_RESTART left off so usleep/pause return. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int timeout_s = params->readiness_timeout_s > 0
                        ? params->readiness_timeout_s : 8;

    /* 1. Readiness wait. Missing devices => exit non-zero for launchd retry. */
    if (wait_for_devices(params->interface_uid, params->blackhole_uid,
                         timeout_s) != 0) {
        return g_stop ? 0 : 1;
    }

    /* 2. Create the private aggregate (one clock domain, drift-corrected). */
    xb_aggregate agg;
    OSStatus st = xb_aggregate_create(params->interface_uid,
                                      params->blackhole_uid,
                                      params->sample_rate, &agg);
    if (st != noErr) {
        fprintf(stderr, "xlrbridge: failed to create aggregate (OSStatus %d)\n",
                (int)st);
        return 1;
    }

    /* Validate the requested input channel exists within the interface's
     * channels. We don't know the interface's exact input count here, but the
     * src global index must be < the aggregate's total input channels. */
    unsigned int src_channel = agg.interface_input_offset + params->in_channel;
    if (src_channel >= agg.in_channels) {
        fprintf(stderr,
                "xlrbridge: input channel %u out of range "
                "(aggregate has %u input channels; src index %u)\n",
                params->in_channel, agg.in_channels, src_channel);
        xb_aggregate_destroy(agg.id);
        return 1;
    }
    if (agg.blackhole_output_channels < 2) {
        fprintf(stderr,
                "xlrbridge: BlackHole exposes %u output channel(s), need 2\n",
                agg.blackhole_output_channels);
        xb_aggregate_destroy(agg.id);
        return 1;
    }

    /* 3. Build the IOProc context. */
    double gdb = params->gain_db;
    xb_ioproc_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.gain        = (float)pow(10.0, gdb / 20.0);
    ctx.src_channel = src_channel;
    ctx.dst_l       = agg.blackhole_output_offset + 0;
    ctx.dst_r       = agg.blackhole_output_offset + 1;
    atomic_store(&ctx.resolved, 0);
    atomic_store(&ctx.resolve_failed, 0);

    fprintf(stderr,
            "xlrbridge: aggregate id=%u (%u in / %u out); "
            "routing in ch %u (global %u) -> BlackHole out %u,%u; gain %.2f dB "
            "(x%.5f)\n",
            (unsigned int)agg.id, agg.in_channels, agg.out_channels,
            params->in_channel, ctx.src_channel, ctx.dst_l, ctx.dst_r,
            gdb, (double)ctx.gain);

    /* 4. Install the IOProc and start. */
    AudioDeviceIOProcID proc_id = NULL;
    st = AudioDeviceCreateIOProcID(agg.id, xb_ioproc, &ctx, &proc_id);
    if (st != noErr || proc_id == NULL) {
        fprintf(stderr, "xlrbridge: AudioDeviceCreateIOProcID failed "
                        "(OSStatus %d)\n", (int)st);
        xb_aggregate_destroy(agg.id);
        return 1;
    }

    st = AudioDeviceStart(agg.id, proc_id);
    if (st != noErr) {
        fprintf(stderr, "xlrbridge: AudioDeviceStart failed (OSStatus %d) "
                        "— exiting for launchd retry\n", (int)st);
        AudioDeviceDestroyIOProcID(agg.id, proc_id);
        xb_aggregate_destroy(agg.id);
        return 1;
    }

    fprintf(stderr, "xlrbridge: running. Ctrl-C / SIGTERM to stop.\n");

    /* 5. Block until a signal. Poll the stop flag (and the resolve_failed flag,
     * which means the IOProc found an impossible layout). */
    int rc = 0;
    while (!g_stop) {
        if (atomic_load(&ctx.resolve_failed)) {
            fprintf(stderr, "xlrbridge: IOProc could not map channels into the "
                            "aggregate buffer layout — exiting\n");
            rc = 1;
            break;
        }
        usleep(200 * 1000);
    }

    /* 6. Clean teardown, always destroying the aggregate. */
    AudioDeviceStop(agg.id, proc_id);
    AudioDeviceDestroyIOProcID(agg.id, proc_id);
    xb_aggregate_destroy(agg.id);

    if (g_stop) {
        fprintf(stderr, "xlrbridge: signal received; stopped cleanly.\n");
    }
    return rc;
}
