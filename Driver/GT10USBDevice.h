// USB owner for the BOSS GT-10 in Advanced mode (0x0582:0x00DA), used by the
// AudioServerPlugIn host. Checkpoints 1 to 3: discover the device, configure
// it when unconfigured, and open the two audio interfaces on alt 0. No
// alternate-setting switch and no data transfer happen here. Those belong to
// checkpoint 4, which is hardware gated.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Status codes for the injectable operations, kept free of IOKit so the
// control flow is unit testable. GT10_OK is 0, which is also kIOReturnSuccess.
enum {
    GT10_OK          = 0,
    GT10_ERR         = -1,
    GT10_BUSY        = -2,  // another owner holds the exclusive session
    GT10_NOTFOUND    = -3,
    GT10_ALT_UNKNOWN = -4,  // alt 0 not restored, the caller must close the interface
};

// ---- Pure decision logic, no IOKit, unit tested ----

typedef struct {
    bool configure;  // Call SetConfiguration?
    bool valid;
    uint8_t value;  // The bConfigurationValue to set.
} GT10ConfigPlan;

// interfaceCount is how many interface nubs the device currently exposes.
// descriptorValue is bConfigurationValue from the configuration descriptor.
// On macOS 26 the GT-10 reports itself configured yet exposes no interface
// nubs until a client calls SetConfiguration, which instantiates them. So the
// trigger is nub ABSENCE, not the reported configuration value. When nubs are
// already present, never re-set: SetConfiguration destroys and recreates every
// interface object, dropping any owner's open session, including MIDI's.
GT10ConfigPlan GT10PlanConfiguration(int interfaceCount, uint8_t descriptorValue);

typedef enum {
    GT10RoleNone = 0,
    GT10RoleAudioOut,  // IF 0, isochronous OUT
    GT10RoleAudioIn,   // IF 1, isochronous IN
} GT10InterfaceRole;

GT10InterfaceRole GT10RoleForInterface(uint8_t bInterfaceNumber);

// ---- Testable control flow over injectable operations ----
// The live path builds these from the IOUSBLib vtable. Tests supply fakes, so
// the operation order, the error handling, and the "never touch IF2" and
// "alt 0 only" rules are verified without hardware.

typedef struct {
    int (*open)(void *ctx);                       // USBDeviceOpen
    int (*count_interfaces)(void *ctx);           // nub count, or GT10_ERR
    int (*desc_value)(void *ctx, uint8_t *out);   // descriptor bConfigurationValue
    int (*set_config)(void *ctx, uint8_t value);  // SetConfiguration
    void (*close)(void *ctx);                     // USBDeviceClose
    void *ctx;
} GT10DeviceOps;

// Open the exclusive session, count interface nubs, call SetConfiguration only
// when none exist yet (which instantiates them), and close on every path.
// Counting inside the exclusive session closes the two-process race: a second
// opener sees the nubs the first created and skips. GT10_BUSY from open means
// another owner is configuring, reported as success. Returns GT10_OK or the
// first failing code.
int GT10ConfigureVia(const GT10DeviceOps *ops);

typedef struct {
    int (*number)(void *ctx, uint8_t *out);         // GetInterfaceNumber
    int (*alt_setting)(void *ctx, uint8_t *out);    // GetAlternateSetting
    int (*open)(void *ctx);                         // USBInterfaceOpen
    int (*num_endpoints)(void *ctx, uint8_t *out);  // GetNumEndpoints
    void (*close)(void *ctx);                       // USBInterfaceClose
    void *ctx;
} GT10InterfaceOps;

// Read the interface number and reject anything but IF 0 or IF 1 without
// opening it, so the MIDI interface is never stolen. Require alt 0 before and
// after opening, so this never switches a setting. Zero endpoints on alt 0 is
// expected, not an error.
int GT10OpenInterfaceVia(const GT10InterfaceOps *ops, GT10InterfaceRole *outRole);

// ---- Acquisition retry policy, pure, unit tested ----

typedef struct {
    bool retry;
    unsigned delayMs;
} GT10RetryPlan;

// Decide whether to attempt acquisition again after a failure. Interfaces can
// be absent because another owner is midway through instantiating them, so a
// bounded backoff looks again rather than giving up until the next replug.
// attempt counts from 0.
GT10RetryPlan GT10PlanRetry(int attempt, int lastResult);

