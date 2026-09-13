// AudioServerPlugIn for the BOSS GT-10 in Advanced mode.
//
// Ad-hoc signed, no paid Apple Developer account needed. coreaudiod loads it
// despite amfid's -423 error.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "GT10USBDevice.h"
#include "GT10USBStream.h"
#include "../Sources/GT10Clock.h"
#include "../Sources/GT10Ring.h"

#define GT10_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "GT10Audio: " fmt, ##__VA_ARGS__)

enum {
    kObjectID_PlugIn    = kAudioObjectPlugInObject,
    kObjectID_Device    = 2,
    kObjectID_StreamIn  = 3,
    kObjectID_StreamOut = 4,
};

#define kDevice_UID      "GT10Audio:Device"
#define kDevice_ModelUID "GT10Audio:Model:GT-10"

// The GT-10 wire format in Advanced mode. Presented unchanged as both the
// physical and the virtual format, so the USB transport later needs no
// configuration change and the HAL's own convert operations serve clients.
enum {
    kSampleRate    = 44100,
    kChannels      = 2,
    kBitsPerSample = 24,
    kBytesPerFrame = kChannels * kBitsPerSample / 8,
};

// Minimum allowed by the HAL is 10923 frames. The stream engine publishes at
// the same period.

// About 1.5 s each way, far more than any IO buffer.
enum { kRingFrames = 1 << 16 };

// Captured audio reaches the ring one transfer of 4 bus frames after it was
// sampled, plus scheduling. 256 frames is 5.8 ms, which covers that with room
// and is what a player hears when monitoring through the Mac.
//
// Output is rendered when a transfer is submitted. The engine leads by 16 bus
// frames and submits 4 transfers of 4 frames at a start, so the first render
// reaches 31 bus frames ahead. The HAL must have written that far, which is
// why this one stays at 1024 frames, 23.2 ms.
enum { kInputSafetyOffset = 256, kOutputSafetyOffset = 1024 };

static AudioServerPlugInHostRef gHost = NULL;
static _Atomic UInt32 gRefCount       = 0;

// Control path state, guarded by gStateMutex. Never touched from the IO thread.
static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt64 gIOClients           = 0;

// Clock state. gHostTicksPerFrame is fixed in Initialize. The anchor of the
// synthetic clock is set by the first StartIO.
static Float64 gHostTicksPerFrame     = 0.0;
static _Atomic UInt64 gAnchorHostTime = 0;

// The clock the HAL sees: its source and its seed in one word, so a change of
// source and its new seed land in one store. Changed only by compare and swap,
// from StartIO and StopIO and from the monitor queue, which may race.
enum { kClockStopped = 0, kClockSynthetic = 1, kClockDevice = 2 };
static _Atomic UInt64 gClock = 0;  // (seed << 2) | source

static UInt64 ClockSeed(UInt64 clock) { return clock >> 2; }
static UInt32 ClockSource(UInt64 clock) { return (UInt32)(clock & 3); }

// Move to `to` under a new seed, if the current source is in fromMask.
static Boolean ClockMove(UInt32 fromMask, UInt32 to) {
    UInt64 clock = atomic_load(&gClock);
    for (;;) {
        if (((1u << ClockSource(clock)) & fromMask) == 0) return false;
        const UInt64 next = ((ClockSeed(clock) + 1) << 2) | to;
        if (atomic_compare_exchange_weak(&gClock, &clock, next)) return true;
    }
}

// IO operations inside the rings. The IO path counts itself in before it looks
// at the clock, and a ring reset waits for zero after the clock left the
// device. Atomic stores kept their order across cores, so one of the two
// always sees the other.
static _Atomic UInt64 gIOActive = 0;

// USB ownership. The device stays published without the pedal, reports
// itself not alive, and reads silence on a synthetic clock, so a client that
// already runs keeps running.
static _Atomic bool gUSBHeld       = false;
static dispatch_queue_t gUSBQueue  = NULL;
static GT10USBMonitor *gUSBMonitor = NULL;

// Diagnostics for the HAL side, read through the custom property below. The
// plug-in host has no usable log, so these counters are the only way to see
// which sample positions the HAL asks for. The host marshals custom properties
// only as a CFString or a property list, and only when they are declared in
// the custom property info list.
#define kGT10PropertyDiagnostics 'GTdg'

static _Atomic uint64_t gReadCalls = 0, gReadSilent = 0, gWriteCalls = 0;
// IO cycles whose sample time did not belong to the ring they addressed.
static _Atomic uint64_t gStrayPositions  = 0;
static _Atomic int64_t gLastReadPosition = 0, gLastWritePosition = 0;
static _Atomic uint64_t gLastReadValid = 0;

// Streaming buffers, created once for the process.
static GT10Ring *gCaptureRing    = NULL;
static GT10Ring *gPlaybackRing   = NULL;
static GT10TimeStamp *gTimeStamp = NULL;

static void USBHeldChanged(bool held, void *ctx) {
    (void)ctx;
    atomic_store(&gUSBHeld, held);
    GT10_LOG("USB: interfaces %s", held ? "held" : "released");

    // Asynchronous, because the host may query properties from inside the
    // notification while StartIO holds the state lock and waits on this queue.
    AudioServerPlugInHostRef host = gHost;
    if (host == NULL) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const AudioObjectPropertyAddress alive = {kAudioDevicePropertyDeviceIsAlive,
                                                  kAudioObjectPropertyScopeGlobal,
                                                  kAudioObjectPropertyElementMain};
        host->PropertiesChanged(host, kObjectID_Device, 1, &alive);
    });
}

