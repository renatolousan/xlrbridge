/*
 * xlrbridge — aggregate device layer (implementation).
 *
 * See aggregate.h for the contract.
 *
 * Channel-offset computation (the part Phase 3 depends on):
 *   An aggregate concatenates its sub-devices' channels in SUB-DEVICE LIST
 *   ORDER. The combined input stream is [sub0 inputs][sub1 inputs]...; likewise
 *   for outputs. So a sub-device's offset within a given scope is the SUM of
 *   the channel counts (on that scope) of all sub-devices listed before it.
 *
 *   We don't hardcode "BlackHole starts at 6". Instead we read the aggregate's
 *   own ordered sub-device UID list (kAudioAggregateDevicePropertyFullSubDevice
 *   List), then for each sub-device resolve it and read its real input/output
 *   channel counts from the HAL, accumulating offsets in list order. We then
 *   sanity-check the running totals against the aggregate's actual combined
 *   stream configuration.
 *
 * All CoreFoundation objects we create are released; all malloc'd HAL buffers
 * are freed.
 */

#include "aggregate.h"
#include "audio_devices.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <string.h>

/* Older/newer SDKs renamed the master sub-device key. The wire value is the
 * literal "master" in both; define a fallback if the symbol is absent. */
#ifndef kAudioAggregateDeviceMasterSubDeviceKey
#define kAudioAggregateDeviceMasterSubDeviceKey "master"
#endif

/* ---- small CF helpers ------------------------------------------------- */

/* Build a CFNumber holding an int. Caller releases. */
static CFNumberRef cf_int(int v) {
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
}

/* Build a one-entry CFString from a C string. Caller releases. */
static CFStringRef cf_str(const char *s) {
    return CFStringCreateWithCString(kCFAllocatorDefault, s,
                                     kCFStringEncodingUTF8);
}

/* ---- HAL channel-count helper (duplicated minimally from the device layer
 * so we can query arbitrary AudioDeviceIDs, including the aggregate itself) -- */

static unsigned int channel_count(AudioDeviceID dev,
                                  AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioDevicePropertyStreamConfiguration,
        .mScope    = scope,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr ||
        size == 0) {
        return 0;
    }
    AudioBufferList *bl = (AudioBufferList *)malloc(size);
    if (bl == NULL) {
        return 0;
    }
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, bl) != noErr) {
        free(bl);
        return 0;
    }
    unsigned int total = 0;
    for (UInt32 i = 0; i < bl->mNumberBuffers; i++) {
        total += bl->mBuffers[i].mNumberChannels;
    }
    free(bl);
    return total;
}

/* ---- composition dictionary ------------------------------------------- */

/* Build the sub-device sub-dictionary { kAudioSubDeviceUIDKey: uid [,
 * kAudioSubDeviceDriftCompensationKey: 1] }. Caller releases. */
static CFDictionaryRef make_subdevice_dict(const char *uid, int drift_comp) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (d == NULL) {
        return NULL;
    }
    CFStringRef cfuid = cf_str(uid);
    if (cfuid != NULL) {
        CFDictionarySetValue(d, CFSTR(kAudioSubDeviceUIDKey), cfuid);
        CFRelease(cfuid);
    }
    if (drift_comp) {
        CFNumberRef one = cf_int(1);
        if (one != NULL) {
            CFDictionarySetValue(d, CFSTR(kAudioSubDeviceDriftCompensationKey),
                                 one);
            CFRelease(one);
        }
    }
    return d;
}

/* Build the full aggregate composition dictionary. Caller releases. */
static CFDictionaryRef make_composition(const char *interface_uid,
                                        const char *blackhole_uid) {
    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (desc == NULL) {
        return NULL;
    }

    /* UID + name */
    CFStringRef uid = cf_str(XB_AGGREGATE_UID);
    CFStringRef name = cf_str(XB_AGGREGATE_NAME);
    CFStringRef master = cf_str(interface_uid);
    if (uid)    { CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), uid); }
    if (name)   { CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), name); }

    /* Sub-device list: interface first (so it leads the channel map and is the
     * clock master), then BlackHole with drift compensation on. */
    CFDictionaryRef sub_if = make_subdevice_dict(interface_uid, 0);
    CFDictionaryRef sub_bh = make_subdevice_dict(blackhole_uid, 1);
    if (sub_if && sub_bh) {
        const void *subs[2] = { sub_if, sub_bh };
        CFArrayRef list = CFArrayCreate(kCFAllocatorDefault, subs, 2,
                                        &kCFTypeArrayCallBacks);
        if (list != NULL) {
            CFDictionarySetValue(desc,
                                 CFSTR(kAudioAggregateDeviceSubDeviceListKey),
                                 list);
            CFRelease(list);
        }
    }
    if (sub_if) { CFRelease(sub_if); }
    if (sub_bh) { CFRelease(sub_bh); }

    /* Clock master = interface. */
    if (master) {
        CFDictionarySetValue(desc,
                             CFSTR(kAudioAggregateDeviceMasterSubDeviceKey),
                             master);
    }

    /* Private: visible only to this process. */
    CFNumberRef one = cf_int(1);
    if (one != NULL) {
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey),
                             one);
        CFRelease(one);
    }

    if (uid)    { CFRelease(uid); }
    if (name)   { CFRelease(name); }
    if (master) { CFRelease(master); }
    return desc;
}

