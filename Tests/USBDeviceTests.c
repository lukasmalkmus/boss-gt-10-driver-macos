// Tests for GT10USBDevice: the pure decision logic and the control-flow cores
// driven through fake operations. The live IOKit path is exercised on hardware
// through `make usb-probe`, never here.

#include "../Driver/GT10USBDevice.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

// ---- pure logic ----

static void TestConfigPlan(void) {
    // No interface nubs: configure to instantiate them.
    GT10ConfigPlan p = GT10PlanConfiguration(0, 1);
    CHECK(p.valid && p.configure && p.value == 1);

    // Value comes from the descriptor, never a hardcoded 1.
    p = GT10PlanConfiguration(0, 3);
    CHECK(p.valid && p.configure && p.value == 3);

    // Nubs already present: valid, but never re-set, which would destroy them.
    p = GT10PlanConfiguration(3, 1);
    CHECK(p.valid && !p.configure);
    p = GT10PlanConfiguration(1, 1);
    CHECK(p.valid && !p.configure);

    // Descriptor value 0 is the unconfigured state, not a target.
    p = GT10PlanConfiguration(0, 0);
    CHECK(!p.valid && !p.configure);
}

static void TestInterfaceRole(void) {
    CHECK(GT10RoleForInterface(0) == GT10RoleAudioOut);
    CHECK(GT10RoleForInterface(1) == GT10RoleAudioIn);
    CHECK(GT10RoleForInterface(2) == GT10RoleNone);  // MIDI, not ours
    CHECK(GT10RoleForInterface(255) == GT10RoleNone);
}

// ---- fake device ops for GT10ConfigureVia ----

typedef struct {
    int openReturn;
    int interfaceCount;  // negative means the count op fails
    int descReturn;
    uint8_t descValue;
    int setConfigReturn;
    // recorded
    int opens, counts, sets, closes;
    uint8_t setValue;
} FakeDev;

static int FDevOpen(void *c) {
    FakeDev *f = c;
    f->opens++;
    return f->openReturn;
}
static int FDevCount(void *c) {
    FakeDev *f = c;
    f->counts++;
    return f->interfaceCount;
}
static int FDevDesc(void *c, uint8_t *o) {
    FakeDev *f = c;
    *o         = f->descValue;
    return f->descReturn;
}
static int FDevSet(void *c, uint8_t v) {
    FakeDev *f = c;
    f->sets++;
    f->setValue = v;
    return f->setConfigReturn;
}
static void FDevClose(void *c) {
    FakeDev *f = c;
    f->closes++;
}

static GT10DeviceOps DevOpsFor(FakeDev *f) {
    GT10DeviceOps ops = {FDevOpen, FDevCount, FDevDesc, FDevSet, FDevClose, f};
    return ops;
}

