// Drives the plug-in through its COM table the way the HAL does, without
// coreaudiod. Links Driver/GT10Audio.c directly, so the static entry points
// are reachable only through the interface, which is the contract under test.

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../Driver/GT10USBStream.h"
#include "../Sources/GT10Clock.h"
#include "../Sources/GT10Ring.h"

void *GT10AudioFactory(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID);

void FakeUSBSetHeld(bool held);
void FakeUSBFault(void);
bool FakeUSBAnnounceAgain(void);
int FakeUSBPrepares(void);
void FakeUSBSetStartResult(int result);
void FakeUSBSetPublishOnStart(bool publish);
bool FakeUSBStreaming(void);
const GT10StreamConfig *FakeUSBConfig(void);
int FakeUSBStreamStarts(void);
int FakeUSBStreamStops(void);

static int gChecks   = 0;
static int gFailures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        gChecks++;                                                                                 \
        if (!(cond)) {                                                                             \
            gFailures++;                                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

// For a precondition the rest of the run depends on. CHECK keeps going so one
// run reports every failure, which is right for an assertion about a result and
// wrong for the pointer every later line dereferences.
#define REQUIRE(cond)                                                                              \
    do {                                                                                           \
        gChecks++;                                                                                 \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static AudioServerPlugInDriverRef gRef;
static AudioServerPlugInClientInfo gClient;
static AudioServerPlugInIOCycleInfo gCycle;
static int gChangeInfo;

static _Atomic int gAliveNotifications = 0;

static OSStatus HostPropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n,
                                      const AudioObjectPropertyAddress *a) {
    (void)h;
    for (UInt32 i = 0; i < n; i++) {
        if (o == 2 && a[i].mSelector == kAudioDevicePropertyDeviceIsAlive) gAliveNotifications++;
    }
    return kAudioHardwareNoError;
}
static OSStatus HostCopyFromStorage(AudioServerPlugInHostRef h, CFStringRef k,
                                    CFPropertyListRef *out) {
    (void)h;
    (void)k;
    *out = NULL;
    return kAudioHardwareNoError;
}
static OSStatus HostWriteToStorage(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef v) {
    (void)h;
    (void)k;
    (void)v;
    return kAudioHardwareNoError;
}
static OSStatus HostDeleteFromStorage(AudioServerPlugInHostRef h, CFStringRef k) {
    (void)h;
    (void)k;
    return kAudioHardwareNoError;
}
static OSStatus HostRequestChange(AudioServerPlugInHostRef h, AudioObjectID d, UInt64 a, void *i) {
    (void)h;
    (void)d;
    (void)a;
    (void)i;
    return kAudioHardwareNoError;
}
static AudioServerPlugInHostInterface gHostInterface = {
    HostPropertiesChanged, HostCopyFromStorage, HostWriteToStorage,
    HostDeleteFromStorage, HostRequestChange,
};

static AudioServerPlugInIOCycleInfo CycleAt(Float64 inputSampleTime) {
    AudioServerPlugInIOCycleInfo c;
    memset(&c, 0, sizeof c);
    c.mInputTime.mSampleTime = inputSampleTime;
    c.mInputTime.mFlags      = kAudioTimeStampSampleTimeValid;
    return c;
}

static AudioServerPlugInIOCycleInfo CycleOut(Float64 outputSampleTime) {
    AudioServerPlugInIOCycleInfo c;
    memset(&c, 0, sizeof c);
    c.mOutputTime.mSampleTime = outputSampleTime;
    c.mOutputTime.mFlags      = kAudioTimeStampSampleTimeValid;
    return c;
}

static void TestFactory(void) {
    CFUUIDRef wrong = CFUUIDCreateFromString(NULL, CFSTR("00000000-0000-0000-0000-000000000000"));
    CHECK(GT10AudioFactory(NULL, wrong) == NULL);
    CFRelease(wrong);
    CHECK(GT10AudioFactory(NULL, NULL) == NULL);

    gRef = GT10AudioFactory(NULL, kAudioServerPlugInTypeUUID);
    REQUIRE(gRef != NULL);
    CHECK((*gRef)->QueryInterface != NULL && (*gRef)->Initialize != NULL);

    // The factory hands out one counted reference.
    CHECK((*gRef)->AddRef(gRef) == 2);
    CHECK((*gRef)->Release(gRef) == 1);
}

static void TestQueryInterface(void) {
    void *out  = NULL;
    HRESULT hr = (*gRef)->QueryInterface(gRef, CFUUIDGetUUIDBytes(IUnknownUUID), NULL);
    CHECK(hr == E_POINTER);
    CHECK(FAILED(hr));

    // A refused query must also clear the output, whatever the reason.
    int garbage = 0;
    out         = &garbage;
    hr          = (*gRef)->QueryInterface(&garbage, CFUUIDGetUUIDBytes(IUnknownUUID), &out);
    CHECK(FAILED(hr));
    CHECK(out == NULL);

    CFUUIDRef unknown = CFUUIDCreateFromString(NULL, CFSTR("11111111-2222-3333-4444-555555555555"));
    out               = &garbage;
    hr                = (*gRef)->QueryInterface(gRef, CFUUIDGetUUIDBytes(unknown), &out);
    CFRelease(unknown);
    CHECK(hr == E_NOINTERFACE);
    CHECK(out == NULL);

    out = NULL;
    hr  = (*gRef)->QueryInterface(gRef, CFUUIDGetUUIDBytes(IUnknownUUID), &out);
    CHECK(hr == S_OK);
    CHECK(out == gRef);
    // factory (1) + this query (2).
    CHECK((*gRef)->AddRef(gRef) == 3);
    CHECK((*gRef)->Release(gRef) == 2);

    out = NULL;
    hr  = (*gRef)->QueryInterface(gRef, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID),
                                  &out);
    CHECK(hr == S_OK);
    CHECK(out == gRef);
    CHECK((*gRef)->Release(gRef) == 2);
    CHECK((*gRef)->Release(gRef) == 1);
}

