// Fake USB backend linked into the host-side plug-in test in place of the real
// IOKit implementation, so `make test` never touches hardware. Tests drive the
// held state and play the stream engine's part through the hooks at the end.

#include "../Driver/GT10USBDevice.h"
#include "../Driver/GT10USBStream.h"
#include "../Sources/GT10Clock.h"

#include <mach/mach_time.h>
#include <stddef.h>
#include <string.h>

GT10USBDevice *GT10USBDeviceFind(void) { return NULL; }
int GT10USBDeviceEnsureConfigured(GT10USBDevice *dev) {
    (void)dev;
    return GT10_ERR;
}
int GT10USBDeviceOpenInterfaces(GT10USBDevice *dev) {
    (void)dev;
    return GT10_ERR;
}
bool GT10USBDeviceHasInterfaces(const GT10USBDevice *dev) {
    (void)dev;
    return false;
}
void GT10USBDeviceFree(GT10USBDevice *dev) { (void)dev; }

static int gMonitorToken;
static GT10USBHeldChanged gHeldChanged;
static void *gHeldContext;
static bool gHeld;
static int gStartResult     = GT10_OK;
static bool gPublishOnStart = true;
static bool gStreaming;
static bool gAnnounced;
static GT10StreamConfig gConfig;
static GT10StreamOwner gOwner;
static bool gWantStream;
static int gStreamStarts, gStreamStops, gPrepares;

GT10USBMonitor *GT10USBMonitorStart(void *q, GT10USBHeldChanged cb, void *ctx) {
    (void)q;
    gHeldChanged = cb;
    gHeldContext = ctx;
    return (GT10USBMonitor *)&gMonitorToken;
}

void GT10USBMonitorStop(GT10USBMonitor *m) { (void)m; }

// Mirrors the monitor: a started stream that was announced reports its end.
static void EndStream(void) {
    if (!gStreaming) return;
    gStreaming = false;
    gStreamStops++;
    if (gAnnounced) {
        gAnnounced = false;
        if (gOwner.stateChanged != NULL) gOwner.stateChanged(gOwner.context, false);
    }
}

// Mirrors the monitor's start: prepare, publish the first timestamp, announce.
static int StartStream(void) {
    if (!gHeld) return GT10_ERR;
    if (gOwner.prepare != NULL) {
        gPrepares++;
        if (!gOwner.prepare(gOwner.context)) return GT10_ERR;
    }
    if (gStartResult != GT10_OK) return GT10_ERR;
    gStreaming = true;
    gStreamStarts++;
    if (!gPublishOnStart || gConfig.timeStamp == NULL) {
        EndStream();
        return GT10_ERR;
    }
    GT10TimeStampPublish(gConfig.timeStamp, 0, mach_absolute_time());
    if (gOwner.stateChanged != NULL && !gOwner.stateChanged(gOwner.context, true)) {
        EndStream();
        return GT10_ERR;
    }
    gAnnounced = true;
    return GT10_OK;
}

int GT10USBMonitorSetStream(GT10USBMonitor *m, const GT10StreamConfig *config,
                            const GT10StreamOwner *owner) {
    (void)m;
    gWantStream = false;
    EndStream();
    if (config == NULL) return GT10_OK;
    gConfig = *config;
    memset(&gOwner, 0, sizeof gOwner);
    if (owner != NULL) gOwner = *owner;
    gWantStream = true;
    return StartStream();
}

// ---- test hooks ----

void FakeUSBSetHeld(bool held) {
    gHeld = held;
    // The real monitor stops the stream on removal, and starts one again after
    // it reacquires the device, when an owner still wants one.
    if (!held) EndStream();
    if (gHeldChanged != NULL) gHeldChanged(held, gHeldContext);
    if (held && gWantStream) StartStream();
}

// An engine fault: the monitor stops the stream on its queue.
void FakeUSBFault(void) { EndStream(); }

// Reports running again, as a start that lost a race with StopIO would.
bool FakeUSBAnnounceAgain(void) {
    return gOwner.stateChanged != NULL && gOwner.stateChanged(gOwner.context, true);
}

int FakeUSBPrepares(void) { return gPrepares; }

void FakeUSBSetStartResult(int result) { gStartResult = result; }
void FakeUSBSetPublishOnStart(bool publish) { gPublishOnStart = publish; }
bool FakeUSBStreaming(void) { return gStreaming; }
const GT10StreamConfig *FakeUSBConfig(void) { return &gConfig; }
int FakeUSBStreamStarts(void) { return gStreamStarts; }
int FakeUSBStreamStops(void) { return gStreamStops; }
