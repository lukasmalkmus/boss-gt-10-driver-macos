#include "GT10USBDevice.h"
#include "GT10USBInternal.h"
#include "GT10USBStream.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Block.h>
#include <dispatch/dispatch.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define USB_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "GT10USB: " fmt, ##__VA_ARGS__)

namespace {

constexpr uint16_t kVendorRoland        = 0x0582;
constexpr uint16_t kProductGT10Advanced = 0x00DA;
constexpr uint8_t kInterfaceAudioOut    = 0;
constexpr uint8_t kInterfaceAudioIn     = 1;

int MapReturn(IOReturn kr) {
    if (kr == kIOReturnSuccess) return GT10_OK;
    if (kr == kIOReturnExclusiveAccess) return GT10_BUSY;
    return GT10_ERR;
}

}  // namespace

// ---- Pure decision logic ----

GT10ConfigPlan GT10PlanConfiguration(int interfaceCount, uint8_t descriptorValue) {
    GT10ConfigPlan plan = {false, false, 0};

    // A configuration value of 0 is the unconfigured state, never a target.
    if (descriptorValue == 0) return plan;

    plan.valid = true;
    plan.value = descriptorValue;
    // Instantiate interfaces only when none exist. Setting the configuration
    // when nubs are already present would destroy and recreate them, dropping
    // another owner's open session.
    plan.configure = (interfaceCount == 0);
    return plan;
}

GT10InterfaceRole GT10RoleForInterface(uint8_t bInterfaceNumber) {
    switch (bInterfaceNumber) {
    case kInterfaceAudioOut: return GT10RoleAudioOut;
    case kInterfaceAudioIn: return GT10RoleAudioIn;
    default: return GT10RoleNone;
    }
}

// Five attempts spanning about 3 seconds, which covers another owner
// instantiating the interfaces without retrying forever on a dead device.
enum { kMaxAcquireAttempts = 5 };

// How long starting a stream waits for its first captured audio.
enum { kFirstTimeStampWaitMs = 500 };

// How many times a faulted stream starts again before the monitor gives up. A
// fault is usually one late transfer, and a restart recovers it. A device that
// faults on every start must not spin.
enum { kMaxFaultRestarts = 3 };

GT10RetryPlan GT10PlanRetry(int attempt, int lastResult) {
    GT10RetryPlan plan = {false, 0};
    if (lastResult == GT10_OK) return plan;
    if (attempt < 0 || attempt >= kMaxAcquireAttempts) return plan;

    plan.retry = true;
    // 100 ms doubling per attempt. The attempt bound above caps the last delay
    // at 1600 ms, so no separate ceiling is needed.
    plan.delayMs = 100u << (unsigned)attempt;
    return plan;
}

// ---- Testable control flow over injectable operations ----

int GT10ConfigureVia(const GT10DeviceOps *ops) {
    if (ops == nullptr) return GT10_ERR;

    const int opened = ops->open(ops->ctx);
    if (opened == GT10_BUSY) return GT10_OK;  // another owner configures
    if (opened != GT10_OK) return opened;

    int result      = GT10_OK;
    const int count = ops->count_interfaces(ops->ctx);
    if (count < 0) {
        // A failed count must never authorize configuring, which would destroy
        // another process's interfaces.
        ops->close(ops->ctx);
        return GT10_ERR;
    }

    uint8_t descValue         = 0;
    const bool haveDesc       = ops->desc_value(ops->ctx, &descValue) == GT10_OK;
    const GT10ConfigPlan plan = GT10PlanConfiguration(count, haveDesc ? descValue : 0);

    if (!plan.valid) {
        result = GT10_NOTFOUND;
    } else if (plan.configure) {
        result = ops->set_config(ops->ctx, plan.value);
    }
    ops->close(ops->ctx);
    return result;
}

int GT10OpenInterfaceVia(const GT10InterfaceOps *ops, GT10InterfaceRole *outRole) {
    *outRole = GT10RoleNone;
    if (ops == nullptr) return GT10_ERR;

    uint8_t number = 0xFF;
    if (ops->number(ops->ctx, &number) != GT10_OK) return GT10_ERR;
    const GT10InterfaceRole role = GT10RoleForInterface(number);
    if (role == GT10RoleNone) return GT10_ERR;  // not ours, never open it

    // Reject any interface that is not on alt 0 without switching it, so this
    // checkpoint never touches the isochronous setting.
    uint8_t alt = 0xFF;
    if (ops->alt_setting(ops->ctx, &alt) != GT10_OK) return GT10_ERR;
    if (alt != 0) return GT10_ERR;

    if (ops->open(ops->ctx) != GT10_OK) return GT10_ERR;

    // Read the setting again. Another process can switch it between the first
    // read and the open, and the open does not report that.
    if (ops->alt_setting(ops->ctx, &alt) != GT10_OK || alt != 0) {
        ops->close(ops->ctx);
        return GT10_ERR;
    }

    uint8_t endpoints = 0xFF;
    if (ops->num_endpoints(ops->ctx, &endpoints) != GT10_OK) {
        ops->close(ops->ctx);
        return GT10_ERR;
    }
    // Alt 0 is the vendor marker interface and reports zero endpoints, which is expected.

    *outRole = role;
    return GT10_OK;
}

// ---- Checkpoint 4a ----

namespace {

constexpr uint8_t kSpeedFull        = 1;  // kUSBDeviceSpeedFull
constexpr uint8_t kTypeIsoc         = 1;  // kUSBIsoc
constexpr uint8_t kSyncAsynchronous = 1;  // kUSBAsynchronousIsocSyncType
constexpr uint8_t kSyncAdaptive     = 2;  // kUSBAdaptiveIsocSyncType

// Return to alt 0 and read it back.
int RestoreAlt0(const GT10StreamOps *ops) {
    if (ops->set_alt(ops->ctx, 0) != GT10_OK) return GT10_ALT_UNKNOWN;
    uint8_t alt = 0xFF;
    if (ops->alt_setting(ops->ctx, &alt) != GT10_OK || alt != 0) return GT10_ALT_UNKNOWN;
    return GT10_OK;
}

}  // namespace