static void TestReleaseFloor(void) {
    CHECK((*gRef)->Release(gRef) == 0);
    CHECK((*gRef)->Release(gRef) == 0);
    CHECK((*gRef)->AddRef(gRef) == 1);
    int other = 0;
    CHECK((*gRef)->AddRef(&other) == 0);
    CHECK((*gRef)->Release(&other) == 0);
    CHECK((*gRef)->Release(gRef) == 0);
}

enum { kThreads = 8, kIterations = 200000 };

static void *Churn(void *arg) {
    (void)arg;
    for (int i = 0; i < kIterations; i++) {
        (*gRef)->AddRef(gRef);
        (*gRef)->Release(gRef);
    }
    return NULL;
}

static void TestConcurrentRefCount(void) {
    // Hold one reference so the churn never touches the zero floor, which
    // would mask a lost decrement.
    CHECK((*gRef)->AddRef(gRef) == 1);
    pthread_t threads[kThreads];
    for (int i = 0; i < kThreads; i++)
        pthread_create(&threads[i], NULL, Churn, NULL);
    for (int i = 0; i < kThreads; i++)
        pthread_join(threads[i], NULL);
    // Read the count before the last release. A lost increment clamps here at
    // the zero floor, a stray one pushes it above 2.
    CHECK((*gRef)->AddRef(gRef) == 2);
    CHECK((*gRef)->Release(gRef) == 1);
    CHECK((*gRef)->Release(gRef) == 0);
}

enum { kPlugIn = kAudioObjectPlugInObject, kDevice = 2, kStreamIn = 3, kStreamOut = 4 };

static AudioObjectPropertyAddress Addr(AudioObjectPropertySelector s,
                                       AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = {s, scope, kAudioObjectPropertyElementMain};
    return a;
}

#define GLOBAL kAudioObjectPropertyScopeGlobal
#define INPUT  kAudioObjectPropertyScopeInput
#define OUTPUT kAudioObjectPropertyScopeOutput

static OSStatus Get(AudioObjectID obj, AudioObjectPropertyAddress a, UInt32 inSize, UInt32 *outSize,
                    void *out) {
    return (*gRef)->GetPropertyData(gRef, obj, 0, &a, 0, NULL, inSize, outSize, out);
}

static OSStatus GetSize(AudioObjectID obj, AudioObjectPropertyAddress a, UInt32 *size) {
    return (*gRef)->GetPropertyDataSize(gRef, obj, 0, &a, 0, NULL, size);
}

static UInt32 Get32(AudioObjectID obj, AudioObjectPropertySelector s,
                    AudioObjectPropertyScope scope) {
    UInt32 v = 0xDEADBEEF, size = 0;
    CHECK(Get(obj, Addr(s, scope), sizeof v, &size, &v) == kAudioHardwareNoError);
    CHECK(size == sizeof v);
    return v;
}

static UInt32 GetIDs(AudioObjectID obj, AudioObjectPropertySelector s,
                     AudioObjectPropertyScope scope, AudioObjectID *ids, UInt32 max) {
    UInt32 size = 0;
    CHECK(Get(obj, Addr(s, scope), max * sizeof(AudioObjectID), &size, ids) ==
          kAudioHardwareNoError);
    return size / sizeof(AudioObjectID);
}

// Arrays fill a short buffer partially instead of refusing it.
static Boolean IsArrayProperty(AudioObjectPropertySelector s) {
    switch (s) {
    case kAudioObjectPropertyOwnedObjects:
    case kAudioObjectPropertyControlList:
    case kAudioPlugInPropertyDeviceList:
    case kAudioDevicePropertyStreams:
    case kAudioDevicePropertyRelatedDevices:
    case kAudioDevicePropertyAvailableNominalSampleRates:
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: return true;
    default: return false;
    }
}

// Every advertised property must answer both the size query and the data
// query, and a buffer one byte short must be refused for scalar payloads.
static void WalkObject(AudioObjectID obj, const AudioObjectPropertySelector *sels, int n,
                       AudioObjectPropertyScope scope) {
    for (int i = 0; i < n; i++) {
        AudioObjectPropertyAddress a = Addr(sels[i], scope);
        CHECK((*gRef)->HasProperty(gRef, obj, 0, &a));
        UInt32 size = 0;
        CHECK(GetSize(obj, a, &size) == kAudioHardwareNoError);
        UInt8 buf[256];
        UInt32 got = 0;
        CHECK(size <= sizeof buf);
        OSStatus err = Get(obj, a, size, &got, buf);
        CHECK(err == kAudioHardwareNoError);
        CHECK(got == size);
        if (size > 0 && !IsArrayProperty(sels[i])) {
            CHECK(Get(obj, a, size - 1, &got, buf) == kAudioHardwareBadPropertySizeError);
        }
        Boolean settable = true;
        CHECK((*gRef)->IsPropertySettable(gRef, obj, 0, &a, &settable) == kAudioHardwareNoError);
    }
}