static void RenderPlayback(void *ctx, int64_t position, uint8_t *dst, uint32_t frames) {
    GT10RingRead((GT10Ring *)ctx, position, dst, frames);
}

// Monitor queue. The device clock takes over only while the synthetic one
// runs, which means IO is still running, and hands back when the stream ends.
static bool StreamStateChanged(void *ctx, bool running) {
    (void)ctx;
    if (running) {
        const Boolean taken = ClockMove(1u << kClockSynthetic, kClockDevice);
        GT10_LOG("USB: %s", taken ? "streaming on the device clock" : "stream refused, IO stopped");
        return taken;
    }
    if (ClockMove(1u << kClockDevice, kClockSynthetic)) {
        GT10_LOG("USB: stream ended, back on the synthetic clock");
    }
    return true;
}

// Monitor queue, just before a start. No engine runs or was ever abandoned, so
// only an IO operation that saw the previous device clock can still be inside
// a ring. Resetting under it would break the ring's single-reader contract.
static bool PrepareStream(void *ctx) {
    (void)ctx;
    for (int waited = 0; atomic_load(&gIOActive) != 0; waited++) {
        if (waited >= 100) {
            GT10_LOG("USB: IO did not drain, staying on the synthetic clock");
            return false;
        }
        usleep(1000);
    }
    GT10RingReset(gCaptureRing);
    GT10RingReset(gPlaybackRing);
    GT10TimeStampClear(gTimeStamp);
    return true;
}

// Called with gStateMutex held, when the first client starts IO. The pedal does
// not have to be attached: registering the request is what makes the monitor
// start a stream by itself once the pedal arrives. Returning early here instead
// would leave a recording app on the synthetic clock for as long as it runs.
static void StartStream(void) {
    if (gUSBMonitor == NULL || gCaptureRing == NULL || gPlaybackRing == NULL ||
        gTimeStamp == NULL) {
        return;
    }
    const GT10StreamConfig config = {
        .capture       = true,
        .captureRing   = gCaptureRing,
        .render        = RenderPlayback,
        .renderContext = gPlaybackRing,
        .timeStamp     = gTimeStamp,
    };
    const GT10StreamOwner owner = {PrepareStream, StreamStateChanged, NULL};
    if (GT10USBMonitorSetStream(gUSBMonitor, &config, &owner) != GT10_OK) {
        GT10_LOG("USB: stream did not start, staying on the synthetic clock");
    }
}

// Called with gStateMutex held, when the last client stops IO.
static void StopStream(void) {
    if (gUSBMonitor != NULL && GT10USBMonitorSetStream(gUSBMonitor, NULL, NULL) != GT10_OK) {
        GT10_LOG("USB: stream did not stop cleanly, the device is quarantined");
    }
}

#pragma mark IUnknown

static HRESULT GT10_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG GT10_AddRef(void *inDriver);
static ULONG GT10_Release(void *inDriver);

#pragma mark Lifecycle

static OSStatus GT10_Initialize(AudioServerPlugInDriverRef inDriver,
                                AudioServerPlugInHostRef inHost) {
    (void)inDriver;
    gHost = inHost;

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const Float64 ticksPerSecond = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
    gHostTicksPerFrame           = ticksPerSecond / (Float64)kSampleRate;

    GT10_LOG("Initialize, host %p, %.3f ticks per frame", (void *)inHost, gHostTicksPerFrame);

    // Start the USB monitor once, even if the host calls Initialize again. One
    // persistent serial queue owns every USB step, so acquisition, retry and
    // release never overlap.
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        gCaptureRing  = GT10RingCreate(kRingFrames, kBytesPerFrame);
        gPlaybackRing = GT10RingCreate(kRingFrames, kBytesPerFrame);
        gTimeStamp    = GT10TimeStampCreate();
        gUSBQueue     = dispatch_queue_create("com.lukasmalkmus.gt10audio.usb", NULL);
        if (gUSBQueue != NULL) {
            gUSBMonitor = GT10USBMonitorStart(gUSBQueue, USBHeldChanged, NULL);
        }
    });
    return kAudioHardwareNoError;
}