bool GT10PipeFitsRole(GT10InterfaceRole role, uint8_t deviceSpeed, const GT10PipeInfo *p) {
    if (p == nullptr || deviceSpeed != kSpeedFull) return false;

    uint8_t direction, endpoint, sync;
    switch (role) {
    case GT10RoleAudioOut:
        direction = 0;
        endpoint  = 1;
        sync      = kSyncAdaptive;
        break;
    case GT10RoleAudioIn:
        direction = 1;
        endpoint  = 2;
        sync      = kSyncAsynchronous;
        break;
    default: return false;
    }
    return p->alt == 1 && p->direction == direction && p->endpointNumber == endpoint &&
           p->transferType == kTypeIsoc && p->syncType == sync && p->usageType == 0 &&
           p->interval == 1 && p->maxPacketSize == kGT10PipeBytes;
}

int GT10EnableStreamVia(const GT10StreamOps *ops, GT10InterfaceRole role, uint8_t deviceSpeed,
                        GT10PipeInfo *outPipe) {
    if (ops == nullptr || outPipe == nullptr) return GT10_ERR;
    if (role != GT10RoleAudioOut && role != GT10RoleAudioIn) return GT10_ERR;
    if (deviceSpeed != kSpeedFull) return GT10_ERR;

    bool ok = ops->set_alt(ops->ctx, 1) == GT10_OK;

    uint8_t alt = 0xFF;
    if (ok) ok = ops->alt_setting(ops->ctx, &alt) == GT10_OK && alt == 1;

    uint8_t endpoints = 0xFF;
    if (ok) ok = ops->num_endpoints(ops->ctx, &endpoints) == GT10_OK && endpoints == 1;

    if (ok) {
        ok = ops->pipe_props(ops->ctx, kGT10PipeRef, outPipe) == GT10_OK &&
             GT10PipeFitsRole(role, deviceSpeed, outPipe);
    }

    if (ok) return GT10_OK;
    return RestoreAlt0(ops) == GT10_OK ? GT10_ERR : GT10_ALT_UNKNOWN;
}

int GT10DisableStreamVia(const GT10StreamOps *ops) {
    if (ops == nullptr) return GT10_ALT_UNKNOWN;
    return RestoreAlt0(ops);
}

int GT10EnableStreamsVia(const GT10StreamOps *out, const GT10StreamOps *in, uint8_t deviceSpeed,
                         GT10PipeInfo pipes[2]) {
    if (out == nullptr || in == nullptr || pipes == nullptr) return GT10_ERR;

    const int outResult = GT10EnableStreamVia(out, GT10RoleAudioOut, deviceSpeed, &pipes[0]);
    if (outResult != GT10_OK) return outResult;

    const int inResult = GT10EnableStreamVia(in, GT10RoleAudioIn, deviceSpeed, &pipes[1]);
    if (inResult == GT10_OK) return GT10_OK;

    const int outRestore = RestoreAlt0(out);
    if (inResult == GT10_ALT_UNKNOWN || outRestore != GT10_OK) return GT10_ALT_UNKNOWN;
    return GT10_ERR;
}

// ---- Checkpoint 4b ----

bool GT10PlanRead(uint16_t pipeCapacity, uint32_t numFrames, GT10ReadPlan *out) {
    if (out == nullptr || pipeCapacity == 0) return false;
    if (numFrames == 0 || numFrames > kGT10MaxReadFrames) return false;
    out->reqPerFrame = pipeCapacity;
    out->numFrames   = numFrames;
    out->bufferBytes = (uint32_t)pipeCapacity * numFrames;  // at most 65535 * 64
    return true;
}

bool GT10FrameStart(uint64_t currentFrame, uint32_t leadFrames, uint64_t *out) {
    if (out == nullptr || leadFrames == 0 || leadFrames > kGT10MaxLeadFrames) return false;
    if (currentFrame > UINT64_MAX - leadFrames) return false;
    *out = currentFrame + leadFrames;
    return true;
}

namespace {
constexpr int32_t kReturnUnderrun = (int32_t)0xE00002E7;  // kIOReturnUnderrun
}  // namespace

bool GT10FrameAcceptable(int32_t status, uint16_t reqCount, uint16_t actCount,
                         uint32_t bytesPerFrame) {
    if (status != 0 && status != kReturnUnderrun) return false;
    if (bytesPerFrame == 0 || actCount > reqCount) return false;
    return actCount % bytesPerFrame == 0;
}

bool GT10ReadAccepted(const GT10ReadReport *r, uint32_t bytesPerFrame) {
    if (r == nullptr || r->outcome != GT10ReadCompleted) return false;
    if (r->completionReturn != 0 && r->completionReturn != kReturnUnderrun) return false;
    if (r->numFrames > kGT10MaxReadFrames) return false;
    uint32_t received = 0;
    for (uint32_t i = 0; i < r->numFrames; i++) {
        if (!GT10FrameAcceptable(r->frameStatus[i], r->frameReq[i], r->frameAct[i], bytesPerFrame))
            return false;
        received += r->frameAct[i];
    }
    return received > 0;
}

GT10ReadOutcome GT10ReadOnceVia(const GT10ReadOps *ops, unsigned waitMs, unsigned abortWaitMs) {
    if (ops == nullptr) return GT10ReadRejected;
    if (ops->submit(ops->ctx) != GT10_OK) return GT10ReadRejected;
    if (ops->wait(ops->ctx, waitMs)) return GT10ReadCompleted;

    // AbortPipe's result is ignored: an error there does not prove that nothing
    // is outstanding, so the completion is awaited either way.
    (void)ops->abort(ops->ctx);
    return ops->wait(ops->ctx, abortWaitMs) ? GT10ReadAborted : GT10ReadAbandoned;
}

// ---- Live USB owner ----