static void TestPlugInProperties(void) {
    static const AudioObjectPropertySelector sels[] = {
        kAudioObjectPropertyBaseClass,    kAudioObjectPropertyClass,
        kAudioObjectPropertyOwner,        kAudioObjectPropertyName,
        kAudioObjectPropertyManufacturer, kAudioObjectPropertyOwnedObjects,
        kAudioPlugInPropertyDeviceList,   kAudioPlugInPropertyResourceBundle,
    };
    WalkObject(kPlugIn, sels, 8, GLOBAL);

    CHECK(Get32(kPlugIn, kAudioObjectPropertyBaseClass, GLOBAL) == kAudioObjectClassID);
    CHECK(Get32(kPlugIn, kAudioObjectPropertyClass, GLOBAL) == kAudioPlugInClassID);

    AudioObjectID ids[4];
    CHECK(GetIDs(kPlugIn, kAudioPlugInPropertyDeviceList, GLOBAL, ids, 4) == 1);
    CHECK(ids[0] == kDevice);

    // UID translation needs a real qualifier.
    AudioObjectPropertyAddress a = Addr(kAudioPlugInPropertyTranslateUIDToDevice, GLOBAL);
    CFStringRef uid              = CFSTR("GT10Audio:Device");
    AudioObjectID dev            = 0;
    UInt32 size                  = 0;
    CHECK((*gRef)->GetPropertyData(gRef, kPlugIn, 0, &a, sizeof uid, &uid, sizeof dev, &size,
                                   &dev) == kAudioHardwareNoError);
    CHECK(dev == kDevice);
    CFStringRef other = CFSTR("nope");
    CHECK((*gRef)->GetPropertyData(gRef, kPlugIn, 0, &a, sizeof other, &other, sizeof dev, &size,
                                   &dev) == kAudioHardwareNoError);
    CHECK(dev == kAudioObjectUnknown);
    CHECK((*gRef)->GetPropertyData(gRef, kPlugIn, 0, &a, 0, NULL, sizeof dev, &size, &dev) !=
          kAudioHardwareNoError);

    // Nonexistent addresses and objects.
    a = Addr(kAudioObjectPropertyManufacturer, INPUT);
    CHECK(!(*gRef)->HasProperty(gRef, kPlugIn, 0, &a));
    a          = Addr(kAudioObjectPropertyManufacturer, GLOBAL);
    a.mElement = 123;
    CHECK(!(*gRef)->HasProperty(gRef, kPlugIn, 0, &a));
    a = Addr('zzzz', GLOBAL);
    CHECK(!(*gRef)->HasProperty(gRef, kPlugIn, 0, &a));
    CHECK(GetSize(kPlugIn, a, &size) == kAudioHardwareUnknownPropertyError);
    a = Addr(kAudioObjectPropertyClass, GLOBAL);
    CHECK(!(*gRef)->HasProperty(gRef, 99, 0, &a));
    CHECK(GetSize(99, a, &size) == kAudioHardwareBadObjectError);
    Boolean settable = true;
    CHECK((*gRef)->IsPropertySettable(gRef, 99, 0, &a, &settable) == kAudioHardwareBadObjectError);
    CHECK((*gRef)->AddDeviceClient(gRef, 99, &gClient) == kAudioHardwareBadObjectError);
}