static void TestConfigureVia(void) {
    // No nubs: exactly one SetConfiguration with the descriptor value, and the
    // session is closed. This is the macOS 26 case where the device reports
    // itself configured but exposes no interfaces.
    FakeDev f         = {0};
    f.interfaceCount  = 0;
    f.descValue       = 1;
    GT10DeviceOps ops = DevOpsFor(&f);
    CHECK(GT10ConfigureVia(&ops) == GT10_OK);
    CHECK(f.opens == 1 && f.sets == 1 && f.setValue == 1 && f.closes == 1);

    // Nubs already present: never SetConfiguration, which would destroy the
    // interfaces another owner holds. Still closed.
    memset(&f, 0, sizeof f);
    f.interfaceCount = 3;
    f.descValue      = 1;
    ops              = DevOpsFor(&f);
    CHECK(GT10ConfigureVia(&ops) == GT10_OK);
    CHECK(f.opens == 1 && f.counts == 1 && f.sets == 0 && f.closes == 1);

    // Count failure MUST NOT configure. This is the guard that protects the
    // MIDI process's interfaces.
    memset(&f, 0, sizeof f);
    f.interfaceCount = GT10_ERR;
    f.descValue      = 1;
    ops              = DevOpsFor(&f);
    CHECK(GT10ConfigureVia(&ops) == GT10_ERR);
    CHECK(f.opens == 1 && f.sets == 0 && f.closes == 1);

    // Another owner holds the session: reported success, no count, no
    // configure, no close (we never opened).
    memset(&f, 0, sizeof f);
    f.openReturn = GT10_BUSY;
    ops          = DevOpsFor(&f);
    CHECK(GT10ConfigureVia(&ops) == GT10_OK);
    CHECK(f.opens == 1 && f.counts == 0 && f.sets == 0 && f.closes == 0);

    // Open fails for another reason: error, no configure, no close.
    memset(&f, 0, sizeof f);
    f.openReturn = GT10_ERR;
    ops          = DevOpsFor(&f);
    CHECK(GT10ConfigureVia(&ops) == GT10_ERR);
    CHECK(f.sets == 0 && f.closes == 0);

    // Descriptor read fails while nubs absent: no usable value, no configure,
    // still closed.
    memset(&f, 0, sizeof f);
    f.interfaceCount = 0;
    f.descReturn     = GT10_ERR;
    ops              = DevOpsFor(&f);
    CHECK(GT10ConfigureVia(&ops) == GT10_NOTFOUND);
    CHECK(f.sets == 0 && f.closes == 1);

    // A nonzero config value from the descriptor is honored, not hardcoded 1.
    memset(&f, 0, sizeof f);
    f.interfaceCount = 0;
    f.descValue      = 5;
    ops              = DevOpsFor(&f);
    CHECK(GT10ConfigureVia(&ops) == GT10_OK);
    CHECK(f.sets == 1 && f.setValue == 5);
}

// ---- fake interface ops for GT10OpenInterfaceVia ----

typedef struct {
    int numberReturn;
    uint8_t number;
    int altReturn;
    uint8_t alt;
    int openReturn;
    int endpointsReturn;
    uint8_t endpoints;
    // what GetAlternateSetting reports once the interface is open
    uint8_t altAfterOpen;
    int altReturnAfterOpen;
    // recorded
    int opens, closes;
} FakeIf;

static int FIfNum(void *c, uint8_t *o) {
    FakeIf *f = c;
    *o        = f->number;
    return f->numberReturn;
}
static int FIfAlt(void *c, uint8_t *o) {
    FakeIf *f = c;
    *o        = f->alt;
    return f->altReturn;
}
static int FIfOpen(void *c) {
    FakeIf *f = c;
    f->opens++;
    f->alt       = f->altAfterOpen;
    f->altReturn = f->altReturnAfterOpen;
    return f->openReturn;
}
static int FIfEnd(void *c, uint8_t *o) {
    FakeIf *f = c;
    *o        = f->endpoints;
    return f->endpointsReturn;
}
static void FIfClose(void *c) {
    FakeIf *f = c;
    f->closes++;
}

static GT10InterfaceOps IfOpsFor(FakeIf *f) {
    GT10InterfaceOps ops = {FIfNum, FIfAlt, FIfOpen, FIfEnd, FIfClose, f};
    return ops;
}

