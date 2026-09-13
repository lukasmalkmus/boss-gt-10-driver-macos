// Pure logic of the GT-10 stream engine, free of IOKit so it is unit testable.
// The live engine in GT10USBStream.cpp drives it from the USB thread.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    kGT10BytesPerFrame     = 6,  // 2 channels, 24 bit
    kGT10MaxTransferFrames = 64,
};

// The fields of IOUSBIsocFrame, without IOKit. The engine copies into it.
typedef struct {
    int32_t status;
    uint16_t reqCount;
    uint16_t actCount;
} GT10IsocFrame;

typedef enum {
    GT10FrameGood = 0,  // success, or underrun for a short packet
    GT10FrameLost,      // a transient bus or host controller error for one packet
    GT10FrameTerminal,  // scheduling, cancellation, removal, or anything unknown
} GT10FrameClass;

// Unknown statuses are terminal, so a surprise stops the stream instead of
// being concealed as silence.
GT10FrameClass GT10ClassifyFrameStatus(int32_t status);

// The most severe class among the frames, and the first status of that class.
GT10FrameClass GT10WorstFrameStatus(const GT10IsocFrame *frames, uint32_t numFrames,
                                    int32_t *firstStatus);

// Receives captured audio in bus-frame order. index is the packet's position
// in the frame list. A NULL bytes with zero frames marks a lost packet, whose
// length is unknown. The receiver decides how much silence stands in for it.
typedef void (*GT10CaptureSink)(void *ctx, uint32_t index, const uint8_t *bytes, uint32_t frames);

typedef struct {
    uint32_t sampleFrames;  // received, estimates for lost packets excluded
    uint32_t lostPackets;
    uint32_t packets;
} GT10CaptureResult;

// Walk a completed input frame list. Frame i starts at the sum of the earlier
// frames' reqCount.
GT10CaptureResult GT10ExtractCapture(const uint8_t *buffer, uint32_t bufferBytes,
                                     const GT10IsocFrame *frames, uint32_t numFrames,
                                     GT10CaptureSink sink, void *ctx);

// ---- Playback aligned to the capture timeline ----

// Beyond this distance playback jumps to its target instead of drifting back.
enum { kGT10RealignFrames = 441 };

// The capture sample position at the start of bus frame frame, extrapolated at
// 44.1 frames per bus frame from positionAfter, the position just past the
// last captured frame lastFrame.
int64_t GT10PredictPosition(int64_t positionAfter, uint64_t lastFrame, uint64_t frame);

// Sample frames to send in one bus frame: the pacer's nominal count, moved by
// one toward target when playback is more than two frames away from it. The
// adaptive endpoint follows the host rate, so a one-frame nudge is inaudible.
uint32_t GT10PlaybackFrames(uint32_t nominal, int64_t position, int64_t target);

#ifdef __cplusplus
}
#endif
