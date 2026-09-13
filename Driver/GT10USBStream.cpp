#include "GT10USBStream.h"

#include "GT10StreamCore.h"
#include "GT10USBInternal.h"
#include "../Sources/USBAudioFormat.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>

#include <atomic>
#include <cstring>
#include <new>

namespace {

constexpr uint32_t kTransfers         = 4;
constexpr uint32_t kFramesPerTransfer = 4;
constexpr uint32_t kLeadFrames        = 16;
// A request must start at least this many bus frames after the current one.
constexpr uint64_t kMinHeadroomFrames = 2;
constexpr uint32_t kSampleRate        = 44100;
constexpr uint32_t kMaxPacketFrames   = kGT10PipeBytes / kGT10BytesPerFrame;
constexpr int32_t kReturnUnderrun     = (int32_t)0xE00002E7;
constexpr double kStopDeadlineSeconds = 1.0;
constexpr double kIdleRunSeconds      = 0.25;
// A bus frame time further than this from the read time is rejected.
constexpr double kClockTolerance = 0.1;
// Consecutive rejected observations before the clock is declared lost, about a
// second of capture completions.
constexpr uint32_t kClockOutageLimit = 250;
// Stop waits for the engine thread this much beyond its own abort deadline.
constexpr double kStopJoinGraceSeconds = 2.0;

// How long a start waits for the engine to finish its first USB calls. Those
// calls are synchronous, so a stuck kernel would otherwise hold the caller
// forever, and the caller holds the state lock of the plug-in.
constexpr double kStartDeadlineSeconds = 2.0;

struct Transfer {
    GT10Stream *engine;
    bool outstanding;
    uint64_t seq;
    uint64_t startFrame;
    uint8_t buffer[kFramesPerTransfer * kGT10PipeBytes];
    IOUSBIsocFrame frames[kFramesPerTransfer];
};

uint64_t AbsFromSeconds(double seconds) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (uint64_t)(seconds * 1e9 * tb.denom / tb.numer);
}

}  // namespace

struct GT10Stream {
    IOUSBInterfaceInterface650 **in  = nullptr;
    IOUSBInterfaceInterface650 **out = nullptr;
    GT10StreamConfig config          = {};
    pthread_t thread                 = nullptr;
    CFRunLoopRef runLoop             = nullptr;
    CFRunLoopSourceRef inSource      = nullptr;
    CFRunLoopSourceRef outSource     = nullptr;
    dispatch_semaphore_t ready       = nullptr;
    dispatch_semaphore_t finished    = nullptr;

    Transfer capture[kTransfers]  = {};
    Transfer playback[kTransfers] = {};

    // Engine thread only.
    uint32_t outstanding      = 0;
    bool stopRequested        = false;
    bool faulted              = false;
    uint64_t stopDeadline     = 0;
    uint64_t captureNextStart = 0, captureNextSeq = 0, captureExpectSeq = 0;
    uint64_t playbackNextStart = 0, playbackNextSeq = 0, playbackExpectSeq = 0;
    uint32_t playbackSubmitted = 0;
    int64_t capturePos         = 0;
    int64_t playbackPos        = 0;
    uint64_t lastCaptureFrame  = 0;  // last bus frame of the newest processed capture
    uint64_t sinkTransferStart = 0;  // bus frame of the transfer being extracted
    bool playbackStarted       = false;
    uint32_t clockOutage       = 0;
    USBAudioPacer lossPacer    = {};
    USBAudioPacer playPacer    = {};
    GT10Clock clock            = {};

    // Written on the engine thread, read from any thread.
    std::atomic<bool> running{false}, abandoned{false}, realtime{false};
    std::atomic<int32_t> firstError{0};
    std::atomic<uint64_t> captureTransfers{0}, capturedFrames{0}, estimatedFrames{0},
        lostPackets{0};
    std::atomic<uint64_t> firstBusFrame{0}, lastBusFrame{0}, maxLatencyFrames{0};
    std::atomic<uint64_t> playbackTransfers{0}, playedFrames{0}, playbackFrameErrors{0};
    std::atomic<uint64_t> outOfOrder{0}, lateSubmits{0}, lostFrameStatus{0}, playbackRealigns{0};
    std::atomic<uint64_t> clockRejected{0};
    std::atomic<int64_t> captureEnd{0};
    // Engine thread only while it runs. Stop reads it after the join.
    IOUSBIsocFrame lastPlayback[kFramesPerTransfer] = {};
};