static void TestOpenInterfaceVia(void) {
    GT10InterfaceRole role;

    // IF0 on alt 0, zero endpoints: opened, role AudioOut. Zero endpoints is
    // expected on alt 0, not an error.
    FakeIf f             = {0};
    f.number             = 0;
    f.alt                = 0;
    f.endpoints          = 0;
    GT10InterfaceOps ops = IfOpsFor(&f);
    CHECK(GT10OpenInterfaceVia(&ops, &role) == GT10_OK);
    CHECK(role == GT10RoleAudioOut && f.opens == 1 && f.closes == 0);

    // IF1 on alt 0: opened, role AudioIn.
    memset(&f, 0, sizeof f);
    f.number = 1;
    ops      = IfOpsFor(&f);
    CHECK(GT10OpenInterfaceVia(&ops, &role) == GT10_OK);
    CHECK(role == GT10RoleAudioIn && f.opens == 1);

    // IF2 (MIDI): never opened, so the MIDI process keeps it.
    memset(&f, 0, sizeof f);
    f.number = 2;
    ops      = IfOpsFor(&f);
    CHECK(GT10OpenInterfaceVia(&ops, &role) == GT10_ERR);
    CHECK(role == GT10RoleNone && f.opens == 0);

    // An audio interface not on alt 0: rejected without opening, so this never
    // switches a setting.
    memset(&f, 0, sizeof f);
    f.number = 0;
    f.alt    = 1;
    ops      = IfOpsFor(&f);
    CHECK(GT10OpenInterfaceVia(&ops, &role) == GT10_ERR);
    CHECK(role == GT10RoleNone && f.opens == 0);

    // GetAlternateSetting failure: rejected without opening.
    memset(&f, 0, sizeof f);
    f.number    = 0;
    f.altReturn = GT10_ERR;
    ops         = IfOpsFor(&f);
    CHECK(GT10OpenInterfaceVia(&ops, &role) == GT10_ERR);
    CHECK(f.opens == 0);

    // USBInterfaceOpen failure: role stays None.
    memset(&f, 0, sizeof f);
    f.number     = 1;
    f.openReturn = GT10_ERR;
    ops          = IfOpsFor(&f);
    CHECK(GT10OpenInterfaceVia(&ops, &role) == GT10_ERR);
    CHECK(role == GT10RoleNone);

    // GetNumEndpoints failure after opening: the interface is closed again.
    memset(&f, 0, sizeof f);
    f.number          = 0;
    f.endpointsReturn = GT10_ERR;
    ops               = IfOpsFor(&f);
    CHECK(GT10OpenInterfaceVia(&ops, &role) == GT10_ERR);
    CHECK(f.opens == 1 && f.closes == 1);

    // GetInterfaceNumber failure: nothing opened.
    memset(&f, 0, sizeof f);
    f.numberReturn = GT10_ERR;
    ops            = IfOpsFor(&f);
    CHECK(GT10OpenInterfaceVia(&ops, &role) == GT10_ERR);
    CHECK(f.opens == 0);

    // Alt 0 before the open, alt 1 once open: closed again, never switched.
    memset(&f, 0, sizeof f);
    f.number       = 1;
    f.altAfterOpen = 1;
    ops            = IfOpsFor(&f);
    CHECK(GT10OpenInterfaceVia(&ops, &role) == GT10_ERR);
    CHECK(role == GT10RoleNone && f.opens == 1 && f.closes == 1);

    // GetAlternateSetting fails once open: closed again.
    memset(&f, 0, sizeof f);
    f.number             = 0;
    f.altReturnAfterOpen = GT10_ERR;
    ops                  = IfOpsFor(&f);
    CHECK(GT10OpenInterfaceVia(&ops, &role) == GT10_ERR);
    CHECK(role == GT10RoleNone && f.opens == 1 && f.closes == 1);
}

static void TestRetryPlan(void) {
    // Success never retries.
    GT10RetryPlan r = GT10PlanRetry(0, GT10_OK);
    CHECK(!r.retry);

    // Absent interfaces back off and look again, growing the delay.
    r = GT10PlanRetry(0, GT10_NOTFOUND);
    CHECK(r.retry && r.delayMs == 100);
    r = GT10PlanRetry(1, GT10_NOTFOUND);
    CHECK(r.retry && r.delayMs == 200);
    r = GT10PlanRetry(3, GT10_NOTFOUND);
    CHECK(r.retry && r.delayMs == 800);

    // The delay is capped, and the attempts are bounded so a dead device does
    // not retry forever.
    r = GT10PlanRetry(4, GT10_NOTFOUND);
    CHECK(r.retry && r.delayMs == 1600);
    r = GT10PlanRetry(5, GT10_NOTFOUND);
    CHECK(!r.retry);
    r = GT10PlanRetry(99, GT10_ERR);
    CHECK(!r.retry);

    // Other failures retry on the same schedule.
    r = GT10PlanRetry(0, GT10_ERR);
    CHECK(r.retry && r.delayMs == 100);

    // A negative attempt is refused rather than shifting by a negative amount.
    r = GT10PlanRetry(-1, GT10_ERR);
    CHECK(!r.retry);
}