static OSStatus GT10_CreateDevice(AudioServerPlugInDriverRef inDriver,
                                  CFDictionaryRef inDescription,
                                  const AudioServerPlugInClientInfo *inClientInfo,
                                  AudioObjectID *outDeviceObjectID) {
    (void)inDriver;
    (void)inDescription;
    (void)inClientInfo;
    (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus GT10_DestroyDevice(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inDeviceObjectID) {
    (void)inDriver;
    (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus GT10_AddDeviceClient(AudioServerPlugInDriverRef inDriver,
                                     AudioObjectID inDeviceObjectID,
                                     const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver;
    (void)inClientInfo;
    return inDeviceObjectID == kObjectID_Device ? kAudioHardwareNoError
                                                : kAudioHardwareBadObjectError;
}

static OSStatus GT10_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                        AudioObjectID inDeviceObjectID,
                                        const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver;
    (void)inClientInfo;
    return inDeviceObjectID == kObjectID_Device ? kAudioHardwareNoError
                                                : kAudioHardwareBadObjectError;
}

// The device never requests a configuration change, so the host never has
// anything to perform or abort.
static OSStatus GT10_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                      AudioObjectID inDeviceObjectID,
                                                      UInt64 inChangeAction, void *inChangeInfo) {
    (void)inDriver;
    (void)inChangeAction;
    (void)inChangeInfo;
    return inDeviceObjectID == kObjectID_Device ? kAudioHardwareNoError
                                                : kAudioHardwareBadObjectError;
}

static OSStatus GT10_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                    AudioObjectID inDeviceObjectID,
                                                    UInt64 inChangeAction, void *inChangeInfo) {
    (void)inDriver;
    (void)inChangeAction;
    (void)inChangeInfo;
    return inDeviceObjectID == kObjectID_Device ? kAudioHardwareNoError
                                                : kAudioHardwareBadObjectError;
}

#pragma mark Property payloads

// Every property answer goes through these. outData NULL means a size query,
// which lets GetPropertyDataSize and GetPropertyData share one switch.

static OSStatus PutBytes(void *outData, UInt32 inSize, UInt32 *outSize, const void *src, UInt32 n) {
    *outSize = n;
    if (outData == NULL) return kAudioHardwareNoError;
    if (inSize < n) return kAudioHardwareBadPropertySizeError;
    memcpy(outData, src, n);
    return kAudioHardwareNoError;
}

static OSStatus Put32(void *outData, UInt32 inSize, UInt32 *outSize, UInt32 v) {
    return PutBytes(outData, inSize, outSize, &v, sizeof v);
}

static OSStatus PutF64(void *outData, UInt32 inSize, UInt32 *outSize, Float64 v) {
    return PutBytes(outData, inSize, outSize, &v, sizeof v);
}

// The caller owns the returned string.
static OSStatus PutString(void *outData, UInt32 inSize, UInt32 *outSize, CFStringRef s) {
    *outSize = sizeof(CFStringRef);
    if (outData == NULL) return kAudioHardwareNoError;
    if (inSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
    *(CFStringRef *)outData = (CFStringRef)CFRetain(s);
    return kAudioHardwareNoError;
}

// Arrays fill whatever fits and report the bytes written, per the HAL contract.
static OSStatus PutArray(void *outData, UInt32 inSize, UInt32 *outSize, const void *items,
                         UInt32 count, UInt32 itemSize) {
    if (outData == NULL) {
        *outSize = count * itemSize;
        return kAudioHardwareNoError;
    }
    UInt32 fit = inSize / itemSize;
    if (fit > count) fit = count;
    if (fit != 0) memcpy(outData, items, fit * itemSize);
    *outSize = fit * itemSize;
    return kAudioHardwareNoError;
}

// OwnedObjects takes an optional array of class IDs. An object is listed when
// its class or its base class is named, so kAudioObjectClassID matches all.
static Boolean ClassWanted(AudioClassID cls, UInt32 qSize, const void *q) {
    if (q == NULL || qSize == 0) return true;
    const AudioClassID *want = q;
    for (UInt32 i = 0; i < qSize / sizeof(AudioClassID); i++) {
        if (want[i] == cls || want[i] == kAudioObjectClassID) return true;
    }
    return false;
}

static AudioStreamBasicDescription StreamFormat(void) {
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate       = kSampleRate;
    f.mFormatID         = kAudioFormatLinearPCM;
    f.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket   = kBytesPerFrame;
    f.mFramesPerPacket  = 1;
    f.mBytesPerFrame    = kBytesPerFrame;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel   = kBitsPerSample;
    return f;
}

// Every field except mReserved.
static Boolean SameFormat(const AudioStreamBasicDescription *a,
                          const AudioStreamBasicDescription *b) {
    return a->mSampleRate == b->mSampleRate && a->mFormatID == b->mFormatID &&
           a->mFormatFlags == b->mFormatFlags && a->mBytesPerPacket == b->mBytesPerPacket &&
           a->mFramesPerPacket == b->mFramesPerPacket && a->mBytesPerFrame == b->mBytesPerFrame &&
           a->mChannelsPerFrame == b->mChannelsPerFrame && a->mBitsPerChannel == b->mBitsPerChannel;
}

#pragma mark Property tables

static Boolean PlugInHas(AudioObjectPropertySelector s) {
    switch (s) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:
    case kAudioPlugInPropertyTranslateUIDToDevice:
    case kAudioPlugInPropertyResourceBundle: return true;
    default: return false;
    }
}

static Boolean DeviceHas(AudioObjectPropertySelector s) {
    switch (s) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioObjectPropertyControlList:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kAudioDevicePropertyTransportType:
    case kAudioDevicePropertyRelatedDevices:
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertyStreams:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDevicePropertyAvailableNominalSampleRates:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyPreferredChannelsForStereo:
    case kAudioDevicePropertyPreferredChannelLayout:
    case kAudioDevicePropertyZeroTimeStampPeriod:
    case kAudioObjectPropertyCustomPropertyInfoList:
    case kGT10PropertyDiagnostics: return true;
    default: return false;
    }
}

static Boolean StreamHas(AudioObjectPropertySelector s) {
    switch (s) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyDirection:
    case kAudioStreamPropertyTerminalType:
    case kAudioStreamPropertyStartingChannel:
    case kAudioStreamPropertyLatency:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: return true;
    default: return false;
    }
}

// Device properties that have a per-direction answer. Everything else on
// every object is global only, and all objects have only the main element.
static Boolean DeviceScoped(AudioObjectPropertySelector s) {
    switch (s) {
    // AudioHardwareBase.h: the scope selects which default role is asked
    // about, so the HAL queries this one per direction.
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioObjectPropertyControlList:
    case kAudioDevicePropertyStreams:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyPreferredChannelsForStereo:
    case kAudioDevicePropertyPreferredChannelLayout: return true;
    default: return false;
    }
}