static void TestDeviceProperties(void) {
    static const AudioObjectPropertySelector sels[] = {
        kAudioObjectPropertyBaseClass,
        kAudioObjectPropertyClass,
        kAudioObjectPropertyOwner,
        kAudioObjectPropertyName,
        kAudioObjectPropertyManufacturer,
        kAudioObjectPropertyOwnedObjects,
        kAudioObjectPropertyControlList,
        kAudioDevicePropertyDeviceUID,
        kAudioDevicePropertyModelUID,
        kAudioDevicePropertyTransportType,
        kAudioDevicePropertyRelatedDevices,
        kAudioDevicePropertyClockDomain,
        kAudioDevicePropertyDeviceIsAlive,
        kAudioDevicePropertyDeviceIsRunning,
        kAudioDevicePropertyDeviceCanBeDefaultDevice,
        kAudioDevicePropertyDeviceCanBeDefaultSystemDevice,
        kAudioDevicePropertyLatency,
        kAudioDevicePropertyStreams,
        kAudioDevicePropertySafetyOffset,
        kAudioDevicePropertyNominalSampleRate,
        kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioDevicePropertyIsHidden,
        kAudioDevicePropertyPreferredChannelsForStereo,
        kAudioDevicePropertyPreferredChannelLayout,
        kAudioDevicePropertyZeroTimeStampPeriod,
    };
    static const AudioObjectPropertySelector scoped[] = {
        kAudioObjectPropertyOwnedObjects,
        kAudioObjectPropertyControlList,
        kAudioDevicePropertyStreams,
        kAudioDevicePropertyLatency,
        kAudioDevicePropertySafetyOffset,
        kAudioDevicePropertyPreferredChannelsForStereo,
        kAudioDevicePropertyPreferredChannelLayout,
        kAudioDevicePropertyDeviceCanBeDefaultDevice,
    };
    WalkObject(kDevice, sels, 25, GLOBAL);
    WalkObject(kDevice, scoped, 8, INPUT);
    WalkObject(kDevice, scoped, 8, OUTPUT);

    // Global-only selectors do not exist in a directional scope.
    for (int i = 0; i < 25; i++) {
        Boolean isScoped = false;
        for (int j = 0; j < 8; j++)
            isScoped |= sels[i] == scoped[j];
        if (isScoped) continue;
        AudioObjectPropertyAddress a = Addr(sels[i], INPUT);
        CHECK(!(*gRef)->HasProperty(gRef, kDevice, 0, &a));
        UInt32 size = 0;
        CHECK(GetSize(kDevice, a, &size) == kAudioHardwareUnknownPropertyError);
    }

    CHECK(Get32(kDevice, kAudioObjectPropertyClass, GLOBAL) == kAudioDeviceClassID);
    CHECK(Get32(kDevice, kAudioObjectPropertyOwner, GLOBAL) == kPlugIn);
    // No pedal is attached under test.
    CHECK(Get32(kDevice, kAudioDevicePropertyDeviceIsAlive, GLOBAL) == 0);
    CHECK(Get32(kDevice, kAudioDevicePropertyTransportType, GLOBAL) ==
          kAudioDeviceTransportTypeUSB);
    CHECK(Get32(kDevice, kAudioDevicePropertyDeviceIsRunning, GLOBAL) == 0);
    CHECK(Get32(kDevice, kAudioDevicePropertySafetyOffset, INPUT) > 0);
    CHECK(Get32(kDevice, kAudioDevicePropertySafetyOffset, OUTPUT) > 0);
    CHECK(Get32(kDevice, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, GLOBAL) == 0);
    CHECK(Get32(kDevice, kAudioDevicePropertyZeroTimeStampPeriod, GLOBAL) >= 10923);

    AudioObjectID ids[4];
    CHECK(GetIDs(kDevice, kAudioDevicePropertyStreams, GLOBAL, ids, 4) == 2);
    CHECK(ids[0] == kStreamIn && ids[1] == kStreamOut);
    CHECK(GetIDs(kDevice, kAudioDevicePropertyStreams, INPUT, ids, 4) == 1);
    CHECK(ids[0] == kStreamIn);
    CHECK(GetIDs(kDevice, kAudioDevicePropertyStreams, OUTPUT, ids, 4) == 1);
    CHECK(ids[0] == kStreamOut);
    CHECK(GetIDs(kDevice, kAudioObjectPropertyOwnedObjects, GLOBAL, ids, 4) == 2);
    // A short buffer gets a partial list, never an error.
    CHECK(GetIDs(kDevice, kAudioDevicePropertyStreams, GLOBAL, ids, 1) == 1);
    CHECK(GetIDs(kDevice, kAudioObjectPropertyControlList, GLOBAL, ids, 4) == 0);

    Float64 rate = 0;
    UInt32 size  = 0;
    CHECK(Get(kDevice, Addr(kAudioDevicePropertyNominalSampleRate, GLOBAL), sizeof rate, &size,
              &rate) == kAudioHardwareNoError);
    CHECK(rate == 44100.0);

    CFStringRef uid = NULL;
    CHECK(Get(kDevice, Addr(kAudioDevicePropertyDeviceUID, GLOBAL), sizeof uid, &size, &uid) ==
          kAudioHardwareNoError);
    CHECK(uid != NULL && CFStringCompare(uid, CFSTR("GT10Audio:Device"), 0) == kCFCompareEqualTo);
    if (uid) CFRelease(uid);

    // Setting the one supported rate succeeds, anything else is refused.
    AudioObjectPropertyAddress a = Addr(kAudioDevicePropertyNominalSampleRate, GLOBAL);
    Boolean settable             = false;
    CHECK((*gRef)->IsPropertySettable(gRef, kDevice, 0, &a, &settable) == kAudioHardwareNoError &&
          settable);
    CHECK((*gRef)->SetPropertyData(gRef, kDevice, 0, &a, 0, NULL, sizeof rate, &rate) ==
          kAudioHardwareNoError);
    rate = 48000.0;
    CHECK((*gRef)->SetPropertyData(gRef, kDevice, 0, &a, 0, NULL, sizeof rate, &rate) ==
          kAudioDeviceUnsupportedFormatError);

    a = Addr(kAudioDevicePropertyStreams, kAudioObjectPropertyScopePlayThrough);
    CHECK(!(*gRef)->HasProperty(gRef, kDevice, 0, &a));

    // OwnedObjects filters by the class qualifier.
    a                 = Addr(kAudioObjectPropertyOwnedObjects, GLOBAL);
    AudioClassID want = kAudioControlClassID;
    CHECK((*gRef)->GetPropertyDataSize(gRef, kDevice, 0, &a, sizeof want, &want, &size) ==
          kAudioHardwareNoError);
    CHECK(size == 0);
    want = kAudioStreamClassID;
    CHECK((*gRef)->GetPropertyDataSize(gRef, kDevice, 0, &a, sizeof want, &want, &size) ==
          kAudioHardwareNoError);
    CHECK(size == 2 * sizeof(AudioObjectID));
    want = kAudioObjectClassID;
    CHECK((*gRef)->GetPropertyDataSize(gRef, kDevice, 0, &a, sizeof want, &want, &size) ==
          kAudioHardwareNoError);
    CHECK(size == 2 * sizeof(AudioObjectID));
    want = kAudioStreamClassID;
    CHECK((*gRef)->GetPropertyDataSize(gRef, kPlugIn, 0, &a, sizeof want, &want, &size) ==
          kAudioHardwareNoError);
    CHECK(size == 0);
    want = kAudioDeviceClassID;
    CHECK((*gRef)->GetPropertyDataSize(gRef, kPlugIn, 0, &a, sizeof want, &want, &size) ==
          kAudioHardwareNoError);
    CHECK(size == sizeof(AudioObjectID));

    // Setter payloads must be exact.
    a                = Addr(kAudioDevicePropertyNominalSampleRate, GLOBAL);
    Float64 rates[2] = {44100.0, 44100.0};
    CHECK((*gRef)->SetPropertyData(gRef, kDevice, 0, &a, 0, NULL, sizeof rates, rates) ==
          kAudioHardwareBadPropertySizeError);
    CHECK((*gRef)->SetPropertyData(gRef, kDevice, 0, &a, 0, NULL, 4, rates) ==
          kAudioHardwareBadPropertySizeError);
}