struct GT10USBDevice {
    io_service_t service;
    IOUSBDeviceInterface650 **device;
    IOUSBInterfaceInterface650 **audioOut;  // IF 0
    IOUSBInterfaceInterface650 **audioIn;   // IF 1
};

namespace {

// Device ops backed by an IOUSBDeviceInterface650.

int DevOpen(void *ctx) {
    IOUSBDeviceInterface650 **d = (IOUSBDeviceInterface650 **)ctx;
    return MapReturn((*d)->USBDeviceOpen(d));
}
int DevCountInterfaces(void *ctx) {
    IOUSBDeviceInterface650 **d   = (IOUSBDeviceInterface650 **)ctx;
    IOUSBFindInterfaceRequest req = {kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare};
    io_iterator_t it              = 0;
    if ((*d)->CreateInterfaceIterator(d, &req, &it) != kIOReturnSuccess) return GT10_ERR;
    int n = 0;
    io_service_t s;
    while ((s = IOIteratorNext(it)) != 0) {
        n++;
        IOObjectRelease(s);
    }
    // IOIteratorNext returns 0 both at the end and when the iteration was
    // disrupted. A disrupted count must not read as zero, which would wrongly
    // authorize SetConfiguration and destroy another owner's interfaces.
    const boolean_t valid = IOIteratorIsValid(it);
    IOObjectRelease(it);
    return valid ? n : GT10_ERR;
}
int DevDescValue(void *ctx, uint8_t *out) {
    IOUSBDeviceInterface650 **d          = (IOUSBDeviceInterface650 **)ctx;
    IOUSBConfigurationDescriptorPtr desc = nullptr;
    if ((*d)->GetConfigurationDescriptorPtr(d, 0, &desc) != kIOReturnSuccess || desc == nullptr) {
        return GT10_ERR;
    }
    *out = desc->bConfigurationValue;
    return GT10_OK;
}
int DevSetConfig(void *ctx, uint8_t value) {
    IOUSBDeviceInterface650 **d = (IOUSBDeviceInterface650 **)ctx;
    const int r                 = MapReturn((*d)->SetConfiguration(d, value));
    USB_LOG("SetConfiguration(%u) -> %d", value, r);
    return r;
}
void DevClose(void *ctx) {
    IOUSBDeviceInterface650 **d = (IOUSBDeviceInterface650 **)ctx;
    (*d)->USBDeviceClose(d);
}

// Interface ops backed by an IOUSBInterfaceInterface650.

int IfNumber(void *ctx, uint8_t *out) {
    IOUSBInterfaceInterface650 **i = (IOUSBInterfaceInterface650 **)ctx;
    UInt8 v                        = 0xFF;
    const int r                    = MapReturn((*i)->GetInterfaceNumber(i, &v));
    *out                           = v;
    return r;
}
int IfAltSetting(void *ctx, uint8_t *out) {
    IOUSBInterfaceInterface650 **i = (IOUSBInterfaceInterface650 **)ctx;
    UInt8 v                        = 0xFF;
    const int r                    = MapReturn((*i)->GetAlternateSetting(i, &v));
    *out                           = v;
    return r;
}
int IfOpen(void *ctx) {
    IOUSBInterfaceInterface650 **i = (IOUSBInterfaceInterface650 **)ctx;
    return MapReturn((*i)->USBInterfaceOpen(i));
}
int IfNumEndpoints(void *ctx, uint8_t *out) {
    IOUSBInterfaceInterface650 **i = (IOUSBInterfaceInterface650 **)ctx;
    UInt8 v                        = 0xFF;
    const int r                    = MapReturn((*i)->GetNumEndpoints(i, &v));
    *out                           = v;
    return r;
}
void IfClose(void *ctx) {
    IOUSBInterfaceInterface650 **i = (IOUSBInterfaceInterface650 **)ctx;
    (*i)->USBInterfaceClose(i);
}

IOUSBInterfaceInterface650 **MakeInterface(io_service_t service) {
    IOCFPlugInInterface **plugIn = nullptr;
    SInt32 score                 = 0;
    if (IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plugIn,
                                          &score) != kIOReturnSuccess ||
        plugIn == nullptr) {
        return nullptr;
    }
    IOUSBInterfaceInterface650 **intf = nullptr;
    (*plugIn)->QueryInterface(plugIn, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650),
                              (LPVOID *)&intf);
    (*plugIn)->Release(plugIn);
    return intf;
}

// Open an audio interface (IF 0 or IF 1) on alt 0. Returns the opened
// interface and its role, or NULL and GT10RoleNone.
IOUSBInterfaceInterface650 **OpenAudioInterface(io_service_t service, GT10InterfaceRole *outRole) {
    *outRole                          = GT10RoleNone;
    IOUSBInterfaceInterface650 **intf = MakeInterface(service);
    if (intf == nullptr) return nullptr;

    GT10InterfaceOps ops = {IfNumber, IfAltSetting, IfOpen, IfNumEndpoints, IfClose, intf};
    if (GT10OpenInterfaceVia(&ops, outRole) != GT10_OK) {
        (*intf)->Release(intf);  // never opened, or opened then closed by the core
        return nullptr;
    }
    return intf;
}

void CloseInterface(IOUSBInterfaceInterface650 **intf) {
    if (intf == nullptr) return;
    (*intf)->USBInterfaceClose(intf);
    (*intf)->Release(intf);
}

}  // namespace

static CFMutableDictionaryRef GT10MatchingDict(void) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (match == nullptr) return nullptr;

    const SInt32 vid   = kVendorRoland;
    const SInt32 pid   = kProductGT10Advanced;
    CFNumberRef vidRef = CFNumberCreate(nullptr, kCFNumberSInt32Type, &vid);
    CFNumberRef pidRef = CFNumberCreate(nullptr, kCFNumberSInt32Type, &pid);
    // CFDictionarySetValue with a null value aborts the process.
    if (vidRef == nullptr || pidRef == nullptr) {
        if (vidRef != nullptr) CFRelease(vidRef);
        if (pidRef != nullptr) CFRelease(pidRef);
        CFRelease(match);
        return nullptr;
    }
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), vidRef);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), pidRef);
    CFRelease(vidRef);
    CFRelease(pidRef);
    return match;
}