static Boolean AddressValid(AudioObjectID obj, const AudioObjectPropertyAddress *a) {
    if (a->mElement != kAudioObjectPropertyElementMain) return false;
    if (a->mScope == kAudioObjectPropertyScopeGlobal) return true;
    return obj == kObjectID_Device && DeviceScoped(a->mSelector) &&
           (a->mScope == kAudioObjectPropertyScopeInput ||
            a->mScope == kAudioObjectPropertyScopeOutput);
}

static Boolean ObjectHas(AudioObjectID obj, const AudioObjectPropertyAddress *a) {
    if (!AddressValid(obj, a)) return false;
    switch (obj) {
    case kObjectID_PlugIn: return PlugInHas(a->mSelector);
    case kObjectID_Device: return DeviceHas(a->mSelector);
    case kObjectID_StreamIn:
    case kObjectID_StreamOut: return StreamHas(a->mSelector);
    default: return false;
    }
}

static Boolean ObjectKnown(AudioObjectID obj) {
    return obj >= kObjectID_PlugIn && obj <= kObjectID_StreamOut;
}

#pragma mark Property answers

static OSStatus PlugInProperty(const AudioObjectPropertyAddress *a, UInt32 qSize, const void *q,
                               UInt32 inSize, UInt32 *outSize, void *outData) {
    static const AudioObjectID kDevices[] = {kObjectID_Device};
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: return Put32(outData, inSize, outSize, kAudioObjectClassID);
    case kAudioObjectPropertyClass: return Put32(outData, inSize, outSize, kAudioPlugInClassID);
    case kAudioObjectPropertyOwner: return Put32(outData, inSize, outSize, kAudioObjectUnknown);
    case kAudioObjectPropertyName: return PutString(outData, inSize, outSize, CFSTR("GT10Audio"));
    case kAudioObjectPropertyManufacturer:
        return PutString(outData, inSize, outSize, CFSTR("Lukas Malkmus"));
    case kAudioObjectPropertyOwnedObjects:
        return PutArray(outData, inSize, outSize, kDevices,
                        ClassWanted(kAudioDeviceClassID, qSize, q) ? 1 : 0, sizeof(AudioObjectID));
    case kAudioPlugInPropertyDeviceList:
        return PutArray(outData, inSize, outSize, kDevices, 1, sizeof(AudioObjectID));
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        if (qSize != sizeof(CFStringRef) || q == NULL || *(CFStringRef const *)q == NULL)
            return kAudioHardwareIllegalOperationError;
        const Boolean match =
            CFStringCompare(*(CFStringRef const *)q, CFSTR(kDevice_UID), 0) == kCFCompareEqualTo;
        return Put32(outData, inSize, outSize, match ? kObjectID_Device : kAudioObjectUnknown);
    }
    case kAudioPlugInPropertyResourceBundle: return PutString(outData, inSize, outSize, CFSTR(""));
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus DeviceProperty(const AudioObjectPropertyAddress *a, UInt32 qSize, const void *q,
                               UInt32 inSize, UInt32 *outSize, void *outData) {
    static const AudioObjectID kBoth[] = {kObjectID_StreamIn, kObjectID_StreamOut};
    static const AudioObjectID kIn[]   = {kObjectID_StreamIn};
    static const AudioObjectID kOut[]  = {kObjectID_StreamOut};
    static const AudioObjectID kSelf[] = {kObjectID_Device};
    static const UInt32 kStereo[]      = {1, 2};

    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: return Put32(outData, inSize, outSize, kAudioObjectClassID);
    case kAudioObjectPropertyClass: return Put32(outData, inSize, outSize, kAudioDeviceClassID);
    case kAudioObjectPropertyOwner: return Put32(outData, inSize, outSize, kObjectID_PlugIn);
    case kAudioObjectPropertyName: return PutString(outData, inSize, outSize, CFSTR("BOSS GT-10"));
    case kAudioObjectPropertyManufacturer:
        return PutString(outData, inSize, outSize, CFSTR("Roland"));
    case kAudioObjectPropertyOwnedObjects:
        if (!ClassWanted(kAudioStreamClassID, qSize, q))
            return PutArray(outData, inSize, outSize, NULL, 0, sizeof(AudioObjectID));
        // fall through, the streams are the only owned objects
    case kAudioDevicePropertyStreams:
        switch (a->mScope) {
        case kAudioObjectPropertyScopeInput:
            return PutArray(outData, inSize, outSize, kIn, 1, sizeof(AudioObjectID));
        case kAudioObjectPropertyScopeOutput:
            return PutArray(outData, inSize, outSize, kOut, 1, sizeof(AudioObjectID));
        default: return PutArray(outData, inSize, outSize, kBoth, 2, sizeof(AudioObjectID));
        }
    case kAudioObjectPropertyControlList:
        return PutArray(outData, inSize, outSize, NULL, 0, sizeof(AudioObjectID));
    case kAudioDevicePropertyDeviceUID:
        return PutString(outData, inSize, outSize, CFSTR(kDevice_UID));
    case kAudioDevicePropertyModelUID:
        return PutString(outData, inSize, outSize, CFSTR(kDevice_ModelUID));
    case kAudioDevicePropertyTransportType:
        return Put32(outData, inSize, outSize, kAudioDeviceTransportTypeUSB);
    case kAudioDevicePropertyRelatedDevices:
        return PutArray(outData, inSize, outSize, kSelf, 1, sizeof(AudioObjectID));
    case kAudioDevicePropertyClockDomain: return Put32(outData, inSize, outSize, 0);
    case kAudioDevicePropertyDeviceIsAlive:
        return Put32(outData, inSize, outSize, atomic_load(&gUSBHeld) ? 1 : 0);
    case kAudioDevicePropertyDeviceIsRunning: {
        pthread_mutex_lock(&gStateMutex);
        const UInt32 running = gIOClients > 0;
        pthread_mutex_unlock(&gStateMutex);
        return Put32(outData, inSize, outSize, running);
    }
    case kAudioDevicePropertyDeviceCanBeDefaultDevice: return Put32(outData, inSize, outSize, 1);
    // A guitar pedal must not become the alert and system sound output.
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        return Put32(outData, inSize, outSize, 0);
    case kAudioDevicePropertyLatency: return Put32(outData, inSize, outSize, 0);
    case kAudioDevicePropertySafetyOffset:
        switch (a->mScope) {
        case kAudioObjectPropertyScopeInput:
            return Put32(outData, inSize, outSize, kInputSafetyOffset);
        case kAudioObjectPropertyScopeOutput:
            return Put32(outData, inSize, outSize, kOutputSafetyOffset);
        default: return Put32(outData, inSize, outSize, 0);
        }
    case kAudioDevicePropertyNominalSampleRate:
        return PutF64(outData, inSize, outSize, kSampleRate);
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        const AudioValueRange r = {kSampleRate, kSampleRate};
        return PutArray(outData, inSize, outSize, &r, 1, sizeof r);
    }
    case kAudioDevicePropertyIsHidden: return Put32(outData, inSize, outSize, 0);
    case kAudioDevicePropertyPreferredChannelsForStereo:
        return PutBytes(outData, inSize, outSize, kStereo, sizeof kStereo);
    case kAudioDevicePropertyPreferredChannelLayout: {
        struct {
            AudioChannelLayout layout;
            AudioChannelDescription second;
        } l;
        memset(&l, 0, sizeof l);
        l.layout.mChannelLayoutTag          = kAudioChannelLayoutTag_UseChannelDescriptions;
        l.layout.mNumberChannelDescriptions = kChannels;
        l.layout.mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
        l.second.mChannelLabel                         = kAudioChannelLabel_Right;
        return PutBytes(outData, inSize, outSize, &l, sizeof l);
    }
    case kAudioDevicePropertyZeroTimeStampPeriod:
        return Put32(outData, inSize, outSize, kGT10ZeroTimeStampPeriod);
    case kAudioObjectPropertyCustomPropertyInfoList: {
        static const AudioServerPlugInCustomPropertyInfo kCustom[] = {
            {kGT10PropertyDiagnostics, kAudioServerPlugInCustomPropertyDataTypeCFString,
             kAudioServerPlugInCustomPropertyDataTypeNone},
        };
        return PutArray(outData, inSize, outSize, kCustom, 1, sizeof kCustom[0]);
    }
    case kGT10PropertyDiagnostics: {
        static const char *kSources[] = {"stopped", "synthetic", "device", "?"};
        const UInt64 clock            = atomic_load(&gClock);
        CFStringRef text              = CFStringCreateWithFormat(
            NULL, NULL,
            CFSTR("clock %s seed %llu | reads %llu silent %llu lastPos %lld valid %llu | "
                                            "writes %llu lastPos %lld | captureEnd %lld dropped %llu "
                                            "stray %llu"),
            kSources[clock & 3], (unsigned long long)ClockSeed(clock),
            (unsigned long long)atomic_load(&gReadCalls),
            (unsigned long long)atomic_load(&gReadSilent),
            (long long)atomic_load(&gLastReadPosition),
            (unsigned long long)atomic_load(&gLastReadValid),
            (unsigned long long)atomic_load(&gWriteCalls),
            (long long)atomic_load(&gLastWritePosition),
            (long long)(gCaptureRing != NULL ? GT10RingEnd(gCaptureRing) : 0),
            (unsigned long long)(gCaptureRing != NULL ? GT10RingDropped(gCaptureRing) : 0),
            (unsigned long long)atomic_load(&gStrayPositions));
        if (text == NULL) return kAudioHardwareIllegalOperationError;
        *outSize = sizeof(CFStringRef);
        if (outData == NULL) {
            CFRelease(text);
            return kAudioHardwareNoError;
        }
        if (inSize < sizeof(CFStringRef)) {
            CFRelease(text);
            return kAudioHardwareBadPropertySizeError;
        }
        *(CFStringRef *)outData = text;  // the caller owns it
        return kAudioHardwareNoError;
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus StreamProperty(AudioObjectID obj, const AudioObjectPropertyAddress *a,
                               UInt32 inSize, UInt32 *outSize, void *outData) {
    const Boolean input = obj == kObjectID_StreamIn;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: return Put32(outData, inSize, outSize, kAudioObjectClassID);
    case kAudioObjectPropertyClass: return Put32(outData, inSize, outSize, kAudioStreamClassID);
    case kAudioObjectPropertyOwner: return Put32(outData, inSize, outSize, kObjectID_Device);
    case kAudioObjectPropertyName:
        return PutString(outData, inSize, outSize,
                         input ? CFSTR("GT-10 Input") : CFSTR("GT-10 Output"));
    case kAudioObjectPropertyOwnedObjects:
        return PutArray(outData, inSize, outSize, NULL, 0, sizeof(AudioObjectID));
    case kAudioStreamPropertyIsActive: return Put32(outData, inSize, outSize, 1);
    case kAudioStreamPropertyDirection: return Put32(outData, inSize, outSize, input ? 1 : 0);
    case kAudioStreamPropertyTerminalType:
        return Put32(outData, inSize, outSize, kAudioStreamTerminalTypeLine);
    case kAudioStreamPropertyStartingChannel: return Put32(outData, inSize, outSize, 1);
    case kAudioStreamPropertyLatency: return Put32(outData, inSize, outSize, 0);
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        const AudioStreamBasicDescription f = StreamFormat();
        return PutBytes(outData, inSize, outSize, &f, sizeof f);
    }
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        AudioStreamRangedDescription r;
        r.mFormat                   = StreamFormat();
        r.mSampleRateRange.mMinimum = kSampleRate;
        r.mSampleRateRange.mMaximum = kSampleRate;
        return PutArray(outData, inSize, outSize, &r, 1, sizeof r);
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus ObjectProperty(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qSize,
                               const void *q, UInt32 inSize, UInt32 *outSize, void *outData) {
    if (!ObjectKnown(obj)) return kAudioHardwareBadObjectError;
    if (!ObjectHas(obj, a)) return kAudioHardwareUnknownPropertyError;
    switch (obj) {
    case kObjectID_PlugIn: return PlugInProperty(a, qSize, q, inSize, outSize, outData);
    case kObjectID_Device: return DeviceProperty(a, qSize, q, inSize, outSize, outData);
    default: return StreamProperty(obj, a, inSize, outSize, outData);
    }
}

#pragma mark Property entry points

static Boolean GT10_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                pid_t inClientProcessID,
                                const AudioObjectPropertyAddress *inAddress) {
    (void)inDriver;
    (void)inClientProcessID;
    if (inAddress == NULL) return false;
    return ObjectHas(inObjectID, inAddress);
}