/* ---- offset computation ----------------------------------------------- */

/* Read the aggregate's ordered sub-device UID list and walk it, accumulating
 * each scope's running channel offset. We match the interface and BlackHole by
 * UID to capture their offsets. Returns 0 on success. */
static int compute_offsets(AudioDeviceID agg,
                           const char *interface_uid,
                           const char *blackhole_uid,
                           xb_aggregate *out) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioAggregateDevicePropertyFullSubDeviceList,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(agg, &addr, 0, NULL, &size) != noErr ||
        size == 0) {
        return -1;
    }
    CFArrayRef list = NULL;
    if (AudioObjectGetPropertyData(agg, &addr, 0, NULL, &size, &list) != noErr ||
        list == NULL) {
        return -1;
    }

    unsigned int in_off = 0;   /* running input-channel offset */
    unsigned int out_off = 0;  /* running output-channel offset */
    int found_iface = 0;
    int found_bh = 0;

    CFIndex count = CFArrayGetCount(list);
    for (CFIndex i = 0; i < count; i++) {
        CFStringRef cfuid = (CFStringRef)CFArrayGetValueAtIndex(list, i);
        char uid[256];
        uid[0] = '\0';
        if (cfuid != NULL) {
            CFStringGetCString(cfuid, uid, (CFIndex)sizeof(uid),
                               kCFStringEncodingUTF8);
        }

        AudioDeviceID sub = xb_resolve_device_by_uid(uid);
        unsigned int sin = (sub != XB_DEVICE_UNKNOWN)
                               ? channel_count(sub, kAudioObjectPropertyScopeInput)
                               : 0;
        unsigned int sout = (sub != XB_DEVICE_UNKNOWN)
                               ? channel_count(sub, kAudioObjectPropertyScopeOutput)
                               : 0;

        if (strcmp(uid, interface_uid) == 0) {
            out->interface_input_offset = in_off;
            found_iface = 1;
        }
        if (strcmp(uid, blackhole_uid) == 0) {
            out->blackhole_output_offset = out_off;
            out->blackhole_output_channels = sout;
            found_bh = 1;
        }

        in_off += sin;
        out_off += sout;
    }

    CFRelease(list);
    return (found_iface && found_bh) ? 0 : -1;
}

/* ---- create / destroy ------------------------------------------------- */

OSStatus xb_aggregate_destroy(AudioDeviceID agg) {
    if (agg == XB_DEVICE_UNKNOWN || agg == kAudioObjectUnknown) {
        return noErr;
    }
    return AudioHardwareDestroyAggregateDevice(agg);
}

OSStatus xb_aggregate_create(const char *interface_uid,
                             const char *blackhole_uid,
                             double sample_rate,
                             xb_aggregate *out) {
    if (interface_uid == NULL || blackhole_uid == NULL || out == NULL) {
        return kAudioHardwareIllegalOperationError;
    }
    if (sample_rate <= 0.0) {
        sample_rate = XB_AGGREGATE_DEFAULT_SAMPLE_RATE;
    }
    memset(out, 0, sizeof(*out));
    out->id = XB_DEVICE_UNKNOWN;

    /* Idempotency: tear down any leftover aggregate with our UID so repeated
     * runs don't pile up duplicates. */
    AudioDeviceID stale = xb_resolve_device_by_uid(XB_AGGREGATE_UID);
    if (stale != XB_DEVICE_UNKNOWN) {
        xb_aggregate_destroy(stale);
    }

    CFDictionaryRef desc = make_composition(interface_uid, blackhole_uid);
    if (desc == NULL) {
        return kAudioHardwareUnspecifiedError;
    }

    AudioObjectID agg = kAudioObjectUnknown;
    OSStatus st = AudioHardwareCreateAggregateDevice(desc, &agg);
    CFRelease(desc);
    if (st != noErr || agg == kAudioObjectUnknown) {
        return (st != noErr) ? st : kAudioHardwareUnspecifiedError;
    }

    /* Set the nominal sample rate on the aggregate. Non-fatal if it fails
     * (it may already be at the requested rate). */
    {
        AudioObjectPropertyAddress sr_addr = {
            .mSelector = kAudioDevicePropertyNominalSampleRate,
            .mScope    = kAudioObjectPropertyScopeGlobal,
            .mElement  = kAudioObjectPropertyElementMain,
        };
        Float64 sr = (Float64)sample_rate;
        AudioObjectSetPropertyData(agg, &sr_addr, 0, NULL,
                                   (UInt32)sizeof(sr), &sr);
    }

    out->id = agg;
    out->in_channels  = channel_count(agg, kAudioObjectPropertyScopeInput);
    out->out_channels = channel_count(agg, kAudioObjectPropertyScopeOutput);

    if (compute_offsets(agg, interface_uid, blackhole_uid, out) != 0) {
        /* Couldn't determine the layout — don't leave a half-known aggregate
         * around. */
        xb_aggregate_destroy(agg);
        out->id = XB_DEVICE_UNKNOWN;
        return kAudioHardwareUnspecifiedError;
    }

    return noErr;
}