// Build the owner around an already-matched service. The service reference is
// consumed on success and on failure.
static GT10USBDevice *GT10USBDeviceAdopt(io_service_t service);

GT10USBDevice *GT10USBDeviceFind(void) {
    CFMutableDictionaryRef match = GT10MatchingDict();
    if (match == nullptr) return nullptr;

    // IOServiceGetMatchingService consumes one reference to the dictionary.
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (service == 0) {
        USB_LOG("device 0582:00da not found");
        return nullptr;
    }
    return GT10USBDeviceAdopt(service);
}

static GT10USBDevice *GT10USBDeviceAdopt(io_service_t service) {
    IOCFPlugInInterface **plugIn = nullptr;
    SInt32 score                 = 0;
    if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plugIn,
                                          &score) != kIOReturnSuccess ||
        plugIn == nullptr) {
        USB_LOG("no device plug-in interface");
        IOObjectRelease(service);
        return nullptr;
    }

    IOUSBDeviceInterface650 **device = nullptr;
    (*plugIn)->QueryInterface(plugIn, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650),
                              (LPVOID *)&device);
    (*plugIn)->Release(plugIn);
    if (device == nullptr) {
        USB_LOG("no device interface");
        IOObjectRelease(service);
        return nullptr;
    }

    GT10USBDevice *dev = (GT10USBDevice *)calloc(1, sizeof(GT10USBDevice));
    if (dev == nullptr) {
        (*device)->Release(device);
        IOObjectRelease(service);
        return nullptr;
    }
    dev->service = service;
    dev->device  = device;
    USB_LOG("found device 0582:00da");
    return dev;
}

int GT10USBDeviceEnsureConfigured(GT10USBDevice *dev) {
    if (dev == nullptr || dev->device == nullptr) return GT10_ERR;
    GT10DeviceOps ops = {DevOpen,      DevCountInterfaces, DevDescValue,
                         DevSetConfig, DevClose,           dev->device};
    return GT10ConfigureVia(&ops);
}

int GT10USBDeviceOpenInterfaces(GT10USBDevice *dev) {
    if (dev == nullptr || dev->device == nullptr) return GT10_ERR;
    IOUSBDeviceInterface650 **device = dev->device;

    IOUSBFindInterfaceRequest req;
    req.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting  = kIOUSBFindInterfaceDontCare;

    io_iterator_t iter = 0;
    if ((*device)->CreateInterfaceIterator(device, &req, &iter) != kIOReturnSuccess) {
        USB_LOG("CreateInterfaceIterator failed");
        return GT10_NOTFOUND;
    }

    io_service_t service;
    while ((service = IOIteratorNext(iter)) != 0) {
        GT10InterfaceRole role            = GT10RoleNone;
        IOUSBInterfaceInterface650 **intf = OpenAudioInterface(service, &role);
        IOObjectRelease(service);
        if (intf == nullptr) continue;

        if (role == GT10RoleAudioOut && dev->audioOut == nullptr) {
            dev->audioOut = intf;
        } else if (role == GT10RoleAudioIn && dev->audioIn == nullptr) {
            dev->audioIn = intf;
        } else {
            CloseInterface(intf);  // duplicate role
        }
    }
    IOObjectRelease(iter);

    if (dev->audioOut == nullptr || dev->audioIn == nullptr) {
        USB_LOG("did not open both audio interfaces (out %p, in %p)", (void *)dev->audioOut,
                (void *)dev->audioIn);
        return GT10_NOTFOUND;
    }
    USB_LOG("holding IF0 and IF1 on alt 0");
    return GT10_OK;
}

bool GT10USBDeviceHasInterfaces(const GT10USBDevice *dev) {
    return dev != nullptr && dev->audioOut != nullptr && dev->audioIn != nullptr;
}

IOUSBInterfaceInterface650 **GT10USBDeviceInterface(GT10USBDevice *dev, GT10InterfaceRole role) {
    if (dev == nullptr) return nullptr;
    if (role == GT10RoleAudioOut) return dev->audioOut;
    if (role == GT10RoleAudioIn) return dev->audioIn;
    return nullptr;
}

// ---- Checkpoint 4a live ----

namespace {

// Stream ops backed by an IOUSBInterfaceInterface650. The first raw IOReturn
// is kept, because MapReturn folds bandwidth and scheduling errors into one.
struct StreamCtx {
    IOUSBInterfaceInterface650 **intf;
    IOReturn firstError;
};

int Track(StreamCtx *c, IOReturn kr) {
    if (kr != kIOReturnSuccess && c->firstError == kIOReturnSuccess) c->firstError = kr;
    return kr == kIOReturnSuccess ? GT10_OK : GT10_ERR;
}

int StSetAlt(void *ctx, uint8_t alt) {
    StreamCtx *c = (StreamCtx *)ctx;
    return Track(c, (*c->intf)->SetAlternateInterface(c->intf, alt));
}
int StAlt(void *ctx, uint8_t *out) {
    StreamCtx *c = (StreamCtx *)ctx;
    UInt8 v      = 0xFF;
    const int r  = Track(c, (*c->intf)->GetAlternateSetting(c->intf, &v));
    *out         = v;
    return r;
}
int StEndpoints(void *ctx, uint8_t *out) {
    StreamCtx *c = (StreamCtx *)ctx;
    UInt8 v      = 0xFF;
    const int r  = Track(c, (*c->intf)->GetNumEndpoints(c->intf, &v));
    *out         = v;
    return r;
}
int PipeProps(IOUSBInterfaceInterface650 **intf, uint8_t pipeRef, GT10PipeInfo *out,
              IOReturn *outReturn) {
    IOUSBEndpointProperties props;
    memset(&props, 0, sizeof props);
    props.bVersion    = kUSBEndpointPropertiesVersion3;
    const IOReturn kr = (*intf)->GetPipePropertiesV3(intf, pipeRef, &props);
    *outReturn        = kr;
    if (kr != kIOReturnSuccess) return GT10_ERR;
    out->alt            = props.bAlternateSetting;
    out->direction      = props.bDirection;
    out->endpointNumber = props.bEndpointNumber;
    out->transferType   = props.bTransferType;
    out->syncType       = props.bSyncType;
    out->usageType      = props.bUsageType;
    out->interval       = props.bInterval;
    out->maxPacketSize  = props.wMaxPacketSize;
    return GT10_OK;
}
int StPipeProps(void *ctx, uint8_t pipeRef, GT10PipeInfo *out) {
    StreamCtx *c = (StreamCtx *)ctx;
    IOReturn kr  = kIOReturnSuccess;
    PipeProps(c->intf, pipeRef, out, &kr);
    return Track(c, kr);
}

}  // namespace