static void TestStreamProperties(void) {
    static const AudioObjectPropertySelector sels[] = {
        kAudioObjectPropertyBaseClass,
        kAudioObjectPropertyClass,
        kAudioObjectPropertyOwner,
        kAudioObjectPropertyName,
        kAudioObjectPropertyOwnedObjects,
        kAudioStreamPropertyIsActive,
        kAudioStreamPropertyDirection,
        kAudioStreamPropertyTerminalType,
        kAudioStreamPropertyStartingChannel,
        kAudioStreamPropertyLatency,
        kAudioStreamPropertyVirtualFormat,
        kAudioStreamPropertyPhysicalFormat,
        kAudioStreamPropertyAvailableVirtualFormats,
        kAudioStreamPropertyAvailablePhysicalFormats,
    };
    WalkObject(kStreamIn, sels, 14, GLOBAL);
    WalkObject(kStreamOut, sels, 14, GLOBAL);

    CHECK(Get32(kStreamIn, kAudioObjectPropertyClass, GLOBAL) == kAudioStreamClassID);
    CHECK(Get32(kStreamIn, kAudioObjectPropertyOwner, GLOBAL) == kDevice);
    CHECK(Get32(kStreamIn, kAudioStreamPropertyDirection, GLOBAL) == 1);
    CHECK(Get32(kStreamOut, kAudioStreamPropertyDirection, GLOBAL) == 0);
    CHECK(Get32(kStreamIn, kAudioStreamPropertyStartingChannel, GLOBAL) == 1);

    AudioObjectPropertyAddress a = Addr(kAudioStreamPropertyDirection, INPUT);
    CHECK(!(*gRef)->HasProperty(gRef, kStreamIn, 0, &a));

    // The wire format of the pedal: 24-bit packed integer, 6 bytes per frame.
    AudioStreamBasicDescription f;
    UInt32 size = 0;
    CHECK(Get(kStreamIn, Addr(kAudioStreamPropertyPhysicalFormat, GLOBAL), sizeof f, &size, &f) ==
          kAudioHardwareNoError);
    CHECK(f.mSampleRate == 44100.0);
    CHECK(f.mFormatID == kAudioFormatLinearPCM);
    CHECK(f.mFormatFlags == (kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked));
    CHECK(f.mChannelsPerFrame == 2 && f.mBitsPerChannel == 24);
    CHECK(f.mBytesPerFrame == 6 && f.mBytesPerPacket == 6 && f.mFramesPerPacket == 1);

    AudioStreamBasicDescription v;
    CHECK(Get(kStreamOut, Addr(kAudioStreamPropertyVirtualFormat, GLOBAL), sizeof v, &size, &v) ==
          kAudioHardwareNoError);
    CHECK(memcmp(&f, &v, sizeof f) == 0);

    AudioStreamRangedDescription ranged[2];
    CHECK(Get(kStreamIn, Addr(kAudioStreamPropertyAvailablePhysicalFormats, GLOBAL), sizeof ranged,
              &size, ranged) == kAudioHardwareNoError);
    CHECK(size == sizeof ranged[0]);
    CHECK(ranged[0].mSampleRateRange.mMinimum == 44100.0 &&
          ranged[0].mSampleRateRange.mMaximum == 44100.0);
    CHECK(memcmp(&ranged[0].mFormat, &f, sizeof f) == 0);

    a = Addr(kAudioStreamPropertyPhysicalFormat, GLOBAL);
    CHECK((*gRef)->SetPropertyData(gRef, kStreamIn, 0, &a, 0, NULL, sizeof f, &f) ==
          kAudioHardwareNoError);
    AudioStreamBasicDescription bad = f;
    bad.mBitsPerChannel             = 16;
    CHECK((*gRef)->SetPropertyData(gRef, kStreamIn, 0, &a, 0, NULL, sizeof bad, &bad) ==
          kAudioDeviceUnsupportedFormatError);
    bad                 = f;
    bad.mBytesPerPacket = 0;
    CHECK((*gRef)->SetPropertyData(gRef, kStreamIn, 0, &a, 0, NULL, sizeof bad, &bad) ==
          kAudioDeviceUnsupportedFormatError);
    bad                  = f;
    bad.mFramesPerPacket = 2;
    CHECK((*gRef)->SetPropertyData(gRef, kStreamIn, 0, &a, 0, NULL, sizeof bad, &bad) ==
          kAudioDeviceUnsupportedFormatError);
    AudioStreamBasicDescription two[2] = {f, f};
    CHECK((*gRef)->SetPropertyData(gRef, kStreamIn, 0, &a, 0, NULL, sizeof two, two) ==
          kAudioHardwareBadPropertySizeError);
}

static void TestIOControl(void) {
    Boolean willDo = false, inPlace = false;
    CHECK((*gRef)->WillDoIOOperation(gRef, kDevice, 1, kAudioServerPlugInIOOperationReadInput,
                                     &willDo, &inPlace) == kAudioHardwareNoError);
    CHECK(willDo && inPlace);
    CHECK((*gRef)->WillDoIOOperation(gRef, kDevice, 1, kAudioServerPlugInIOOperationWriteMix,
                                     &willDo, &inPlace) == kAudioHardwareNoError);
    CHECK(willDo);
    CHECK((*gRef)->WillDoIOOperation(gRef, kDevice, 1, kAudioServerPlugInIOOperationConvertInput,
                                     &willDo, &inPlace) == kAudioHardwareNoError);
    CHECK(!willDo);
    CHECK((*gRef)->WillDoIOOperation(gRef, 99, 1, kAudioServerPlugInIOOperationReadInput, &willDo,
                                     &inPlace) == kAudioHardwareBadObjectError);
    CHECK((*gRef)->BeginIOOperation(gRef, kDevice, 1, kAudioServerPlugInIOOperationCycle, 512,
                                    &gCycle) == kAudioHardwareNoError);
    CHECK((*gRef)->EndIOOperation(gRef, kDevice, 1, kAudioServerPlugInIOOperationCycle, 512,
                                  &gCycle) == kAudioHardwareNoError);
    CHECK((*gRef)->PerformDeviceConfigurationChange(gRef, kDevice, 0, &gChangeInfo) ==
          kAudioHardwareNoError);
    CHECK((*gRef)->AddDeviceClient(gRef, kDevice, &gClient) == kAudioHardwareNoError);
    CHECK((*gRef)->RemoveDeviceClient(gRef, kDevice, &gClient) == kAudioHardwareNoError);
}

static UInt64 TicksPerSecond(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (UInt64)(1e9 * tb.denom / (Float64)tb.numer);
}