// ---- checkpoint 4a ----

static GT10PipeInfo GoodPipe(GT10InterfaceRole role) {
    GT10PipeInfo p  = {0};
    p.alt           = 1;
    p.transferType  = 1;
    p.interval      = 1;
    p.maxPacketSize = 288;
    if (role == GT10RoleAudioOut) {
        p.direction      = 0;
        p.endpointNumber = 1;
        p.syncType       = 2;
    } else {
        p.direction      = 1;
        p.endpointNumber = 2;
        p.syncType       = 1;
    }
    return p;
}

static void TestPipeFitsRole(void) {
    GT10PipeInfo out = GoodPipe(GT10RoleAudioOut);
    GT10PipeInfo in  = GoodPipe(GT10RoleAudioIn);
    CHECK(GT10PipeFitsRole(GT10RoleAudioOut, 1, &out));
    CHECK(GT10PipeFitsRole(GT10RoleAudioIn, 1, &in));

    // Swapped roles, no role, no pipe.
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &out));
    CHECK(!GT10PipeFitsRole(GT10RoleAudioOut, 1, &in));
    CHECK(!GT10PipeFitsRole(GT10RoleNone, 1, &in));
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, NULL));

    // High speed would change the frame period the pacer assumes.
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 2, &in));
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 0, &in));

    GT10PipeInfo p;
    p     = in;
    p.alt = 0;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
    p           = in;
    p.direction = 0;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
    p                = in;
    p.endpointNumber = 1;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
    p              = in;
    p.transferType = 3;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
    p          = in;
    p.syncType = 2;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
    p          = out;
    p.syncType = 1;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioOut, 1, &p));
    p           = in;
    p.usageType = 1;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
    p          = in;
    p.interval = 2;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
    // Zero is what GetPipePropertiesV3 reports when the bus had no bandwidth.
    p               = in;
    p.maxPacketSize = 0;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
    p               = in;
    p.maxPacketSize = 270;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
    p               = in;
    p.maxPacketSize = 289;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
    // Raw bmAttributes sync bits (0x04 async) fail closed.
    p          = in;
    p.syncType = 0x04;
    CHECK(!GT10PipeFitsRole(GT10RoleAudioIn, 1, &p));
}

typedef struct {
    uint8_t alt;
    int setReturn[2];  // by target alt
    bool setTakes[2];  // a successful set changes alt
    int altReturn;
    int endpointsReturn;
    uint8_t endpoints;     // reported on alt 1
    bool endpointsAnyAlt;  // report endpoints whatever the setting
    int propsReturn;
    GT10PipeInfo pipe;
    // recorded
    uint8_t sets[8];
    int nsets;
    int propsCalls;
    uint8_t lastPipeRef;
} FakeStream;

static int FSSet(void *c, uint8_t alt) {
    FakeStream *f = c;
    if (f->nsets < 8) f->sets[f->nsets] = alt;
    f->nsets++;
    const int r = f->setReturn[alt & 1];
    if (r == GT10_OK && f->setTakes[alt & 1]) f->alt = alt;
    return r;
}
static int FSAlt(void *c, uint8_t *o) {
    FakeStream *f = c;
    *o            = f->alt;
    return f->altReturn;
}
static int FSEnd(void *c, uint8_t *o) {
    FakeStream *f = c;
    *o            = (f->alt == 1 || f->endpointsAnyAlt) ? f->endpoints : 0;
    return f->endpointsReturn;
}
static int FSProps(void *c, uint8_t ref, GT10PipeInfo *o) {
    FakeStream *f = c;
    f->propsCalls++;
    f->lastPipeRef = ref;
    *o             = f->pipe;
    return f->propsReturn;
}

static GT10StreamOps StreamOpsFor(FakeStream *f) {
    GT10StreamOps ops = {FSSet, FSAlt, FSEnd, FSProps, f};
    return ops;
}