int GT10USBDeviceEnableStreams(GT10USBDevice *dev, GT10StreamReport *report) {
    if (report == nullptr) return GT10_ERR;
    memset(report, 0, sizeof *report);
    report->result = GT10_NOTFOUND;
    if (!GT10USBDeviceHasInterfaces(dev) || dev->device == nullptr) return report->result;

    UInt8 speed = 0xFF;
    if ((*dev->device)->GetDeviceSpeed(dev->device, &speed) != kIOReturnSuccess) {
        report->result = GT10_ERR;
        return report->result;
    }
    report->speed = speed;

    StreamCtx outCtx      = {dev->audioOut, kIOReturnSuccess};
    StreamCtx inCtx       = {dev->audioIn, kIOReturnSuccess};
    GT10StreamOps outOps  = {StSetAlt, StAlt, StEndpoints, StPipeProps, &outCtx};
    GT10StreamOps inOps   = {StSetAlt, StAlt, StEndpoints, StPipeProps, &inCtx};
    report->result        = GT10EnableStreamsVia(&outOps, &inOps, speed, report->pipes);
    report->firstError[0] = outCtx.firstError;
    report->firstError[1] = inCtx.firstError;
    return report->result;
}

int GT10USBDeviceDisableStreams(GT10USBDevice *dev) {
    if (!GT10USBDeviceHasInterfaces(dev)) return GT10_ALT_UNKNOWN;
    StreamCtx outCtx     = {dev->audioOut, kIOReturnSuccess};
    StreamCtx inCtx      = {dev->audioIn, kIOReturnSuccess};
    GT10StreamOps outOps = {StSetAlt, StAlt, StEndpoints, StPipeProps, &outCtx};
    GT10StreamOps inOps  = {StSetAlt, StAlt, StEndpoints, StPipeProps, &inCtx};
    const int in         = GT10DisableStreamVia(&inOps);
    const int out        = GT10DisableStreamVia(&outOps);
    return (in == GT10_OK && out == GT10_OK) ? GT10_OK : GT10_ALT_UNKNOWN;
}

// ---- Checkpoint 4b live ----

namespace {

// Everything the completion can reach. It lives on the heap so an abandoned
// read can leave it allocated after this function returns.
struct ReadCtx {
    IOUSBInterfaceInterface650 **intf;
    uint8_t *buffer;
    IOUSBIsocFrame *frames;
    uint32_t numFrames;
    uint32_t leadFrames;
    uint64_t busFrame;
    uint64_t frameStart;
    IOReturn submitReturn;
    IOReturn completionReturn;
    bool completed;
};

void ReadCompleted(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    ReadCtx *c          = (ReadCtx *)refcon;
    c->completionReturn = result;
    c->completed        = true;
}

int ReadSubmit(void *ctx) {
    ReadCtx *c   = (ReadCtx *)ctx;
    UInt64 frame = 0;
    AbsoluteTime at;
    IOReturn kr = (*c->intf)->GetBusFrameNumber(c->intf, &frame, &at);
    if (kr != kIOReturnSuccess) {
        c->submitReturn = kr;
        return GT10_ERR;
    }
    uint64_t start = 0;
    if (!GT10FrameStart(frame, c->leadFrames, &start)) {
        c->submitReturn = kIOReturnBadArgument;
        return GT10_ERR;
    }
    // Nothing runs between reading the frame and submitting.
    kr = (*c->intf)->ReadIsochPipeAsync(c->intf, kGT10PipeRef, c->buffer, start, c->numFrames,
                                        c->frames, ReadCompleted, c);
    c->busFrame     = frame;
    c->frameStart   = start;
    c->submitReturn = kr;
    return kr == kIOReturnSuccess ? GT10_OK : GT10_ERR;
}

bool ReadWait(void *ctx, unsigned ms) {
    ReadCtx *c = (ReadCtx *)ctx;
    const uint64_t deadline =
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (uint64_t)ms * NSEC_PER_MSEC;
    // A return from the run loop is not a completion. Only the flag is.
    while (!c->completed) {
        const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now >= deadline) break;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, (double)(deadline - now) / NSEC_PER_SEC, true);
    }
    return c->completed;
}

int ReadAbort(void *ctx) {
    ReadCtx *c = (ReadCtx *)ctx;
    return MapReturn((*c->intf)->AbortPipe(c->intf, kGT10PipeRef));
}

}  // namespace