static void TestClock(void) {
    Float64 sampleTime = -1;
    UInt64 hostTime = 0, seed = 0, seed2 = 0;

    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareIllegalOperationError);
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNotRunningError);

    const UInt64 before = mach_absolute_time();
    CHECK((*gRef)->StartIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    const UInt64 after = mach_absolute_time();
    CHECK(Get32(kDevice, kAudioDevicePropertyDeviceIsRunning, GLOBAL) == 1);

    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);
    CHECK(sampleTime == 0.0);
    CHECK(hostTime >= before && hostTime <= after);
    const UInt64 anchor = hostTime;

    // A second client joins the running timeline without re-anchoring it.
    CHECK((*gRef)->StartIO(gRef, kDevice, 2) == kAudioHardwareNoError);
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 2, &sampleTime, &hostTime, &seed2) ==
          kAudioHardwareNoError);
    CHECK(sampleTime == 0.0 && hostTime == anchor && seed2 == seed);

    // One period is 16384 frames, about 371 ms. The sleep has no upper
    // bound, so the answer is bracketed by clock reads around the call. The
    // boundary it names must already have passed.
    const UInt32 period          = Get32(kDevice, kAudioDevicePropertyZeroTimeStampPeriod, GLOBAL);
    const Float64 ticksPerPeriod = (Float64)TicksPerSecond() * period / 44100.0;
    usleep((useconds_t)(1.6e6 * period / 44100.0));

    const UInt64 t0 = mach_absolute_time();
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed2) ==
          kAudioHardwareNoError);
    const UInt64 t1    = mach_absolute_time();
    const UInt64 lo    = (UInt64)((Float64)(t0 - anchor) / ticksPerPeriod);
    const UInt64 hi    = (UInt64)((Float64)(t1 - anchor) / ticksPerPeriod);
    const UInt64 count = (UInt64)(sampleTime / period);
    CHECK(count >= 1 && count >= lo && count <= hi);
    CHECK(hostTime <= t0);
    CHECK(sampleTime == (Float64)count * period);
    const Float64 drift = fabs((Float64)(hostTime - anchor) - (Float64)count * ticksPerPeriod);
    CHECK(drift <= 1.0);
    CHECK(seed2 == seed);

    // Successive answers never move backwards.
    Float64 s2 = 0;
    UInt64 h2  = 0;
    for (int i = 0; i < 1000; i++) {
        CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &s2, &h2, &seed2) ==
              kAudioHardwareNoError);
        CHECK(s2 >= sampleTime && h2 >= hostTime);
        sampleTime = s2;
        hostTime   = h2;
    }

    // Both clients stop, the device is no longer running, a restart re-seeds.
    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK(Get32(kDevice, kAudioDevicePropertyDeviceIsRunning, GLOBAL) == 1);
    CHECK((*gRef)->StopIO(gRef, kDevice, 2) == kAudioHardwareNoError);
    CHECK(Get32(kDevice, kAudioDevicePropertyDeviceIsRunning, GLOBAL) == 0);
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed2) ==
          kAudioHardwareNotRunningError);
    CHECK((*gRef)->StartIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed2) ==
          kAudioHardwareNoError);
    CHECK(sampleTime == 0.0 && seed2 != seed);
    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareNoError);
}

static OSStatus ReadInput(Float64 sampleTime, UInt32 frames, UInt8 *buf) {
    AudioServerPlugInIOCycleInfo c = CycleAt(sampleTime);
    return (*gRef)->DoIOOperation(gRef, kDevice, kStreamIn, 1,
                                  kAudioServerPlugInIOOperationReadInput, frames, &c, buf, NULL);
}

static bool AllZero(const UInt8 *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != 0) return false;
    return true;
}

static void TestInputWithoutUSB(void) {
    enum { kFrames = 512, kBytes = kFrames * 6 };
    static UInt8 buf[kBytes];
    CHECK((*gRef)->StartIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK(!FakeUSBStreaming());

    // Without a pedal the input is silence at any sample time.
    memset(buf, 0x55, sizeof buf);
    CHECK(ReadInput(0, kFrames, buf) == kAudioHardwareNoError);
    CHECK(AllZero(buf, kBytes));
    memset(buf, 0x55, sizeof buf);
    CHECK(ReadInput(1000037, kFrames, buf) == kAudioHardwareNoError);
    CHECK(AllZero(buf, kBytes));

    // Output is accepted and left alone. Mismatched stream and operation
    // pairs are refused.
    AudioServerPlugInIOCycleInfo c = CycleAt(0);
    memset(buf, 0x55, 48);
    CHECK((*gRef)->DoIOOperation(gRef, kDevice, kStreamOut, 1,
                                 kAudioServerPlugInIOOperationWriteMix, 8, &c, buf,
                                 NULL) == kAudioHardwareNoError);
    CHECK(buf[0] == 0x55 && buf[47] == 0x55);
    CHECK((*gRef)->DoIOOperation(gRef, kDevice, kStreamIn, 1, kAudioServerPlugInIOOperationWriteMix,
                                 8, &c, buf, NULL) == kAudioHardwareIllegalOperationError);
    CHECK((*gRef)->DoIOOperation(gRef, kDevice, kStreamOut, 1,
                                 kAudioServerPlugInIOOperationReadInput, 8, &c, buf,
                                 NULL) == kAudioHardwareIllegalOperationError);
    CHECK(buf[0] == 0x55);
    CHECK((*gRef)->DoIOOperation(gRef, kDevice, kStreamIn, 1,
                                 kAudioServerPlugInIOOperationConvertInput, 8, &c, buf,
                                 NULL) == kAudioHardwareUnsupportedOperationError);
    CHECK((*gRef)->DoIOOperation(gRef, kDevice, 99, 1, kAudioServerPlugInIOOperationWriteMix, 8, &c,
                                 buf, NULL) == kAudioHardwareBadObjectError);

    // A sample time that cannot become a position is refused before it reaches
    // llround, where a NaN or an out-of-range value would be undefined.
    // 5.0e18 is inside int64_t and outside the accepted half range, so it tells
    // the bound apart from a full-range one.
    static const Float64 unusable[] = {
        (Float64)NAN, (Float64)INFINITY, -(Float64)INFINITY, 9.3e18, -9.3e18, 5.0e18, -5.0e18};
    for (size_t i = 0; i < sizeof unusable / sizeof unusable[0]; i++) {
        AudioServerPlugInIOCycleInfo bad = CycleAt(unusable[i]);
        CHECK((*gRef)->DoIOOperation(gRef, kDevice, kStreamIn, 1,
                                     kAudioServerPlugInIOOperationReadInput, 8, &bad, buf,
                                     NULL) == kAudioHardwareIllegalOperationError);
        bad = CycleOut(unusable[i]);
        CHECK((*gRef)->DoIOOperation(gRef, kDevice, kStreamOut, 1,
                                     kAudioServerPlugInIOOperationWriteMix, 8, &bad, buf,
                                     NULL) == kAudioHardwareIllegalOperationError);
    }

    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareNoError);
}

