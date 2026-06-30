/*
 * xlrbridge — audio device layer.
 *
 * A thin, reusable wrapper over the CoreAudio HAL for enumerating audio
 * devices and resolving them by their stable UID. Matching by UID (not by
 * index, which shifts as devices come and go) is central to xlrbridge — later
 * phases (aggregate creation, the routing engine) all key off UIDs.
 */

#ifndef XLRBRIDGE_AUDIO_DEVICES_H
#define XLRBRIDGE_AUDIO_DEVICES_H

#include <CoreAudio/CoreAudio.h>
#include <stddef.h>

/* Returned by xb_resolve_device_by_uid() when no device matches. */
#define XB_DEVICE_UNKNOWN ((AudioDeviceID)kAudioObjectUnknown)

/* A snapshot of one audio device. Strings are fixed-size C buffers so the
 * struct owns its storage and callers never free anything. */
typedef struct {
    AudioDeviceID id;
    char          uid[256];   /* stable across reboots/replug; the match key */
    char          name[256];  /* human-readable name */
    unsigned int  in_channels;
    unsigned int  out_channels;
    int           is_blackhole; /* 1 if name contains "BlackHole" */
} xb_device;

/*
 * Enumerate all audio devices into a freshly malloc'd array.
 *
 * On success returns 0, stores the array in *out_devices and the count in
 * *out_count. The caller owns *out_devices and must free() it. On failure
 * returns a non-zero OSStatus and leaves the outputs untouched.
 */
OSStatus xb_enumerate_devices(xb_device **out_devices, size_t *out_count);

/*
 * Resolve a device by its UID. Returns the AudioDeviceID, or
 * XB_DEVICE_UNKNOWN if no device with that UID currently exists.
 */
AudioDeviceID xb_resolve_device_by_uid(const char *uid);

#endif /* XLRBRIDGE_AUDIO_DEVICES_H */
