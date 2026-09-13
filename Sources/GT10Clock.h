// The device clock seen by the HAL: sample times from captured audio, host
// times from USB bus frames.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Bus frame to host time, and zero timestamps from captured packets ----

typedef struct {
    // Host ticks per bus frame before enough observations exist.
    double nominalTicksPerFrame;
    uint32_t period;  // kAudioDevicePropertyZeroTimeStampPeriod

    bool anchored;
    uint64_t anchorFrame, anchorHost;
    uint64_t latestFrame, latestHost;

    int64_t samples;   // captured sample frames accounted so far
    int64_t nextZero;  // sample time of the next zero timestamp
} GT10Clock;

void GT10ClockInit(GT10Clock *clock, double nominalTicksPerFrame, uint32_t period);

// Record one GetBusFrameNumberWithTime observation. The first one anchors the
// mapping. Later ones only lengthen the baseline the rate is measured over, so
// the header's 200 us jitter shrinks with time instead of moving timestamps.
void GT10ClockObserve(GT10Clock *clock, uint64_t frame, uint64_t hostTime);

// Sample frames between zero timestamps. The HAL asks for this as
// kAudioDevicePropertyZeroTimeStampPeriod, and the engine publishes on the same
// boundary, so both sides read it from here.
enum { kGT10ZeroTimeStampPeriod = 16384 };

// Measured over the baseline once it spans kGT10ClockMinBaseline frames.
enum { kGT10ClockMinBaseline = 1000 };
double GT10ClockTicksPerFrame(const GT10Clock *clock);
uint64_t GT10ClockHostOfFrame(const GT10Clock *clock, uint64_t frame);

// Account the packet received in bus frame frame. When it contains the sample
// at the next period boundary, returns true with that zero timestamp, placed
// inside the frame in proportion to the sample's offset. Sample 0 is the first
// boundary, so the first audio publishes a timestamp at once.
bool GT10ClockPacket(GT10Clock *clock, uint64_t frame, uint32_t frames, int64_t *zeroSample,
                     uint64_t *zeroHost);

// ---- Publication to the HAL's real-time thread ----

typedef struct GT10TimeStamp GT10TimeStamp;

GT10TimeStamp *GT10TimeStampCreate(void);
void GT10TimeStampDestroy(GT10TimeStamp *ts);

// One writer thread.
void GT10TimeStampPublish(GT10TimeStamp *ts, int64_t sample, uint64_t hostTime);
void GT10TimeStampClear(GT10TimeStamp *ts);

// Any thread, never blocks. Returns the newest published timestamp, or the one
// before it while a publication is in progress, so a read never fails once
// something is published. False only when nothing is published since the last
// clear.
bool GT10TimeStampRead(const GT10TimeStamp *ts, int64_t *sample, uint64_t *hostTime);

#ifdef __cplusplus
}
#endif