static void Pattern(UInt8 *p, uint32_t frames, uint8_t seed) {
    for (uint32_t i = 0; i < frames * 6; i++)
        p[i] = (uint8_t)(seed + i);
}

static void TestStreaming(void) {
    enum { kFrames = 512, kBytes = kFrames * 6 };
    static UInt8 in[kBytes], out[kBytes], want[kBytes];
    Float64 sampleTime = -1;
    UInt64 hostTime = 0, seed = 0;

    // Arrival makes the device alive and tells the host, off the USB queue.
    const int notified = atomic_load(&gAliveNotifications);
    FakeUSBSetHeld(true);
    CHECK(Get32(kDevice, kAudioDevicePropertyDeviceIsAlive, GLOBAL) == 1);
    for (int i = 0; i < 1000 && atomic_load(&gAliveNotifications) == notified; i++)
        usleep(1000);
    CHECK(atomic_load(&gAliveNotifications) == notified + 1);

    // The first client starts the stream with both rings and a timestamp sink.
    // The rings are reset in the owner's prepare step, on the monitor queue.
    const int starts   = FakeUSBStreamStarts();
    const int prepares = FakeUSBPrepares();
    CHECK((*gRef)->StartIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK(FakeUSBStreaming() && FakeUSBStreamStarts() == starts + 1);
    CHECK(FakeUSBPrepares() == prepares + 1);
    const GT10StreamConfig *cfg = FakeUSBConfig();
    CHECK(cfg->capture && cfg->captureRing != NULL && cfg->render != NULL);
    CHECK(cfg->timeStamp != NULL && cfg->playbackTransfers == 0);

    // The device clock is the stream's, not the synthetic one.
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);
    CHECK(sampleTime == 0.0);
    const UInt64 deviceSeed = seed;
    GT10TimeStampPublish(cfg->timeStamp, 16384, 123456789);
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);
    CHECK(sampleTime == 16384.0 && hostTime == 123456789 && seed == deviceSeed);

    // Input reads the capture ring at the requested sample time.
    Pattern(want, kFrames, 7);
    GT10RingWrite(cfg->captureRing, 0, want, kFrames);
    memset(in, 0x55, sizeof in);
    CHECK(ReadInput(0, kFrames, in) == kAudioHardwareNoError);
    CHECK(memcmp(in, want, kBytes) == 0);
    // A cycle can take its sample time from the synthetic clock and reach the
    // ring after the clock moved to the device. That position must not stick:
    // the floor of the reader never comes back down, so the ring would answer
    // nothing for the rest of the session.
    memset(in, 0x55, sizeof in);
    CHECK(ReadInput(158000000.0, kFrames, in) == kAudioHardwareNoError);
    CHECK(AllZero(in, kBytes));
    memset(in, 0x55, sizeof in);
    CHECK(ReadInput(0, kFrames, in) == kAudioHardwareNoError);
    CHECK(memcmp(in, want, kBytes) == 0);

    memset(in, 0x55, 48);
    CHECK(ReadInput(kFrames, 8, in) == kAudioHardwareNoError);
    CHECK(AllZero(in, 48));

    // Output lands at its sample time, where the engine renders it from.
    Pattern(out, kFrames, 99);
    AudioServerPlugInIOCycleInfo c = CycleOut(4000);
    CHECK((*gRef)->DoIOOperation(gRef, kDevice, kStreamOut, 1,
                                 kAudioServerPlugInIOOperationWriteMix, kFrames, &c, out,
                                 NULL) == kAudioHardwareNoError);
    static UInt8 rendered[kBytes];
    REQUIRE(cfg->render != NULL);
    cfg->render(cfg->renderContext, 4000, rendered, kFrames);
    CHECK(memcmp(rendered, out, kBytes) == 0);

    // The same stray position on the playback ring would move its end past
    // every later write, and the writer never moves it back.
    AudioServerPlugInIOCycleInfo strayOut = CycleOut(158000000.0);
    CHECK((*gRef)->DoIOOperation(gRef, kDevice, kStreamOut, 1,
                                 kAudioServerPlugInIOOperationWriteMix, kFrames, &strayOut, out,
                                 NULL) == kAudioHardwareNoError);
    Pattern(out, kFrames, 55);
    AudioServerPlugInIOCycleInfo nextOut = CycleOut(8000);
    CHECK((*gRef)->DoIOOperation(gRef, kDevice, kStreamOut, 1,
                                 kAudioServerPlugInIOOperationWriteMix, kFrames, &nextOut, out,
                                 NULL) == kAudioHardwareNoError);
    cfg->render(cfg->renderContext, 8000, rendered, kFrames);
    CHECK(memcmp(rendered, out, kBytes) == 0);

    // The last client stops the stream and the clock stops with it.
    const int stops = FakeUSBStreamStops();
    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK(!FakeUSBStreaming() && FakeUSBStreamStops() == stops + 1);
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNotRunningError);

    // A restart resets the rings and uses a new seed.
    GT10RingWrite(FakeUSBConfig()->captureRing, GT10RingEnd(FakeUSBConfig()->captureRing), want, 8);
    CHECK((*gRef)->StartIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK(FakeUSBStreaming());
    CHECK(GT10RingEnd(FakeUSBConfig()->captureRing) == 0);
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);
    CHECK(seed != deviceSeed);

    // An engine fault hands the clock back to the synthetic one under a new
    // seed. The rings still hold audio, but IO stops touching them.
    GT10RingWrite(FakeUSBConfig()->captureRing, 0, want, kFrames);
    CHECK(ReadInput(0, 8, in) == kAudioHardwareNoError);
    CHECK(memcmp(in, want, 48) == 0);
    UInt64 beforeFault = seed;
    FakeUSBFault();
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);
    CHECK(seed != beforeFault);
    memset(in, 0x55, 48);
    CHECK(ReadInput(0, 8, in) == kAudioHardwareNoError);
    CHECK(AllZero(in, 48));
    c = CycleOut(9000);
    CHECK((*gRef)->DoIOOperation(gRef, kDevice, kStreamOut, 1,
                                 kAudioServerPlugInIOOperationWriteMix, kFrames, &c, out,
                                 NULL) == kAudioHardwareNoError);
    FakeUSBConfig()->render(FakeUSBConfig()->renderContext, 9000, rendered, kFrames);
    CHECK(AllZero(rendered, kBytes));
    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK((*gRef)->StartIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK(FakeUSBStreaming());

    // Removal while running: not alive, the timeline changes seed, input is
    // silence, and the clock keeps answering so clients do not error out.
    UInt64 streamSeed = 0;
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &streamSeed) ==
          kAudioHardwareNoError);
    FakeUSBSetHeld(false);
    CHECK(Get32(kDevice, kAudioDevicePropertyDeviceIsAlive, GLOBAL) == 0);
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);
    CHECK(seed != streamSeed);
    memset(in, 0x55, 48);
    CHECK(ReadInput(0, 8, in) == kAudioHardwareNoError);
    CHECK(AllZero(in, 48));
    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareNoError);

    // A replug while IO runs brings streaming back by itself, under a new seed,
    // without the app restarting. The restart itself lives in the monitor, which
    // the fake replaces, so this covers the answer of the HAL side and not the
    // monitor. Hardware covers the monitor, see docs/VERIFICATION.md.
    FakeUSBSetHeld(true);
    CHECK((*gRef)->StartIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK(FakeUSBStreaming());
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);
    const UInt64 beforeReplug = seed;
    FakeUSBSetHeld(false);
    CHECK(!FakeUSBStreaming());
    FakeUSBSetHeld(true);
    CHECK(FakeUSBStreaming());
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);
    CHECK(seed != beforeReplug);
    Pattern(want, 8, 33);
    GT10RingWrite(FakeUSBConfig()->captureRing, 0, want, 8);
    memset(in, 0x55, 48);
    CHECK(ReadInput(0, 8, in) == kAudioHardwareNoError);
    CHECK(memcmp(in, want, 48) == 0);
    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    FakeUSBSetHeld(false);
    FakeUSBSetHeld(true);

    // Once IO has stopped, a late announce cannot put the device clock back.
    CHECK(!FakeUSBAnnounceAgain());
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNotRunningError);

    // A stream that fails to start, or never publishes a timestamp, falls back
    // to the synthetic clock and leaves no stream running.
    FakeUSBSetHeld(true);
    FakeUSBSetStartResult(GT10_ERR);
    CHECK((*gRef)->StartIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK(!FakeUSBStreaming());
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);
    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    FakeUSBSetStartResult(GT10_OK);

    FakeUSBSetPublishOnStart(false);
    CHECK((*gRef)->StartIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK(!FakeUSBStreaming());
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);
    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    FakeUSBSetPublishOnStart(true);
    FakeUSBSetHeld(false);
}