int GT10USBDeviceReadOnce(GT10USBDevice *dev, uint32_t numFrames, uint32_t leadFrames,
                          unsigned waitMs, GT10ReadReport *report) {
    if (report == nullptr) return GT10_ERR;
    memset(report, 0, sizeof *report);
    report->outcome = GT10ReadRejected;
    if (!GT10USBDeviceHasInterfaces(dev) || dev->device == nullptr) return GT10_NOTFOUND;
    IOUSBInterfaceInterface650 **intf = dev->audioIn;

    // Validate the live pipe right before use. A checkpoint 4a result from
    // earlier says nothing about the pipe now.
    UInt8 speed = 0xFF;
    UInt8 alt   = 0xFF;
    GT10PipeInfo pipe;
    memset(&pipe, 0, sizeof pipe);
    IOReturn kr = (*dev->device)->GetDeviceSpeed(dev->device, &speed);
    if (kr != kIOReturnSuccess) {
        report->setupFailure = "device speed";
        report->setupReturn  = kr;
        return GT10_ERR;
    }
    kr = (*intf)->GetAlternateSetting(intf, &alt);
    if (kr != kIOReturnSuccess || alt != 1) {
        report->setupFailure = "alt 1";
        report->setupReturn  = kr;
        return GT10_ERR;
    }
    if (PipeProps(intf, kGT10PipeRef, &pipe, &kr) != GT10_OK ||
        !GT10PipeFitsRole(GT10RoleAudioIn, speed, &pipe)) {
        report->setupFailure = "pipe properties";
        report->setupReturn  = kr;
        return GT10_ERR;
    }

    GT10ReadPlan plan;
    if (!GT10PlanRead(pipe.maxPacketSize, numFrames, &plan)) {
        report->setupFailure = "read plan";
        return GT10_ERR;
    }

    // Create retains the source for the caller in every case, so it is
    // released after removal.
    CFRunLoopSourceRef source = nullptr;
    kr                        = (*intf)->CreateInterfaceAsyncEventSource(intf, &source);
    if (kr != kIOReturnSuccess || source == nullptr) {
        report->setupFailure = "async event source";
        report->setupReturn  = kr;
        return GT10_ERR;
    }

    ReadCtx *c             = (ReadCtx *)calloc(1, sizeof(ReadCtx));
    uint8_t *buffer        = (uint8_t *)malloc(plan.bufferBytes);
    IOUSBIsocFrame *frames = (IOUSBIsocFrame *)calloc(plan.numFrames, sizeof(IOUSBIsocFrame));
    if (c == nullptr || buffer == nullptr || frames == nullptr) {
        free(c);
        free(buffer);
        free(frames);
        CFRelease(source);
        report->setupFailure = "allocation";
        return GT10_ERR;
    }

    memset(buffer, kGT10ReadFill, plan.bufferBytes);
    for (uint32_t i = 0; i < plan.numFrames; i++) {
        frames[i].frStatus   = kIOReturnNotReady;
        frames[i].frReqCount = plan.reqPerFrame;
        frames[i].frActCount = 0;
    }
    c->intf       = intf;
    c->buffer     = buffer;
    c->frames     = frames;
    c->numFrames  = plan.numFrames;
    c->leadFrames = leadFrames;

    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    GT10ReadOps ops = {ReadSubmit, ReadWait, ReadAbort, c};
    report->outcome = GT10ReadOnceVia(&ops, waitMs, waitMs);

    report->submitReturn     = c->submitReturn;
    report->completionReturn = c->completionReturn;
    report->busFrame         = c->busFrame;
    report->frameStart       = c->frameStart;
    report->numFrames        = plan.numFrames;
    report->buffer           = buffer;
    report->bufferBytes      = plan.bufferBytes;

    if (report->outcome == GT10ReadAbandoned) {
        // The kernel may still write the buffer and frame list. Keep all of it.
        return GT10_ERR;
    }

    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    CFRelease(source);
    for (uint32_t i = 0; i < plan.numFrames; i++) {
        report->frameStatus[i] = frames[i].frStatus;
        report->frameReq[i]    = frames[i].frReqCount;
        report->frameAct[i]    = frames[i].frActCount;
    }
    free(frames);
    free(c);
    return report->outcome == GT10ReadRejected ? GT10_ERR : GT10_OK;
}

void GT10USBDeviceFree(GT10USBDevice *dev) {
    if (dev == nullptr) return;
    CloseInterface(dev->audioOut);
    CloseInterface(dev->audioIn);
    if (dev->device != nullptr) (*dev->device)->Release(dev->device);
    if (dev->service != 0) IOObjectRelease(dev->service);
    free(dev);
}

// ---- Arrival and removal monitor ----

struct GT10USBMonitor {
    dispatch_queue_t queue;
    IONotificationPortRef port;
    io_iterator_t arrivals;
    io_object_t termination;
    GT10USBDevice *device;
    GT10USBHeldChanged onChange;
    void *ctx;
    // Bumped on every release. A retry queued against an older attachment
    // carries the old value and drops itself.
    uint64_t generation;
    bool held;
    // The scheduled retry, held so teardown can cancel it. Without this a
    // pending block outlives the monitor and dereferences freed memory.
    dispatch_block_t pendingRetry;
    GT10Stream *stream;
    GT10StreamConfig streamConfig;
    GT10StreamOwner owner;
    bool announced;  // the owner accepted running true for this stream
    // Bumped for every stream start and stop. A fault reported for an older
    // stream carries an older token and is ignored.
    uint64_t streamToken;
    // An engine that could not be joined may still have transfers in the
    // kernel and can still call back into this monitor. Permanent: the device
    // is never streamed again or closed, and the monitor is never freed.
    bool abandoned;
    // An interface left on an unknown setting. This belongs to the attachment,
    // not to the process: a removal retires it, because the next arrival
    // enumerates fresh interfaces on alt 0.
    bool altUnknown;
    // Set while an owner wants a stream, so one starts again after the pedal
    // is unplugged and plugged back in.
    bool wantStream;
    // Starts spent recovering from a fault, for this stream request. Cleared
    // when the owner asks again and when the attachment retires.
    int faultRestarts;
};

