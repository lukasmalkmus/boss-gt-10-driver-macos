// Audio ring addressed by absolute sample position, for one writer thread and
// one reader thread. The HAL asks for input and delivers output by sample
// time, so a position lookup replaces a FIFO read pointer. Neither side blocks.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GT10Ring GT10Ring;

// capacityFrames must be a power of two. Allocates, so never call it on the IO
// thread.
GT10Ring *GT10RingCreate(uint32_t capacityFrames, uint32_t bytesPerFrame);
void GT10RingDestroy(GT10Ring *ring);

// Forget all frames. Only while neither side reads or writes.
void GT10RingReset(GT10Ring *ring);

// Writer side. Append frames at [position, position + frames). The ring never
// changes a published frame and never runs more than one capacity ahead of the
// reader, so frames at or before the published end, or too far ahead, are
// dropped and counted. A forward jump reads back as silence.
void GT10RingWrite(GT10Ring *ring, int64_t position, const uint8_t *src, uint32_t frames);

// Reader side. Copy [position, position + frames) into dst. Positions are
// expected to move forward: anything before the furthest position read so far,
// not written yet, or overwritten reads as silence. Returns how many frames
// were valid.
uint32_t GT10RingRead(GT10Ring *ring, int64_t position, uint8_t *dst, uint32_t frames);

// One past the newest published position, or 0 when empty.
int64_t GT10RingEnd(const GT10Ring *ring);

// Frames the writer dropped since the last reset.
uint64_t GT10RingDropped(const GT10Ring *ring);

#ifdef __cplusplus
}
#endif