namespace {

void Fault(GT10Stream *s, IOReturn kr) {
    if (s->faulted) return;
    s->faulted    = true;
    s->firstError = kr;
    s->running    = false;
    // Notify first: an abort that hangs in the kernel must not keep the owner
    // from handing the clock back.
    if (!s->stopRequested && s->config.faulted != nullptr)
        s->config.faulted(s->config.faultContext, s->config.faultToken);
    // Every accepted request still gets its callback, so outstanding drains.
    (*s->in)->AbortPipe(s->in, kGT10PipeRef);
    (*s->out)->AbortPipe(s->out, kGT10PipeRef);
}

bool CurrentBusFrame(IOUSBInterfaceInterface650 **intf, uint64_t *out) {
    UInt64 frame = 0;
    AbsoluteTime at;
    if ((*intf)->GetBusFrameNumber(intf, &frame, &at) != kIOReturnSuccess) return false;
    *out = frame;
    return true;
}

// Feeds the clock a bus frame with the host time at its start. The header
// calls that time wall time, so a value that is not a recent
// mach_absolute_time is rejected, never replaced. Returns false only when the
// bus frame itself is unavailable.
bool ObserveBusFrame(GT10Stream *s, uint64_t *out) {
    UInt64 frame = 0;
    AbsoluteTime at;
    if ((*s->in)->GetBusFrameNumberWithTime(s->in, &frame, &at) != kIOReturnSuccess) return false;
    const uint64_t now  = mach_absolute_time();
    const uint64_t host = ((uint64_t)at.hi << 32) | at.lo;
    *out                = frame;
    if (host > now || now - host > AbsFromSeconds(kClockTolerance)) {
        s->clockRejected++;
        s->clockOutage++;
        return true;
    }
    s->clockOutage = 0;
    GT10ClockObserve(&s->clock, frame, host);
    return true;
}

void CaptureCompleted(void *refcon, IOReturn result, void *arg0);
void PlaybackCompleted(void *refcon, IOReturn result, void *arg0);

// Starting at the next contiguous frame, never at one already too close.
bool ClaimStart(GT10Stream *s, IOUSBInterfaceInterface650 **intf, uint64_t next, uint64_t *out) {
    uint64_t now = 0;
    if (!CurrentBusFrame(intf, &now)) {
        Fault(s, kIOReturnNoDevice);
        return false;
    }
    if (next < now + kMinHeadroomFrames) {
        s->lateSubmits++;
        Fault(s, kIOReturnIsoTooOld);
        return false;
    }
    *out = next;
    return true;
}

bool SubmitCapture(GT10Stream *s, Transfer *t) {
    uint64_t start = 0;
    if (!ClaimStart(s, s->in, s->captureNextStart, &start)) return false;
    for (uint32_t i = 0; i < kFramesPerTransfer; i++) {
        t->frames[i].frStatus   = kIOReturnNotReady;
        t->frames[i].frReqCount = kGT10PipeBytes;
        t->frames[i].frActCount = 0;
    }
    const IOReturn kr = (*s->in)->ReadIsochPipeAsync(
        s->in, kGT10PipeRef, t->buffer, start, kFramesPerTransfer, t->frames, CaptureCompleted, t);
    if (kr != kIOReturnSuccess) {
        Fault(s, kr);
        return false;
    }
    t->outstanding = true;
    t->startFrame  = start;
    t->seq         = s->captureNextSeq++;
    s->outstanding++;
    s->captureNextStart = start + kFramesPerTransfer;
    return true;
}

bool SubmitPlayback(GT10Stream *s, Transfer *t) {
    if (s->config.playbackTransfers != 0 && s->playbackSubmitted >= s->config.playbackTransfers)
        return false;
    uint64_t start = 0;
    if (!ClaimStart(s, s->out, s->playbackNextStart, &start)) return false;

    // Packed: each frame's bytes follow the previous frame's request.
    uint32_t offset = 0;
    for (uint32_t i = 0; i < kFramesPerTransfer; i++) {
        uint32_t n = USBAudioPacerNextFrames(&s->playPacer);
        if (s->config.capture) {
            const int64_t target =
                GT10PredictPosition(s->capturePos, s->lastCaptureFrame, start + i);
            const int64_t distance =
                target > s->playbackPos ? target - s->playbackPos : s->playbackPos - target;
            if (distance > kGT10RealignFrames) {
                s->playbackPos = target;
                s->playbackRealigns++;
            }
            n = GT10PlaybackFrames(n, s->playbackPos, target);
        }
        if (n > kMaxPacketFrames) n = kMaxPacketFrames;
        s->config.render(s->config.renderContext, s->playbackPos, t->buffer + offset, n);
        s->playbackPos += n;
        t->frames[i].frStatus   = kIOReturnNotReady;
        t->frames[i].frReqCount = (UInt16)(n * kGT10BytesPerFrame);
        t->frames[i].frActCount = 0;
        offset += n * kGT10BytesPerFrame;
    }
    const IOReturn kr =
        (*s->out)->WriteIsochPipeAsync(s->out, kGT10PipeRef, t->buffer, start, kFramesPerTransfer,
                                       t->frames, PlaybackCompleted, t);
    if (kr != kIOReturnSuccess) {
        Fault(s, kr);
        return false;
    }
    t->outstanding = true;
    t->startFrame  = start;
    t->seq         = s->playbackNextSeq++;
    s->outstanding++;
    s->playbackSubmitted++;
    s->playbackNextStart = start + kFramesPerTransfer;
    return true;
}

void CaptureSink(void *ctx, uint32_t index, const uint8_t *bytes, uint32_t frames) {
    static const uint8_t kSilence[kMaxPacketFrames * kGT10BytesPerFrame] = {};
    GT10Stream *s                                                        = (GT10Stream *)ctx;
    if (bytes == nullptr) {
        frames = USBAudioPacerNextFrames(&s->lossPacer);
        bytes  = kSilence;
        s->estimatedFrames += frames;
    } else {
        s->capturedFrames += frames;
    }
    if (s->config.captureRing != nullptr)
        GT10RingWrite(s->config.captureRing, s->capturePos, bytes, frames);
    s->capturePos += frames;
    s->captureEnd = s->capturePos;

    // Only with a sink, which requires an anchored clock from the start.
    int64_t zeroSample = 0;
    uint64_t zeroHost  = 0;
    if (s->config.timeStamp != nullptr &&
        GT10ClockPacket(&s->clock, s->sinkTransferStart + index, frames, &zeroSample, &zeroHost)) {
        GT10TimeStampPublish(s->config.timeStamp, zeroSample, zeroHost);
    }
}

void StartPlayback(GT10Stream *s, uint64_t now) {
    s->playbackStarted   = true;
    s->playbackNextStart = now + kLeadFrames;
    s->playbackPos = s->config.capture ? GT10PredictPosition(s->capturePos, s->lastCaptureFrame,
                                                             s->playbackNextStart)
                                       : 0;
    for (uint32_t k = 0; k < kTransfers && !s->faulted; k++)
        SubmitPlayback(s, &s->playback[k]);
}

// The engine's own view of a completed frame list, copied field by field. The
// SDK type and GT10IsocFrame match in layout but are unrelated types.
void CopyFrames(const IOUSBIsocFrame *src, GT10IsocFrame *dst) {
    for (uint32_t i = 0; i < kFramesPerTransfer; i++) {
        dst[i].status   = src[i].frStatus;
        dst[i].reqCount = src[i].frReqCount;
        dst[i].actCount = src[i].frActCount;
    }
}

// Shared checks for a completion. False means the transfer is not processed.
bool Accept(GT10Stream *s, Transfer *t, uint64_t *expectSeq) {
    if (!t->outstanding || s->outstanding == 0) {
        // A callback for a request the engine never counted. Nothing it points
        // to can be trusted, so stop without touching the bookkeeping.
        Fault(s, kIOReturnInternalError);
        return false;
    }
    t->outstanding = false;
    s->outstanding--;
    if (s->faulted || s->stopRequested) return false;
    if (t->seq != *expectSeq) {
        s->outOfOrder++;
        Fault(s, kIOReturnError);
        return false;
    }
    (*expectSeq)++;
    return true;
}

void CaptureCompleted(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    Transfer *t   = (Transfer *)refcon;
    GT10Stream *s = t->engine;
    if (!Accept(s, t, &s->captureExpectSeq)) return;
    if (result != kIOReturnSuccess && result != kReturnUnderrun) {
        Fault(s, result);
        return;
    }

    GT10IsocFrame frames[kFramesPerTransfer];
    CopyFrames(t->frames, frames);
    int32_t status = 0;
    if (GT10WorstFrameStatus(frames, kFramesPerTransfer, &status) == GT10FrameTerminal) {
        Fault(s, status);
        return;
    }
    for (uint32_t i = 0; i < kFramesPerTransfer; i++) {
        if (GT10ClassifyFrameStatus(frames[i].status) == GT10FrameLost) s->lostFrameStatus++;
    }

    uint64_t now             = 0;
    const uint64_t lastFrame = t->startFrame + kFramesPerTransfer - 1;
    if (!ObserveBusFrame(s, &now)) {
        Fault(s, kIOReturnNoDevice);
        return;
    }
    if (s->clockOutage >= kClockOutageLimit) {
        Fault(s, kIOReturnTimeout);
        return;
    }
    if (now > lastFrame && now - lastFrame > s->maxLatencyFrames)
        s->maxLatencyFrames = now - lastFrame;

    s->sinkTransferStart = t->startFrame;
    const GT10CaptureResult r =
        GT10ExtractCapture(t->buffer, sizeof t->buffer, frames, kFramesPerTransfer, CaptureSink, s);
    s->lostPackets += r.lostPackets;
    if (s->captureTransfers == 0) s->firstBusFrame = t->startFrame;
    s->lastBusFrame     = lastFrame;
    s->lastCaptureFrame = lastFrame;
    s->captureTransfers++;

    if (SubmitCapture(s, t) && s->config.render != nullptr && !s->playbackStarted)
        StartPlayback(s, now);
}

void PlaybackCompleted(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    Transfer *t   = (Transfer *)refcon;
    GT10Stream *s = t->engine;
    if (!Accept(s, t, &s->playbackExpectSeq)) return;

    memcpy(s->lastPlayback, t->frames, sizeof s->lastPlayback);
    GT10IsocFrame frames[kFramesPerTransfer];
    CopyFrames(t->frames, frames);
    for (uint32_t i = 0; i < kFramesPerTransfer; i++) {
        if (frames[i].status != kIOReturnSuccess) s->playbackFrameErrors++;
        s->playedFrames += frames[i].actCount / kGT10BytesPerFrame;
    }
    s->playbackTransfers++;
    if (result != kIOReturnSuccess) {
        Fault(s, result);
        return;
    }
    int32_t status = 0;
    if (GT10WorstFrameStatus(frames, kFramesPerTransfer, &status) == GT10FrameTerminal) {
        Fault(s, status);
        return;
    }
    SubmitPlayback(s, t);
}

void SetRealtime(GT10Stream *s) {
    thread_time_constraint_policy_data_t p;
    p.period      = (uint32_t)AbsFromSeconds(kFramesPerTransfer / 1000.0);
    p.computation = (uint32_t)AbsFromSeconds(0.001);
    p.constraint  = (uint32_t)AbsFromSeconds(kFramesPerTransfer / 1000.0);
    p.preemptible = TRUE;
    s->realtime =
        thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                          (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

void *EngineMain(void *arg) {
    GT10Stream *s = (GT10Stream *)arg;
    pthread_setname_np("GT10 USB stream");
    SetRealtime(s);

    s->runLoop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
    CFRunLoopAddSource(s->runLoop, s->inSource, kCFRunLoopDefaultMode);
    CFRunLoopAddSource(s->runLoop, s->outSource, kCFRunLoopDefaultMode);
    s->running = true;

    uint64_t now = 0;
    if (!ObserveBusFrame(s, &now)) {
        Fault(s, kIOReturnNoDevice);
    } else if (s->config.timeStamp != nullptr && !s->clock.anchored) {
        // Timestamps need a trusted anchor from the start.
        Fault(s, kIOReturnTimeout);
    } else if (s->config.capture) {
        // Playback waits for the first capture, which fixes the timeline.
        s->captureNextStart = now + kLeadFrames;
        for (uint32_t k = 0; k < kTransfers && !s->faulted; k++)
            SubmitCapture(s, &s->capture[k]);
    } else {
        StartPlayback(s, now);
    }
    dispatch_semaphore_signal(s->ready);

    for (;;) {
        if (s->stopRequested) {
            if (s->outstanding == 0) break;
            if (mach_absolute_time() >= s->stopDeadline) {
                s->abandoned = true;
                break;
            }
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, kIdleRunSeconds, true);
    }

    if (!s->abandoned) {
        CFRunLoopRemoveSource(s->runLoop, s->inSource, kCFRunLoopDefaultMode);
        CFRunLoopRemoveSource(s->runLoop, s->outSource, kCFRunLoopDefaultMode);
    }
    s->running = false;
    dispatch_semaphore_signal(s->finished);
    return nullptr;
}

// Only ever called when the engine thread is joined or was never created. An
// abandoned engine keeps everything here, because the kernel may still write it.
void Destroy(GT10Stream *s) {
    if (s->inSource != nullptr) CFRelease(s->inSource);
    if (s->outSource != nullptr) CFRelease(s->outSource);
    if (s->runLoop != nullptr) CFRelease(s->runLoop);
    if (s->ready != nullptr) dispatch_release(s->ready);
    if (s->finished != nullptr) dispatch_release(s->finished);
    delete s;
}

}  // namespace

GT10Stream *GT10StreamStart(GT10USBDevice *dev, const GT10StreamConfig *config,
                            bool *outAbandoned) {
    if (outAbandoned != nullptr) *outAbandoned = false;
    if (config == nullptr || (!config->capture && config->render == nullptr)) return nullptr;
    IOUSBInterfaceInterface650 **in  = GT10USBDeviceInterface(dev, GT10RoleAudioIn);
    IOUSBInterfaceInterface650 **out = GT10USBDeviceInterface(dev, GT10RoleAudioOut);
    if (in == nullptr || out == nullptr) return nullptr;

    GT10Stream *s = new (std::nothrow) GT10Stream();
    if (s == nullptr) return nullptr;
    s->in     = in;
    s->out    = out;
    s->config = *config;
    for (uint32_t k = 0; k < kTransfers; k++) {
        s->capture[k].engine  = s;
        s->playback[k].engine = s;
    }
    USBAudioPacerInit(&s->lossPacer, kSampleRate, 1000);
    USBAudioPacerInit(&s->playPacer, kSampleRate, 1000);
    GT10ClockInit(&s->clock, (double)AbsFromSeconds(0.001), kGT10ZeroTimeStampPeriod);

    // Create retains each source for the caller.
    if ((*in)->CreateInterfaceAsyncEventSource(in, &s->inSource) != kIOReturnSuccess ||
        (*out)->CreateInterfaceAsyncEventSource(out, &s->outSource) != kIOReturnSuccess) {
        Destroy(s);
        return nullptr;
    }
    s->ready    = dispatch_semaphore_create(0);
    s->finished = dispatch_semaphore_create(0);
    if (pthread_create(&s->thread, nullptr, EngineMain, s) != 0) {
        Destroy(s);
        return nullptr;
    }
    if (dispatch_semaphore_wait(
            s->ready, dispatch_time(DISPATCH_TIME_NOW,
                                    (int64_t)(kStartDeadlineSeconds * NSEC_PER_SEC))) != 0) {
        // The engine is stuck in a kernel call. Leave it, and everything it
        // references, allocated. The kernel can still write the transfers.
        if (outAbandoned != nullptr) *outAbandoned = true;
        return nullptr;
    }
    return s;
}

int GT10StreamStop(GT10Stream **stream, GT10StreamStats *finalStats) {
    if (finalStats != nullptr) memset(finalStats, 0, sizeof *finalStats);
    if (stream == nullptr || *stream == nullptr) return GT10_OK;
    GT10Stream *s = *stream;
    *stream       = nullptr;
    CFRunLoopPerformBlock(s->runLoop, kCFRunLoopDefaultMode, ^{
        if (s->stopRequested) return;
        s->stopRequested = true;
        s->stopDeadline  = mach_absolute_time() + AbsFromSeconds(kStopDeadlineSeconds);
        s->running       = false;
        (*s->in)->AbortPipe(s->in, kGT10PipeRef);
        (*s->out)->AbortPipe(s->out, kGT10PipeRef);
    });
    CFRunLoopWakeUp(s->runLoop);
    const double limit = kStopDeadlineSeconds + kIdleRunSeconds + kStopJoinGraceSeconds;
    if (dispatch_semaphore_wait(
            s->finished, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(limit * NSEC_PER_SEC))) != 0) {
        // The engine thread is stuck, likely in a kernel call. Leave it and
        // everything it references allocated.
        s->abandoned = true;
        if (finalStats != nullptr) GT10StreamGetStats(s, finalStats);
        return GT10_ERR;
    }
    pthread_join(s->thread, nullptr);
    if (finalStats != nullptr) {
        GT10StreamGetStats(s, finalStats);
        for (uint32_t i = 0; i < kFramesPerTransfer; i++) {
            finalStats->lastPlaybackStatus[i] = s->lastPlayback[i].frStatus;
            finalStats->lastPlaybackReq[i]    = s->lastPlayback[i].frReqCount;
            finalStats->lastPlaybackAct[i]    = s->lastPlayback[i].frActCount;
        }
    }
    if (s->abandoned) return GT10_ERR;  // the kernel may still write the transfers
    Destroy(s);
    return GT10_OK;
}

int64_t GT10StreamCapturePosition(const GT10Stream *s) {
    return s != nullptr ? s->captureEnd.load() : 0;
}

void GT10StreamGetStats(const GT10Stream *s, GT10StreamStats *out) {
    if (out == nullptr) return;
    memset(out, 0, sizeof *out);
    if (s == nullptr) return;
    out->running             = s->running;
    out->abandoned           = s->abandoned;
    out->realtime            = s->realtime;
    out->firstError          = s->firstError;
    out->captureTransfers    = s->captureTransfers;
    out->capturedFrames      = s->capturedFrames;
    out->estimatedFrames     = s->estimatedFrames;
    out->lostPackets         = s->lostPackets;
    out->firstBusFrame       = s->firstBusFrame;
    out->lastBusFrame        = s->lastBusFrame;
    out->maxLatencyFrames    = s->maxLatencyFrames;
    out->capturePosition     = s->captureEnd;
    out->lostFrameStatus     = s->lostFrameStatus;
    out->playbackTransfers   = s->playbackTransfers;
    out->playedFrames        = s->playedFrames;
    out->playbackFrameErrors = s->playbackFrameErrors;
    out->outOfOrder          = s->outOfOrder;
    out->lateSubmits         = s->lateSubmits;
    out->playbackRealigns    = s->playbackRealigns;
    out->clockRejected       = s->clockRejected;
}