namespace {

void MonitorTryAcquire(GT10USBMonitor *m, int attempt);
bool MonitorStartStream(GT10USBMonitor *m);

void MonitorReport(GT10USBMonitor *m, bool held) {
    if (m->held == held) return;
    m->held = held;
    if (m->onChange != nullptr) m->onChange(held, m->ctx);
}

void MonitorCancelRetry(GT10USBMonitor *m) {
    if (m->pendingRetry == nullptr) return;
    dispatch_block_cancel(m->pendingRetry);
    Block_release(m->pendingRetry);
    m->pendingRetry = nullptr;
}

// GT10_OK when nothing runs any more and both interfaces are confirmed on alt 0.
// A removed device has nothing to restore and nothing to quarantine: its
// interfaces are gone, and the next arrival enumerates fresh ones on alt 0.
int MonitorStopStream(GT10USBMonitor *m, bool deviceGone) {
    if (m->stream == nullptr) return m->abandoned || m->altUnknown ? GT10_ERR : GT10_OK;
    m->streamToken++;
    GT10StreamStats stats;
    int result = GT10_OK;
    if (GT10StreamStop(&m->stream, &stats) != GT10_OK) {
        USB_LOG("stream abandoned, quarantining the device");
        m->abandoned = true;
        result       = GT10_ERR;
    } else if (deviceGone) {
        USB_LOG("stream stopped by removal");
    } else if (GT10USBDeviceDisableStreams(m->device) != GT10_OK) {
        // A removal can reach this first, through a faulted engine, so this
        // state must not outlive the attachment.
        USB_LOG("alt 0 unconfirmed after stop");
        m->altUnknown = true;
        result        = GT10_ERR;
    } else {
        USB_LOG("stream stopped: %llu frames captured, %llu played, error 0x%08x",
                (unsigned long long)stats.capturedFrames, (unsigned long long)stats.playedFrames,
                (uint32_t)stats.firstError);
    }
    if (m->announced) {
        m->announced = false;
        if (m->owner.stateChanged != nullptr) m->owner.stateChanged(m->owner.context, false);
    }
    return result;
}

// Engine thread. The stop runs on the monitor queue, behind whatever is there.
void MonitorEngineFaulted(void *ctx, uint64_t token) {
    GT10USBMonitor *m = (GT10USBMonitor *)ctx;
    dispatch_async(m->queue, ^{
        if (m->stream == nullptr || m->streamToken != token) return;
        USB_LOG("stream faulted, stopping");
        MonitorStopStream(m, false);
        // Nothing else would ever start it again. The HAL does not repeat
        // StartIO, so without this the device stays alive and silent until the
        // app stops and starts audio, or the pedal is unplugged.
        if (m->wantStream && m->faultRestarts < kMaxFaultRestarts) {
            m->faultRestarts++;
            USB_LOG("starting the stream again after a fault, attempt %d", m->faultRestarts);
            MonitorStartStream(m);
        }
    });
}

bool MonitorStartStream(GT10USBMonitor *m) {
    if (m->stream != nullptr) return true;
    if (m->device == nullptr || m->abandoned || m->altUnknown) return false;
    if (m->owner.prepare != nullptr && !m->owner.prepare(m->owner.context)) return false;
    GT10StreamReport report;
    const int enabled = GT10USBDeviceEnableStreams(m->device, &report);
    if (enabled != GT10_OK) {
        USB_LOG("enable streams failed: %d", enabled);
        if (enabled == GT10_ALT_UNKNOWN) m->altUnknown = true;
        return false;
    }

    m->streamToken++;
    GT10StreamConfig config = m->streamConfig;
    config.faulted          = MonitorEngineFaulted;
    config.faultContext     = m;
    config.faultToken       = m->streamToken;
    bool startAbandoned     = false;
    m->stream               = GT10StreamStart(m->device, &config, &startAbandoned);
    if (m->stream == nullptr) {
        if (startAbandoned) {
            // Restoring alt 0 under an engine the kernel still owns is the one
            // thing that must never happen.
            USB_LOG("stream start did not settle, quarantining the device");
            m->abandoned = true;
        } else if (GT10USBDeviceDisableStreams(m->device) != GT10_OK) {
            m->altUnknown = true;
        }
        return false;
    }

    // Wait for the first timestamp here, so a removal or fault queued meanwhile
    // runs only after this start has either committed or backed out.
    bool published = config.timeStamp == nullptr;
    for (int waited = 0; !published && waited < kFirstTimeStampWaitMs; waited += 2) {
        GT10StreamStats stats;
        GT10StreamGetStats(m->stream, &stats);
        if (!stats.running) break;
        int64_t sample;
        uint64_t host;
        published = GT10TimeStampRead(config.timeStamp, &sample, &host);
        if (!published) usleep(2000);
    }
    if (!published) {
        USB_LOG("no timestamp from the stream, stopping it");
        MonitorStopStream(m, false);
        return false;
    }
    if (m->owner.stateChanged != nullptr && !m->owner.stateChanged(m->owner.context, true)) {
        MonitorStopStream(m, false);
        return false;
    }
    m->announced = true;
    return true;
}

void MonitorRelease(GT10USBMonitor *m, bool deviceGone) {
    m->generation += 1;
    MonitorCancelRetry(m);
    MonitorStopStream(m, deviceGone);
    if (m->termination != 0) {
        IOObjectRelease(m->termination);
        m->termination = 0;
    }
    if (m->device != nullptr && !m->abandoned) GT10USBDeviceFree(m->device);
    m->device = nullptr;
    // The interfaces this belonged to are gone with the attachment.
    m->altUnknown    = false;
    m->faultRestarts = 0;
    MonitorReport(m, false);
}

void TerminationCallback(void *refcon, io_service_t service, uint32_t type, void *arg) {
    (void)service;
    (void)arg;
    if (type != kIOMessageServiceIsTerminated) return;
    GT10USBMonitor *m = (GT10USBMonitor *)refcon;
    USB_LOG("device removed, releasing interfaces");
    MonitorRelease(m, true);
    // An arrival that landed while the old attachment was still held was
    // discarded, so nothing else would look for the pedal now.
    if (!m->abandoned) MonitorTryAcquire(m, 0);
}

void MonitorTryAcquire(GT10USBMonitor *m, int attempt) {
    if (m->held) return;

    // Enumerate fresh on every attempt. A snapshot taken before another owner
    // instantiated the interfaces would stay empty forever.
    int result         = GT10_NOTFOUND;
    GT10USBDevice *dev = GT10USBDeviceFind();
    if (dev != nullptr) {
        result = GT10USBDeviceEnsureConfigured(dev);
        if (result == GT10_OK) result = GT10USBDeviceOpenInterfaces(dev);
        if (result != GT10_OK) {
            GT10USBDeviceFree(dev);
            dev = nullptr;
        }
    }

    if (result == GT10_OK) {
        // Without this notification a removal is invisible, and the monitor
        // would hold interfaces that no longer exist for the life of the
        // process. Treat it as a failed acquisition and retry.
        if (IOServiceAddInterestNotification(m->port, dev->service, kIOGeneralInterest,
                                             TerminationCallback, m,
                                             &m->termination) != kIOReturnSuccess) {
            m->termination = 0;
            GT10USBDeviceFree(dev);
            result = GT10_ERR;
        } else {
            m->device = dev;
            USB_LOG("acquired IF0 and IF1 on attempt %d", attempt);
            MonitorReport(m, true);
            // The owner still wants a stream after a replug, and it cannot ask
            // for one from here: its request would wait on this queue.
            if (m->wantStream) MonitorStartStream(m);
            return;
        }
    }

    const GT10RetryPlan plan = GT10PlanRetry(attempt, result);
    if (!plan.retry) {
        USB_LOG("giving up acquisition after attempt %d", attempt);
        return;
    }
    const uint64_t generation = m->generation;
    MonitorCancelRetry(m);
    m->pendingRetry = dispatch_block_create(DISPATCH_BLOCK_ASSIGN_CURRENT, ^{
        if (m->generation != generation) return;  // a removal superseded this
        MonitorCancelRetry(m);
        MonitorTryAcquire(m, attempt + 1);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)plan.delayMs * NSEC_PER_MSEC),
                   m->queue, m->pendingRetry);
}