// ---- Checkpoint 4a: alt-1 switch and pipe validation ----

// Each audio interface has one endpoint on alt 1, so its pipe is always index 1.
enum { kGT10PipeRef = 1, kGT10PipeBytes = 288 };

// The fields of IOUSBEndpointProperties this driver checks.
typedef struct {
    uint8_t alt;
    uint8_t direction;  // kUSBOut 0, kUSBIn 1
    uint8_t endpointNumber;
    uint8_t transferType;  // kUSBIsoc 1
    uint8_t syncType;
    uint8_t usageType;
    uint8_t interval;
    uint16_t maxPacketSize;
} GT10PipeInfo;

// True only for the exact pipe FINDINGS.md records for the role, on a
// full-speed bus. syncType must be the USBSpec.h enum (1 async, 2 adaptive).
// Whether GetPipePropertiesV3 reports that enum or raw bmAttributes bits is
// unverified, so a raw encoding fails closed here.
bool GT10PipeFitsRole(GT10InterfaceRole role, uint8_t deviceSpeed, const GT10PipeInfo *p);

typedef struct {
    int (*set_alt)(void *ctx, uint8_t alt);                            // SetAlternateInterface
    int (*alt_setting)(void *ctx, uint8_t *out);                       // GetAlternateSetting
    int (*num_endpoints)(void *ctx, uint8_t *out);                     // GetNumEndpoints
    int (*pipe_props)(void *ctx, uint8_t pipeRef, GT10PipeInfo *out);  // GetPipePropertiesV3
    void *ctx;
} GT10StreamOps;

// Switch an open interface to alt 1 and accept only the pipe its role expects.
// On any failure, including a failed switch, return to alt 0 and read it back:
// GT10_ERR when alt 0 is confirmed, GT10_ALT_UNKNOWN otherwise. *outPipe holds
// what the pipe reported, also on a mismatch. There is no configuration op, so
// this path can never reconfigure the device.
int GT10EnableStreamVia(const GT10StreamOps *ops, GT10InterfaceRole role, uint8_t deviceSpeed,
                        GT10PipeInfo *outPipe);

// Return to alt 0 and read it back. GT10_OK or GT10_ALT_UNKNOWN.
int GT10DisableStreamVia(const GT10StreamOps *ops);

// Enable IF0 then IF1. When IF1 fails, IF0 goes back to alt 0 too. Returns
// GT10_ALT_UNKNOWN when any restore is unconfirmed.
int GT10EnableStreamsVia(const GT10StreamOps *out, const GT10StreamOps *in, uint8_t deviceSpeed,
                         GT10PipeInfo pipes[2]);

// ---- Checkpoint 4b: one bounded isochronous read ----

enum { kGT10MaxReadFrames = 64, kGT10MaxLeadFrames = 64 };

typedef struct {
    uint16_t reqPerFrame;
    uint32_t numFrames;
    uint32_t bufferBytes;
} GT10ReadPlan;

// Every frame requests the full pipe capacity. On the asynchronous input the
// device picks the packet size, and a smaller request risks an overrun.
bool GT10PlanRead(uint16_t pipeCapacity, uint32_t numFrames, GT10ReadPlan *out);

// currentFrame + leadFrames, refusing a lead outside 1 to kGT10MaxLeadFrames
// and an overflowing sum.
bool GT10FrameStart(uint64_t currentFrame, uint32_t leadFrames, uint64_t *out);

typedef enum {
    GT10ReadRejected = 0,  // submission refused, nothing outstanding
    GT10ReadCompleted,     // completion arrived before the deadline
    GT10ReadAborted,       // deadline passed, AbortPipe, then the completion arrived
    GT10ReadAbandoned,     // no completion even after abort
} GT10ReadOutcome;

typedef struct {
    int (*submit)(void *ctx);              // bus frame, then ReadIsochPipeAsync
    bool (*wait)(void *ctx, unsigned ms);  // true once the completion arrived
    int (*abort)(void *ctx);               // AbortPipe
    void *ctx;
} GT10ReadOps;