static OSStatus GT10_IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                        AudioObjectID inObjectID, pid_t inClientProcessID,
                                        const AudioObjectPropertyAddress *inAddress,
                                        Boolean *outIsSettable) {
    (void)inDriver;
    (void)inClientProcessID;
    if (inAddress == NULL || outIsSettable == NULL) return kAudioHardwareIllegalOperationError;
    if (!ObjectKnown(inObjectID)) return kAudioHardwareBadObjectError;
    if (!ObjectHas(inObjectID, inAddress)) return kAudioHardwareUnknownPropertyError;

    // Clients set these to the value they already hold. Advertising them as
    // settable and accepting only that value keeps such clients working.
    switch (inAddress->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: *outIsSettable = true; break;
    default: *outIsSettable = false;
    }
    return kAudioHardwareNoError;
}

static OSStatus GT10_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                         AudioObjectID inObjectID, pid_t inClientProcessID,
                                         const AudioObjectPropertyAddress *inAddress,
                                         UInt32 inQualifierDataSize, const void *inQualifierData,
                                         UInt32 *outDataSize) {
    (void)inDriver;
    (void)inClientProcessID;
    if (inAddress == NULL || outDataSize == NULL) return kAudioHardwareIllegalOperationError;
    return ObjectProperty(inObjectID, inAddress, inQualifierDataSize, inQualifierData, 0,
                          outDataSize, NULL);
}