void ArrivalCallback(void *refcon, io_iterator_t iterator) {
    GT10USBMonitor *m = (GT10USBMonitor *)refcon;
    io_service_t service;
    bool arrived = false;
    // The iterator must be drained or the notification never re-arms.
    while ((service = IOIteratorNext(iterator)) != 0) {
        IOObjectRelease(service);
        arrived = true;
    }
    if (arrived && !m->held) MonitorTryAcquire(m, 0);
}

}  // namespace

GT10USBMonitor *GT10USBMonitorStart(void *serialQueue, GT10USBHeldChanged onChange, void *ctx) {
    if (serialQueue == nullptr) return nullptr;

    GT10USBMonitor *m = (GT10USBMonitor *)calloc(1, sizeof(GT10USBMonitor));
    if (m == nullptr) return nullptr;
    m->queue    = (dispatch_queue_t)serialQueue;
    m->onChange = onChange;
    m->ctx      = ctx;

    m->port = IONotificationPortCreate(kIOMainPortDefault);
    if (m->port == nullptr) {
        free(m);
        return nullptr;
    }
    IONotificationPortSetDispatchQueue(m->port, m->queue);

    CFMutableDictionaryRef match = GT10MatchingDict();
    if (match == nullptr) {
        IONotificationPortDestroy(m->port);
        free(m);
        return nullptr;
    }
    // IOServiceAddMatchingNotification consumes the dictionary reference.
    if (IOServiceAddMatchingNotification(m->port, kIOFirstMatchNotification, match, ArrivalCallback,
                                         m, &m->arrivals) != kIOReturnSuccess) {
        IONotificationPortDestroy(m->port);
        free(m);
        return nullptr;
    }
    // Draining arms the notification and handles a device already attached. It
    // runs on the monitor queue because that queue is already delivering
    // notifications: a drain on this thread would race a removal that lands
    // while acquisition is still opening the interfaces.
    dispatch_sync(m->queue, ^{
        ArrivalCallback(m, m->arrivals);
    });
    return m;
}

int GT10USBMonitorSetStream(GT10USBMonitor *m, const GT10StreamConfig *config,
                            const GT10StreamOwner *owner) {
    if (m == nullptr) return GT10_ERR;
    __block int result = GT10_ERR;
    dispatch_sync(m->queue, ^{
        m->wantStream     = false;
        m->faultRestarts  = 0;
        const int stopped = MonitorStopStream(m, false);
        if (config == nullptr) {
            result = stopped;
            return;
        }
        m->streamConfig = *config;
        m->owner        = owner != nullptr ? *owner : GT10StreamOwner{};
        m->wantStream   = true;
        // Acquisition gives up after a few seconds and then waits for an
        // arrival. A restart of coreaudiod produces exactly the case that
        // outlives it: the previous host still holds the interfaces, and the
        // pedal is never unplugged, so no arrival ever comes. A client asking
        // for audio is the other moment worth trying.
        if (!m->held && !m->abandoned) MonitorTryAcquire(m, 0);
        result = stopped == GT10_OK && MonitorStartStream(m) ? GT10_OK : GT10_ERR;
    });
    return result;
}

void GT10USBMonitorStop(GT10USBMonitor *m) {
    if (m == nullptr) return;
    // Tear down on the monitor's own queue so no callback or retry is running
    // or can start, then free once that queue is past this block.
    dispatch_sync(m->queue, ^{
        m->wantStream = false;
        MonitorRelease(m, false);
        if (m->arrivals != 0) {
            IOObjectRelease(m->arrivals);
            m->arrivals = 0;
        }
        if (m->port != nullptr) {
            IONotificationPortDestroy(m->port);
            m->port = nullptr;
        }
    });
    // A fault reported while the stream was stopping queued a block that
    // references the monitor. A joined stream queues nothing after this flush.
    // An abandoned one still might, so its monitor stays allocated.
    dispatch_sync(m->queue, ^{
                  });
    if (!m->abandoned) free(m);
}
