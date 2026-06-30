/*
 * xlrbridge — audio device layer (implementation).
 *
 * See audio_devices.h for the contract. All CoreFoundation objects we copy out
 * of the HAL are CFRelease'd; all malloc'd HAL buffers are free'd.
 */

#include "audio_devices.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <string.h>

/* Copy a CFStringRef into a fixed C buffer (UTF-8). On any failure the buffer
 * is set to an empty string so callers always see a valid NUL-terminated C
 * string. Does NOT release `s` (the caller owns it). */
static void cfstring_to_c(CFStringRef s, char *buf, size_t buflen) {
    if (buflen == 0) {
        return;
    }
    buf[0] = '\0';
    if (s == NULL) {
        return;
    }
    if (!CFStringGetCString(s, buf, (CFIndex)buflen, kCFStringEncodingUTF8)) {
        buf[0] = '\0';
    }
}

/* Sum mNumberChannels across all buffers of a device's stream configuration on
 * the given scope (input or output). Returns the channel count (0 on error). */
static unsigned int channel_count_for_scope(AudioDeviceID dev,
                                            AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioDevicePropertyStreamConfiguration,
        .mScope    = scope,
        .mElement  = kAudioObjectPropertyElementMain,
    };

    UInt32 size = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size);
    if (st != noErr || size == 0) {
        return 0;
    }

    AudioBufferList *bl = (AudioBufferList *)malloc(size);
    if (bl == NULL) {
        return 0;
    }

    st = AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, bl);
    if (st != noErr) {
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

/* Read a device's UID into buf. */
static void read_device_uid(AudioDeviceID dev, char *buf, size_t buflen) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioDevicePropertyDeviceUID,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    CFStringRef uid = NULL;
    UInt32 size = sizeof(uid);
    OSStatus st = AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &uid);
    if (st == noErr && uid != NULL) {
        cfstring_to_c(uid, buf, buflen);
        CFRelease(uid);
    } else {
        if (buflen > 0) {
            buf[0] = '\0';
        }
    }
}

/* Read a device's human-readable name into buf. Prefer the device-name
 * property, falling back to the generic object-name property. */
static void read_device_name(AudioDeviceID dev, char *buf, size_t buflen) {
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioDevicePropertyDeviceNameCFString,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    CFStringRef name = NULL;
    UInt32 size = sizeof(name);
    OSStatus st = AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &name);
    if (st != noErr || name == NULL) {
        addr.mSelector = kAudioObjectPropertyName;
        st = AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &name);
    }
    if (st == noErr && name != NULL) {
        cfstring_to_c(name, buf, buflen);
        CFRelease(name);
    } else {
        if (buflen > 0) {
            buf[0] = '\0';
        }
    }
}

OSStatus xb_enumerate_devices(xb_device **out_devices, size_t *out_count) {
    if (out_devices == NULL || out_count == NULL) {
        return kAudioHardwareIllegalOperationError;
    }

    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioHardwarePropertyDevices,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };

    UInt32 size = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                                 &addr, 0, NULL, &size);
    if (st != noErr) {
        return st;
    }

    size_t n = size / sizeof(AudioDeviceID);
    if (n == 0) {
        *out_devices = NULL;
        *out_count = 0;
        return noErr;
    }

    AudioDeviceID *ids = (AudioDeviceID *)malloc(size);
    if (ids == NULL) {
        return kAudioHardwareUnspecifiedError;
    }

    st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL,
                                    &size, ids);
    if (st != noErr) {
        free(ids);
        return st;
    }
    /* The size HAL filled in may differ from our first query; trust it. */
    n = size / sizeof(AudioDeviceID);

    xb_device *devs = (xb_device *)calloc(n, sizeof(xb_device));
    if (devs == NULL) {
        free(ids);
        return kAudioHardwareUnspecifiedError;
    }

    for (size_t i = 0; i < n; i++) {
        xb_device *d = &devs[i];
        d->id = ids[i];
        read_device_uid(ids[i], d->uid, sizeof(d->uid));
        read_device_name(ids[i], d->name, sizeof(d->name));
        d->in_channels  = channel_count_for_scope(ids[i],
                                                   kAudioObjectPropertyScopeInput);
        d->out_channels = channel_count_for_scope(ids[i],
                                                   kAudioObjectPropertyScopeOutput);
        d->is_blackhole = (strstr(d->name, "BlackHole") != NULL) ? 1 : 0;
    }

    free(ids);
    *out_devices = devs;
    *out_count = n;
    return noErr;
}

AudioDeviceID xb_resolve_device_by_uid(const char *uid) {
    if (uid == NULL || uid[0] == '\0') {
        return XB_DEVICE_UNKNOWN;
    }

    xb_device *devs = NULL;
    size_t n = 0;
    if (xb_enumerate_devices(&devs, &n) != noErr) {
        return XB_DEVICE_UNKNOWN;
    }

    AudioDeviceID found = XB_DEVICE_UNKNOWN;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(devs[i].uid, uid) == 0) {
            found = devs[i].id;
            break;
        }
    }

    free(devs);
    return found;
}