// A timeout is not a completion. After the deadline this aborts and keeps
// waiting, because the kernel still owns the buffer and frame list until the
// callback runs. On GT10ReadAbandoned, the caller must not free anything the
// callback can reach. The process must exit instead of tearing down.
GT10ReadOutcome GT10ReadOnceVia(const GT10ReadOps *ops, unsigned waitMs, unsigned abortWaitMs);

// ---- Live USB owner, IOKit, exercised on hardware ----

typedef struct GT10USBDevice GT10USBDevice;

// Checkpoint 1. Find 0x0582:0x00DA and obtain a device interface. Opens
// nothing and transfers nothing. Returns NULL when the device is absent.
GT10USBDevice *GT10USBDeviceFind(void);

// Checkpoint 2. Guarded configuration through GT10ConfigureVia.
int GT10USBDeviceEnsureConfigured(GT10USBDevice *dev);

// Checkpoint 3. Open IF 0 and IF 1 on alt 0 through GT10OpenInterfaceVia. No
// alternate-setting switch, no transfer.
int GT10USBDeviceOpenInterfaces(GT10USBDevice *dev);

bool GT10USBDeviceHasInterfaces(const GT10USBDevice *dev);

typedef struct {
    // Index 0 is IF0, index 1 is IF1.
    uint8_t speed;
    int result;
    int32_t firstError[2];  // first raw IOReturn, 0 when none
    GT10PipeInfo pipes[2];
} GT10StreamReport;

// Checkpoint 4a. Needs checkpoint 3. Reserves isochronous bandwidth, submits no
// transfer.
int GT10USBDeviceEnableStreams(GT10USBDevice *dev, GT10StreamReport *report);
int GT10USBDeviceDisableStreams(GT10USBDevice *dev);

typedef struct {
    GT10ReadOutcome outcome;
    int32_t submitReturn;      // raw IOReturn
    int32_t completionReturn;  // raw IOReturn
    uint64_t busFrame;
    uint64_t frameStart;
    uint32_t numFrames;
    int32_t frameStatus[kGT10MaxReadFrames];
    uint16_t frameReq[kGT10MaxReadFrames];
    uint16_t frameAct[kGT10MaxReadFrames];
    uint8_t *buffer;  // prefilled with kGT10ReadFill before submission
    uint32_t bufferBytes;
    const char *setupFailure;  // the check that stopped the read before submission, or NULL
    int32_t setupReturn;       // its raw IOReturn, 0 when it was not an IOKit call
} GT10ReadReport;

enum { kGT10ReadFill = 0xA5 };

// A frame passes with success or underrun, which is how a short isochronous
// packet reports, and with a whole number of sample frames within the request.
bool GT10FrameAcceptable(int32_t status, uint16_t reqCount, uint16_t actCount,
                         uint32_t bytesPerFrame);

// The callback arriving is not a passing capture. This also requires a clean
// completion status, every frame acceptable, and some audio received.
bool GT10ReadAccepted(const GT10ReadReport *report, uint32_t bytesPerFrame);

// Checkpoint 4b. Needs checkpoint 4a. One ReadIsochPipeAsync on IF1, driven by
// the current thread's run loop. Free report->buffer with free(), except after
// GT10ReadAbandoned, where the caller must exit without freeing anything.
int GT10USBDeviceReadOnce(GT10USBDevice *dev, uint32_t numFrames, uint32_t leadFrames,
                          unsigned waitMs, GT10ReadReport *report);

// Release interfaces and the device. Safe on a partially built device and on
// NULL.
void GT10USBDeviceFree(GT10USBDevice *dev);

// ---- Arrival and removal monitor ----
// Acquires the interfaces when the pedal appears and releases them when it
// goes away, so a replug reacquires and an unplug never leaves the owner
// claiming interfaces that no longer exist. Every callback runs on the
// caller's serial queue, which is what serializes the state.

typedef void (*GT10USBHeldChanged)(bool held, void *ctx);

typedef struct GT10USBMonitor GT10USBMonitor;

GT10USBMonitor *GT10USBMonitorStart(void *serialQueue, GT10USBHeldChanged onChange, void *ctx);
void GT10USBMonitorStop(GT10USBMonitor *monitor);

#ifdef __cplusplus
}
#endif