static void TestInitialize(void) {
    CHECK((*gRef)->Initialize(gRef, &gHostInterface) == kAudioHardwareNoError);
}

// An app can start recording while the pedal is unplugged, because the device
// stays listed in that state. The request has to outlive the absence: the app
// will not start IO a second time when the pedal shows up.
static void TestStartBeforeArrival(void) {
    Float64 sampleTime = -1;
    UInt64 hostTime = 0, seed = 0;

    FakeUSBSetHeld(false);
    const int starts = FakeUSBStreamStarts();
    CHECK((*gRef)->StartIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    CHECK(!FakeUSBStreaming() && FakeUSBStreamStarts() == starts);
    // Silence on the synthetic clock until the pedal arrives.
    CHECK((*gRef)->GetZeroTimeStamp(gRef, kDevice, 1, &sampleTime, &hostTime, &seed) ==
          kAudioHardwareNoError);

    FakeUSBSetHeld(true);
    CHECK(FakeUSBStreaming() && FakeUSBStreamStarts() == starts + 1);

    CHECK((*gRef)->StopIO(gRef, kDevice, 1) == kAudioHardwareNoError);
    FakeUSBSetHeld(false);
}

int main(void) {
    TestFactory();
    TestQueryInterface();
    TestReleaseFloor();
    TestConcurrentRefCount();
    TestInitialize();
    TestPlugInProperties();
    TestDeviceProperties();
    TestStreamProperties();
    TestIOControl();
    TestClock();
    TestInputWithoutUSB();
    TestStreaming();
    TestStartBeforeArrival();
    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