static OSStatus GT10_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                     pid_t inClientProcessID,
                                     const AudioObjectPropertyAddress *inAddress,
                                     UInt32 inQualifierDataSize, const void *inQualifierData,
                                     UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)inDriver;
    (void)inClientProcessID;
    if (inAddress == NULL || outDataSize == NULL || outData == NULL)
        return kAudioHardwareIllegalOperationError;
    return ObjectProperty(inObjectID, inAddress, inQualifierDataSize, inQualifierData, inDataSize,
                          outDataSize, outData);
}

static OSStatus GT10_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                     pid_t inClientProcessID,
                                     const AudioObjectPropertyAddress *inAddress,
                                     UInt32 inQualifierDataSize, const void *inQualifierData,
                                     UInt32 inDataSize, const void *inData) {
    (void)inDriver;
    (void)inClientProcessID;
    (void)inQualifierDataSize;
    (void)inQualifierData;
    if (inAddress == NULL || inData == NULL) return kAudioHardwareIllegalOperationError;
    if (!ObjectKnown(inObjectID)) return kAudioHardwareBadObjectError;
    if (!ObjectHas(inObjectID, inAddress)) return kAudioHardwareUnknownPropertyError;

    switch (inAddress->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return *(const Float64 *)inData == kSampleRate ? kAudioHardwareNoError
                                                       : kAudioDeviceUnsupportedFormatError;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        if (inDataSize != sizeof(AudioStreamBasicDescription))
            return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription f = StreamFormat();
        return SameFormat((const AudioStreamBasicDescription *)inData, &f)
                   ? kAudioHardwareNoError
                   : kAudioDeviceUnsupportedFormatError;
    }
    default: return kAudioHardwareUnsupportedOperationError;
    }
}

#pragma mark IO

static OSStatus GT10_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                             UInt32 inClientID) {
    (void)inDriver;
    (void)inClientID;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    OSStatus err = kAudioHardwareNoError;
    pthread_mutex_lock(&gStateMutex);
    if (gIOClients == UINT64_MAX) {
        err = kAudioHardwareIllegalOperationError;
    } else if (gIOClients == 0) {
        // First client anchors the synthetic timeline, which runs until the
        // device clock takes over. Later clients join whichever runs.
        atomic_store(&gAnchorHostTime, mach_absolute_time());
        ClockMove(1u << kClockStopped, kClockSynthetic);
        gIOClients = 1;
        StartStream();
    } else {
        gIOClients += 1;
    }
    pthread_mutex_unlock(&gStateMutex);
    return err;
}