static FakeStream HealthyStream(GT10InterfaceRole role) {
    FakeStream f;
    memset(&f, 0, sizeof f);
    f.setTakes[0] = f.setTakes[1] = true;
    f.endpoints                   = 1;
    f.pipe                        = GoodPipe(role);
    return f;
}

static void TestEnableStreamVia(void) {
    GT10PipeInfo pipe;
    GT10InterfaceRole in = GT10RoleAudioIn;

    // Happy path: one switch to alt 1, pipe index 1, stays on alt 1.
    FakeStream f      = HealthyStream(in);
    GT10StreamOps ops = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_OK);
    CHECK(f.nsets == 1 && f.sets[0] == 1 && f.alt == 1);
    CHECK(f.lastPipeRef == 1 && pipe.endpointNumber == 2);

    // The switch itself fails: still restore alt 0 and confirm it.
    f              = HealthyStream(in);
    f.setReturn[1] = GT10_ERR;
    ops            = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ERR);
    CHECK(f.nsets == 2 && f.sets[1] == 0 && f.propsCalls == 0);

    // The switch fails and so does the restore.
    f              = HealthyStream(in);
    f.setReturn[1] = GT10_ERR;
    f.setReturn[0] = GT10_ERR;
    ops            = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ALT_UNKNOWN);

    // The switch reports success but the setting did not change.
    f             = HealthyStream(in);
    f.setTakes[1] = false;
    ops           = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ERR);
    CHECK(f.propsCalls == 0 && f.nsets == 2 && f.sets[1] == 0);

    // The setting reads back 0 while the endpoint count still looks right: the
    // readback alone must reject it.
    f                 = HealthyStream(in);
    f.setTakes[1]     = false;
    f.endpointsAnyAlt = true;
    ops               = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ERR);
    CHECK(f.propsCalls == 0);

    // Reading the setting back fails: nothing can be confirmed.
    f           = HealthyStream(in);
    f.altReturn = GT10_ERR;
    ops         = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ALT_UNKNOWN);
    CHECK(f.propsCalls == 0);

    // Not exactly one endpoint on alt 1.
    f           = HealthyStream(in);
    f.endpoints = 2;
    ops         = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ERR);
    CHECK(f.propsCalls == 0 && f.alt == 0);
    f           = HealthyStream(in);
    f.endpoints = 0;
    ops         = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ERR);
    CHECK(f.propsCalls == 0 && f.alt == 0);

    // GetNumEndpoints fails.
    f                 = HealthyStream(in);
    f.endpointsReturn = GT10_ERR;
    ops               = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ERR);
    CHECK(f.propsCalls == 0 && f.alt == 0);

    // GetPipePropertiesV3 fails.
    f             = HealthyStream(in);
    f.propsReturn = GT10_ERR;
    ops           = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ERR);
    CHECK(f.alt == 0);

    // No bandwidth: rejected, restored, and the zero is reported for diagnosis.
    f                    = HealthyStream(in);
    f.pipe.maxPacketSize = 0;
    ops                  = StreamOpsFor(&f);
    pipe.maxPacketSize   = 99;
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ERR);
    CHECK(f.alt == 0 && pipe.maxPacketSize == 0);

    // A mismatch whose restore does not take.
    f               = HealthyStream(in);
    f.pipe.syncType = 2;
    f.setTakes[0]   = false;
    ops             = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ALT_UNKNOWN);

    // A mismatch whose restore call fails.
    f               = HealthyStream(in);
    f.pipe.syncType = 2;
    f.setReturn[0]  = GT10_ERR;
    ops             = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 1, &pipe) == GT10_ALT_UNKNOWN);

    // Missing arguments and a non-full-speed bus touch nothing.
    f   = HealthyStream(in);
    ops = StreamOpsFor(&f);
    CHECK(GT10EnableStreamVia(&ops, in, 2, &pipe) == GT10_ERR);
    CHECK(GT10EnableStreamVia(NULL, in, 1, &pipe) == GT10_ERR);
    CHECK(GT10EnableStreamVia(&ops, in, 1, NULL) == GT10_ERR);
    CHECK(GT10EnableStreamVia(&ops, GT10RoleNone, 1, &pipe) == GT10_ERR);
    CHECK(f.nsets == 0);
}

