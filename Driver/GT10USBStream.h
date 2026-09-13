// Continuous isochronous streaming for the GT-10. One engine thread owns both
// audio interfaces, their run loop sources, and every transfer.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "GT10USBDevice.h"
#include "../Sources/GT10Clock.h"
#include "../Sources/GT10Ring.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fills dst with frames of 24-bit stereo output starting at sample position
// position. Runs on the engine thread.
typedef void (*GT10RenderOutput)(void *ctx, int64_t position, uint8_t *dst, uint32_t frames);

typedef struct {
    bool capture;           // stream IF1
    GT10Ring *captureRing;  // receives capture at its sample position, may be NULL
    // NULL leaves IF0 idle. With capture on, playback starts after the first
    // capture completes and follows the capture timeline, so render sees the
    // same sample positions the HAL writes output at.
    GT10RenderOutput render;
    void *renderContext;
    uint32_t playbackTransfers;  // stop submitting after this many, 0 for no limit
    GT10TimeStamp *timeStamp;    // receives zero timestamps from capture, may be NULL

    // Called once on the engine thread when a fault stops streaming, never
    // for a requested stop. May be NULL.
    void (*faulted)(void *ctx, uint64_t token);
    void *faultContext;
    uint64_t faultToken;
} GT10StreamConfig;

typedef struct {
    bool running;        // false once stopped or faulted
    bool abandoned;      // callbacks never arrived, the device must not be reused
    bool realtime;       // the time constraint policy was accepted
    int32_t firstError;  // raw IOReturn that faulted the engine, 0 when none

    uint64_t captureTransfers;
    uint64_t capturedFrames;   // received
    uint64_t estimatedFrames;  // silence standing in for lost packets
    uint64_t lostPackets;
    uint64_t firstBusFrame;     // of the first processed capture transfer
    uint64_t lastBusFrame;      // of the last processed capture frame
    uint64_t maxLatencyFrames;  // completion arrival after its last frame

    int64_t capturePosition;   // one past the newest sample written to the capture ring
    uint64_t lostFrameStatus;  // count of frames with a transient error status

    uint64_t playbackTransfers;
    uint64_t playedFrames;
    uint64_t playbackFrameErrors;
    // The last completed playback transfer. Filled only by GT10StreamStop.
    int32_t lastPlaybackStatus[4];
    uint16_t lastPlaybackReq[4];
    uint16_t lastPlaybackAct[4];

    uint64_t outOfOrder;
    uint64_t lateSubmits;
    uint64_t playbackRealigns;  // jumps to the capture timeline
    uint64_t clockRejected;     // bus frame times too far from mach_absolute_time
} GT10StreamStats;

// Needs checkpoint 4a: both interfaces open and on alt 1. Returns once the
// first transfers are submitted, or NULL when the engine could not start. A
// fault during start leaves stats.running false. Stop must still be called.
struct GT10Stream;
typedef struct GT10Stream GT10Stream;
// outAbandoned, when given, reports an engine that started but did not settle.
// Its transfers can still reach the kernel, so the caller must not touch the
// interfaces again and must free nothing the engine holds.
GT10Stream *GT10StreamStart(GT10USBDevice *dev, const GT10StreamConfig *config, bool *outAbandoned);

// Aborts, waits for every callback, fills finalStats after the engine thread
// has exited, frees, and clears *stream. GT10_ERR means the callbacks never
// arrived, or the engine thread did not finish in time: the engine is left
// allocated and the device must stay open and unused until the process exits.
// The wait is bounded, so a stuck kernel call cannot hang the caller. Never call
// it on the engine thread or a real-time HAL callback.
int GT10StreamStop(GT10Stream **stream, GT10StreamStats *finalStats);

// A live snapshot for monitoring, without the last playback transfer.
void GT10StreamGetStats(const GT10Stream *stream, GT10StreamStats *out);

// One past the newest sample written to the capture ring. A reader that stops
// at the ring's end after a stall would never raise its floor again, so it
// reads up to this position instead.
int64_t GT10StreamCapturePosition(const GT10Stream *stream);

// The stream's owner. Every callback runs on the monitor's queue, so none of
// them races a start, a stop, a fault, or a removal.
typedef struct {
    // Called just before a start, once no engine runs and none was ever
    // abandoned, so buffers the engine shares can be reset. Returning false
    // cancels the start.
    bool (*prepare)(void *ctx);
    // running true comes once the first zero timestamp is published. Returning
    // false refuses the stream, which the monitor then stops. running false
    // comes once a started stream has ended for any reason.
    bool (*stateChanged)(void *ctx, bool running);
    void *context;
} GT10StreamOwner;

// Stream while the monitor holds the device. config NULL stops. Runs on the
// monitor's queue and blocks until done, including the wait for the first
// timestamp. GT10_OK means a stream is running on return, or for a stop, that
// both interfaces are back on alt 0. The monitor overrides the config's fault
// callback. A removal stops the stream, and the request survives it: a later
// arrival starts a new stream until config NULL clears the request. An engine
// that could not be joined quarantines the device and the monitor for the rest
// of the process: no restart, and no free.
int GT10USBMonitorSetStream(GT10USBMonitor *monitor, const GT10StreamConfig *config,
                            const GT10StreamOwner *owner);

#ifdef __cplusplus
}
#endif