static OSStatus GT10_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                            UInt32 inClientID) {
    (void)inDriver;
    (void)inClientID;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    OSStatus err = kAudioHardwareNoError;
    pthread_mutex_lock(&gStateMutex);
    if (gIOClients == 0) {
        err = kAudioHardwareIllegalOperationError;
    } else {
        gIOClients -= 1;
        if (gIOClients == 0) {
            StopStream();
            ClockMove((1u << kClockSynthetic) | (1u << kClockDevice), kClockStopped);
        }
    }
    pthread_mutex_unlock(&gStateMutex);
    return err;
}

// The most recent period boundary at or before now on the synthetic clock,
// computed directly so a stalled IO thread costs one division rather than one
// step per missed period.
static void SyntheticZeroTimeStamp(Float64 *sampleTime, UInt64 *hostTime) {
    const UInt64 anchor          = atomic_load(&gAnchorHostTime);
    const Float64 ticksPerPeriod = gHostTicksPerFrame * (Float64)kGT10ZeroTimeStampPeriod;
    const UInt64 now             = mach_absolute_time();
    const UInt64 elapsed         = now > anchor ? now - anchor : 0;
    const UInt64 count           = (UInt64)((Float64)elapsed / ticksPerPeriod);
    *sampleTime                  = (Float64)count * (Float64)kGT10ZeroTimeStampPeriod;
    *hostTime                    = anchor + (UInt64)((Float64)count * ticksPerPeriod);
}

// Real-time. No lock, no allocation, no logging.
static OSStatus GT10_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                      Float64 *outSampleTime, UInt64 *outHostTime,
                                      UInt64 *outSeed) {
    (void)inDriver;
    (void)inClientID;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (outSampleTime == NULL || outHostTime == NULL || outSeed == NULL)
        return kAudioHardwareIllegalOperationError;

    // The clock word is read again after the payload. A stop and restart in
    // between would otherwise pair one run's seed with the next run's time.
    for (int attempt = 0; attempt < 4; attempt++) {
        const UInt64 clock = atomic_load(&gClock);
        if (ClockSource(clock) == kClockStopped) return kAudioHardwareNotRunningError;

        Float64 sampleTime;
        UInt64 hostTime;
        if (ClockSource(clock) == kClockDevice) {
            // Committed only after its first timestamp, and cleared only while
            // another clock runs.
            int64_t sample;
            uint64_t host;
            if (!GT10TimeStampRead(gTimeStamp, &sample, &host)) continue;
            sampleTime = (Float64)sample;
            hostTime   = host;
        } else {
            SyntheticZeroTimeStamp(&sampleTime, &hostTime);
        }
        if (atomic_load(&gClock) != clock) continue;
        *outSampleTime = sampleTime;
        *outHostTime   = hostTime;
        *outSeed       = ClockSeed(clock);
        return kAudioHardwareNoError;
    }
    return kAudioHardwareNotReadyError;
}

static OSStatus GT10_WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                                       AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                       UInt32 inOperationID, Boolean *outWillDo,
                                       Boolean *outWillDoInPlace) {
    (void)inDriver;
    (void)inClientID;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (outWillDo == NULL || outWillDoInPlace == NULL) return kAudioHardwareIllegalOperationError;

    *outWillDo = inOperationID == kAudioServerPlugInIOOperationReadInput ||
                 inOperationID == kAudioServerPlugInIOOperationWriteMix;
    *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus GT10_BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                      UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                      const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inDriver;
    (void)inClientID;
    (void)inOperationID;
    (void)inIOBufferFrameSize;
    (void)inIOCycleInfo;
    return inDeviceObjectID == kObjectID_Device ? kAudioHardwareNoError
                                                : kAudioHardwareBadObjectError;
}

// The HAL hands over a sample time as a double. llround of a NaN, an infinity,
// or a value past the range of int64_t is undefined, and a position near the
// end of that range overflows the ring's own arithmetic. Half the range leaves
// room for every position plus a buffer, and is still 3 million years of audio.
static const int64_t kMaxSamplePosition = INT64_MAX / 2;

static bool SamplePosition(Float64 time, int64_t *out) {
    if (!isfinite(time) || time < -(Float64)kMaxSamplePosition ||
        time > (Float64)kMaxSamplePosition) {
        return false;
    }
    *out = llround(time);
    return true;
}

// A cycle can fetch its sample time on one timeline and reach the ring after
// the clock has moved to the other. That position means nothing here, and the
// ring keeps it: the reader's floor only rises, and the writer's end only moves
// forward. One stray position would silence the device for the rest of the
// session, so a position that far from the ring is refused.
static bool PositionOnTimeline(const GT10Ring *ring, int64_t position, UInt32 frames) {
    const int64_t end = GT10RingEnd(ring);
    return position <= end + (int64_t)kRingFrames &&
           position + (int64_t)frames >= end - (int64_t)kRingFrames;
}

// Both IO operations count themselves in before they read the clock, so a
// ring reset waits for them. Only the device clock owns ring content.
static void ReadInput(int64_t position, void *buffer, UInt32 frames) {
    atomic_fetch_add(&gIOActive, 1);
    uint32_t valid      = 0;
    const bool onDevice = ClockSource(atomic_load(&gClock)) == kClockDevice;
    if (onDevice && PositionOnTimeline(gCaptureRing, position, frames)) {
        valid = GT10RingRead(gCaptureRing, position, buffer, frames);
    } else {
        if (onDevice) atomic_fetch_add(&gStrayPositions, 1);
        memset(buffer, 0, (size_t)frames * kBytesPerFrame);
    }
    atomic_fetch_sub(&gIOActive, 1);

    atomic_fetch_add(&gReadCalls, 1);
    if (valid == 0) atomic_fetch_add(&gReadSilent, 1);
    atomic_store(&gLastReadPosition, position);
    atomic_store(&gLastReadValid, valid);
}