static void TestDisableStreamVia(void) {
    FakeStream f      = HealthyStream(GT10RoleAudioIn);
    f.alt             = 1;
    GT10StreamOps ops = StreamOpsFor(&f);
    CHECK(GT10DisableStreamVia(&ops) == GT10_OK);
    CHECK(f.alt == 0 && f.nsets == 1 && f.sets[0] == 0);

    f              = HealthyStream(GT10RoleAudioIn);
    f.alt          = 1;
    f.setReturn[0] = GT10_ERR;
    ops            = StreamOpsFor(&f);
    CHECK(GT10DisableStreamVia(&ops) == GT10_ALT_UNKNOWN);

    f             = HealthyStream(GT10RoleAudioIn);
    f.alt         = 1;
    f.setTakes[0] = false;
    ops           = StreamOpsFor(&f);
    CHECK(GT10DisableStreamVia(&ops) == GT10_ALT_UNKNOWN);

    f           = HealthyStream(GT10RoleAudioIn);
    f.alt       = 1;
    f.altReturn = GT10_ERR;
    ops         = StreamOpsFor(&f);
    CHECK(GT10DisableStreamVia(&ops) == GT10_ALT_UNKNOWN);

    CHECK(GT10DisableStreamVia(NULL) == GT10_ALT_UNKNOWN);
}

static void TestEnableStreamsVia(void) {
    GT10PipeInfo pipes[2];

    FakeStream o     = HealthyStream(GT10RoleAudioOut);
    FakeStream i     = HealthyStream(GT10RoleAudioIn);
    GT10StreamOps oo = StreamOpsFor(&o), io = StreamOpsFor(&i);
    CHECK(GT10EnableStreamsVia(&oo, &io, 1, pipes) == GT10_OK);
    CHECK(o.alt == 1 && i.alt == 1);
    CHECK(pipes[0].endpointNumber == 1 && pipes[1].endpointNumber == 2);

    // IF0 fails: IF1 is never touched.
    o                    = HealthyStream(GT10RoleAudioOut);
    o.pipe.maxPacketSize = 0;
    i                    = HealthyStream(GT10RoleAudioIn);
    oo                   = StreamOpsFor(&o);
    io                   = StreamOpsFor(&i);
    CHECK(GT10EnableStreamsVia(&oo, &io, 1, pipes) == GT10_ERR);
    CHECK(i.nsets == 0 && o.alt == 0);

    // IF1 fails: IF0 goes back to alt 0 as well.
    o                    = HealthyStream(GT10RoleAudioOut);
    i                    = HealthyStream(GT10RoleAudioIn);
    i.pipe.maxPacketSize = 0;
    oo                   = StreamOpsFor(&o);
    io                   = StreamOpsFor(&i);
    CHECK(GT10EnableStreamsVia(&oo, &io, 1, pipes) == GT10_ERR);
    CHECK(o.alt == 0 && i.alt == 0);

    // IF1 fails and IF0's restore does not confirm.
    o                    = HealthyStream(GT10RoleAudioOut);
    o.setTakes[0]        = false;
    i                    = HealthyStream(GT10RoleAudioIn);
    i.pipe.maxPacketSize = 0;
    oo                   = StreamOpsFor(&o);
    io                   = StreamOpsFor(&i);
    CHECK(GT10EnableStreamsVia(&oo, &io, 1, pipes) == GT10_ALT_UNKNOWN);

    // IF1 leaves its setting unknown: reported even though IF0 restored.
    o              = HealthyStream(GT10RoleAudioOut);
    i              = HealthyStream(GT10RoleAudioIn);
    i.setReturn[1] = GT10_ERR;
    i.setReturn[0] = GT10_ERR;
    oo             = StreamOpsFor(&o);
    io             = StreamOpsFor(&i);
    CHECK(GT10EnableStreamsVia(&oo, &io, 1, pipes) == GT10_ALT_UNKNOWN);
    CHECK(o.alt == 0);

    // A speed mismatch fails before either interface is switched.
    o  = HealthyStream(GT10RoleAudioOut);
    i  = HealthyStream(GT10RoleAudioIn);
    oo = StreamOpsFor(&o);
    io = StreamOpsFor(&i);
    CHECK(GT10EnableStreamsVia(&oo, &io, 2, pipes) == GT10_ERR);
    CHECK(o.nsets == 0 && i.nsets == 0);

    CHECK(GT10EnableStreamsVia(NULL, &io, 1, pipes) == GT10_ERR);
    CHECK(GT10EnableStreamsVia(&oo, NULL, 1, pipes) == GT10_ERR);
    CHECK(GT10EnableStreamsVia(&oo, &io, 1, NULL) == GT10_ERR);
}

// ---- checkpoint 4b ----

static void TestPlanRead(void) {
    GT10ReadPlan p;
    CHECK(GT10PlanRead(288, 8, &p));
    CHECK(p.reqPerFrame == 288 && p.numFrames == 8 && p.bufferBytes == 2304);
    CHECK(GT10PlanRead(288, 64, &p) && p.bufferBytes == 18432);
    CHECK(GT10PlanRead(288, 1, &p) && p.bufferBytes == 288);

    CHECK(!GT10PlanRead(0, 8, &p));
    CHECK(!GT10PlanRead(288, 0, &p));
    CHECK(!GT10PlanRead(288, 65, &p));
    CHECK(!GT10PlanRead(288, UINT32_MAX, &p));
    CHECK(!GT10PlanRead(288, 8, NULL));
}

static void TestFrameStart(void) {
    uint64_t s = 0;
    CHECK(GT10FrameStart(1000, 16, &s) && s == 1016);
    CHECK(GT10FrameStart(1000, 1, &s) && s == 1001);
    CHECK(GT10FrameStart(1000, 64, &s) && s == 1064);
    CHECK(!GT10FrameStart(1000, 0, &s));
    CHECK(!GT10FrameStart(1000, 65, &s));
    CHECK(GT10FrameStart(UINT64_MAX - 16, 16, &s) && s == UINT64_MAX);
    CHECK(!GT10FrameStart(UINT64_MAX - 15, 16, &s));
    CHECK(!GT10FrameStart(1000, 16, NULL));
}

typedef struct {
    int submitReturn;
    bool waitResult[2];
    int abortReturn;
    // recorded
    int submits, waits, aborts;
    unsigned waitMs[2];
} FakeRead;

static int FRSubmit(void *c) {
    FakeRead *f = c;
    f->submits++;
    return f->submitReturn;
}
static bool FRWait(void *c, unsigned ms) {
    FakeRead *f = c;
    const int n = f->waits++;
    if (n < 2) f->waitMs[n] = ms;
    return n < 2 && f->waitResult[n];
}
static int FRAbort(void *c) {
    FakeRead *f = c;
    f->aborts++;
    return f->abortReturn;
}

static void TestReadOnceVia(void) {
    FakeRead f;
    GT10ReadOps ops = {FRSubmit, FRWait, FRAbort, &f};

    // Refused submission: nothing outstanding, so no wait and no abort.
    memset(&f, 0, sizeof f);
    f.submitReturn = GT10_ERR;
    CHECK(GT10ReadOnceVia(&ops, 1000, 500) == GT10ReadRejected);
    CHECK(f.submits == 1 && f.waits == 0 && f.aborts == 0);

    // Completes in time: no abort.
    memset(&f, 0, sizeof f);
    f.waitResult[0] = true;
    CHECK(GT10ReadOnceVia(&ops, 1000, 500) == GT10ReadCompleted);
    CHECK(f.waits == 1 && f.waitMs[0] == 1000 && f.aborts == 0);

    // Deadline passes: abort, then wait again for the completion.
    memset(&f, 0, sizeof f);
    f.waitResult[1] = true;
    CHECK(GT10ReadOnceVia(&ops, 1000, 500) == GT10ReadAborted);
    CHECK(f.aborts == 1 && f.waits == 2 && f.waitMs[1] == 500);

    // An abort error does not prove nothing is outstanding: still wait.
    memset(&f, 0, sizeof f);
    f.abortReturn   = GT10_ERR;
    f.waitResult[1] = true;
    CHECK(GT10ReadOnceVia(&ops, 1000, 500) == GT10ReadAborted);
    CHECK(f.waits == 2);

    // No completion even after abort.
    memset(&f, 0, sizeof f);
    CHECK(GT10ReadOnceVia(&ops, 1000, 500) == GT10ReadAbandoned);
    CHECK(f.aborts == 1 && f.waits == 2);

    CHECK(GT10ReadOnceVia(NULL, 1000, 500) == GT10ReadRejected);
}