static void WriteMix(int64_t position, const void *buffer, UInt32 frames) {
    atomic_fetch_add(&gIOActive, 1);
    const bool onDevice = ClockSource(atomic_load(&gClock)) == kClockDevice;
    if (onDevice && PositionOnTimeline(gPlaybackRing, position, frames)) {
        GT10RingWrite(gPlaybackRing, position, buffer, frames);
    } else if (onDevice) {
        atomic_fetch_add(&gStrayPositions, 1);
    }
    atomic_fetch_sub(&gIOActive, 1);

    atomic_fetch_add(&gWriteCalls, 1);
    atomic_store(&gLastWritePosition, position);
}

static OSStatus GT10_DoIOOperation(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID,
                                   UInt32 inClientID, UInt32 inOperationID,
                                   UInt32 inIOBufferFrameSize,
                                   const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                                   void *ioMainBuffer, void *ioSecondaryBuffer) {
    (void)inDriver;
    (void)inClientID;
    (void)ioSecondaryBuffer;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (inStreamObjectID != kObjectID_StreamIn && inStreamObjectID != kObjectID_StreamOut)
        return kAudioHardwareBadObjectError;
    if (ioMainBuffer == NULL || inIOCycleInfo == NULL) return kAudioHardwareIllegalOperationError;

    switch (inOperationID) {
    case kAudioServerPlugInIOOperationReadInput: {
        if (inStreamObjectID != kObjectID_StreamIn) return kAudioHardwareIllegalOperationError;
        int64_t position;
        if (!SamplePosition(inIOCycleInfo->mInputTime.mSampleTime, &position))
            return kAudioHardwareIllegalOperationError;
        ReadInput(position, ioMainBuffer, inIOBufferFrameSize);
        return kAudioHardwareNoError;
    }
    case kAudioServerPlugInIOOperationWriteMix: {
        if (inStreamObjectID != kObjectID_StreamOut) return kAudioHardwareIllegalOperationError;
        int64_t position;
        if (!SamplePosition(inIOCycleInfo->mOutputTime.mSampleTime, &position))
            return kAudioHardwareIllegalOperationError;
        WriteMix(position, ioMainBuffer, inIOBufferFrameSize);
        return kAudioHardwareNoError;
    }
    default: return kAudioHardwareUnsupportedOperationError;
    }
}

static OSStatus GT10_EndIOOperation(AudioServerPlugInDriverRef inDriver,
                                    AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                    UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                    const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inDriver;
    (void)inClientID;
    (void)inOperationID;
    (void)inIOBufferFrameSize;
    (void)inIOCycleInfo;
    return inDeviceObjectID == kObjectID_Device ? kAudioHardwareNoError
                                                : kAudioHardwareBadObjectError;
}

#pragma mark Interface

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    GT10_QueryInterface,
    GT10_AddRef,
    GT10_Release,
    GT10_Initialize,
    GT10_CreateDevice,
    GT10_DestroyDevice,
    GT10_AddDeviceClient,
    GT10_RemoveDeviceClient,
    GT10_PerformDeviceConfigurationChange,
    GT10_AbortDeviceConfigurationChange,
    GT10_HasProperty,
    GT10_IsPropertySettable,
    GT10_GetPropertyDataSize,
    GT10_GetPropertyData,
    GT10_SetPropertyData,
    GT10_StartIO,
    GT10_StopIO,
    GT10_GetZeroTimeStamp,
    GT10_WillDoIOOperation,
    GT10_BeginIOOperation,
    GT10_DoIOOperation,
    GT10_EndIOOperation,
};

static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef           = &gInterfacePtr;

static HRESULT GT10_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    // OSStatus codes such as kAudioHardwareBadObjectError are positive, and
    // SUCCEEDED() is hr >= 0, so only real HRESULTs may leave this function.
    if (outInterface == NULL) return E_POINTER;
    *outInterface = NULL;
    if (inDriver != gDriverRef) return E_INVALIDARG;

    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (requested == NULL) return E_INVALIDARG;
    const Boolean wanted = CFEqual(requested, IUnknownUUID) ||
                           CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(requested);

    if (!wanted) return E_NOINTERFACE;

    atomic_fetch_add(&gRefCount, 1);
    *outInterface = gDriverRef;
    return S_OK;
}

static ULONG GT10_AddRef(void *inDriver) {
    if (inDriver != gDriverRef) return 0;
    return atomic_fetch_add(&gRefCount, 1) + 1;
}

// The object is static, so zero has no destructor attached. The count is
// still kept exact because a later USB lifecycle must not hang off a count
// that can underflow.
static ULONG GT10_Release(void *inDriver) {
    if (inDriver != gDriverRef) return 0;
    UInt32 count = atomic_load(&gRefCount);
    while (count != 0 && !atomic_compare_exchange_weak(&gRefCount, &count, count - 1)) {
    }
    return count == 0 ? 0 : count - 1;
}

void *GT10AudioFactory(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID);

void *GT10AudioFactory(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID) {
    (void)allocator;
    if (requestedTypeUUID == NULL) return NULL;

    const Boolean isDriver = CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID);
    GT10_LOG("factory entered, driver type %d", (int)isDriver);
    if (!isDriver) return NULL;

    // The returned IUnknown is a reference the host releases later.
    atomic_fetch_add(&gRefCount, 1);
    return gDriverRef;
}