enum { kUnderrun = (int32_t)0xE00002E7, kOverrun = (int32_t)0xE00002E8 };

static void TestFrameAcceptable(void) {
    CHECK(GT10FrameAcceptable(0, 288, 264, 6));
    CHECK(GT10FrameAcceptable(0, 288, 270, 6));
    CHECK(GT10FrameAcceptable(0, 288, 288, 6));
    CHECK(GT10FrameAcceptable(kUnderrun, 288, 264, 6));
    CHECK(GT10FrameAcceptable(0, 288, 0, 6));

    CHECK(!GT10FrameAcceptable(kOverrun, 288, 288, 6));
    CHECK(
        !GT10FrameAcceptable((int32_t)0xE00002D8, 288, 0, 6));  // kIOReturnNotReady, never written
    CHECK(!GT10FrameAcceptable(0, 288, 289, 6));
    CHECK(!GT10FrameAcceptable(0, 264, 270, 6));  // whole frames, but more than requested
    CHECK(!GT10FrameAcceptable(0, 288, 265, 6));
    CHECK(!GT10FrameAcceptable(0, 288, 264, 0));
}

static GT10ReadReport PassingReport(void) {
    GT10ReadReport r;
    memset(&r, 0, sizeof r);
    r.outcome   = GT10ReadCompleted;
    r.numFrames = 8;
    for (int i = 0; i < 8; i++) {
        r.frameReq[i] = 288;
        r.frameAct[i] = i == 7 ? 270 : 264;
    }
    return r;
}

static void TestReadAccepted(void) {
    GT10ReadReport r = PassingReport();
    CHECK(GT10ReadAccepted(&r, 6));
    r.completionReturn = kUnderrun;
    CHECK(GT10ReadAccepted(&r, 6));

    r         = PassingReport();
    r.outcome = GT10ReadAborted;
    CHECK(!GT10ReadAccepted(&r, 6));
    r         = PassingReport();
    r.outcome = GT10ReadRejected;
    CHECK(!GT10ReadAccepted(&r, 6));
    r                  = PassingReport();
    r.completionReturn = kOverrun;
    CHECK(!GT10ReadAccepted(&r, 6));
    r                = PassingReport();
    r.frameStatus[7] = kOverrun;
    CHECK(!GT10ReadAccepted(&r, 6));
    r             = PassingReport();
    r.frameAct[3] = 289;
    CHECK(!GT10ReadAccepted(&r, 6));
    r           = PassingReport();
    r.numFrames = 0;
    CHECK(!GT10ReadAccepted(&r, 6));
    r           = PassingReport();
    r.numFrames = kGT10MaxReadFrames + 1;
    CHECK(!GT10ReadAccepted(&r, 6));
    r = PassingReport();
    for (int i = 0; i < 8; i++)
        r.frameAct[i] = 0;
    CHECK(!GT10ReadAccepted(&r, 6));  // nothing received
    CHECK(!GT10ReadAccepted(NULL, 6));
}

int main(void) {
    TestFrameAcceptable();
    TestReadAccepted();
    TestConfigPlan();
    TestRetryPlan();
    TestInterfaceRole();
    TestConfigureVia();
    TestOpenInterfaceVia();
    TestPipeFitsRole();
    TestEnableStreamVia();
    TestDisableStreamVia();
    TestEnableStreamsVia();
    TestPlanRead();
    TestFrameStart();
    TestReadOnceVia();
    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
