#include "GT10Ring.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// Lock free, and correct without store ordering between addresses. On Apple
// Silicon a plain store can become visible to another core before an earlier
// atomic store, so neither side may rely on seeing the other's announcement in
// time. Instead every value one side reads from the other can only be stale in
// the safe direction:
//
//   writer: copies data, then raises end. A reader that sees end sees the data.
//   reader: raises floor, then copies. The writer never stores position q
//           unless q < floor + capacity, so the slot it replaces belonged to a
//           position below floor, which the reader never reads. A stale floor
//           is smaller and only stops the writer earlier.
struct GT10Ring {
    uint8_t *bytes;
    uint32_t capacity;
    uint32_t bytesPerFrame;
    _Atomic int64_t end;       // writer
    _Atomic int64_t start;     // writer, positions below it were skipped
    _Atomic int64_t floor;     // reader
    _Atomic uint64_t dropped;  // writer
};

GT10Ring *GT10RingCreate(uint32_t capacityFrames, uint32_t bytesPerFrame) {
    if (capacityFrames == 0 || (capacityFrames & (capacityFrames - 1)) != 0) return NULL;
    if (bytesPerFrame == 0 || capacityFrames > UINT32_MAX / bytesPerFrame) return NULL;
    GT10Ring *r = calloc(1, sizeof *r);
    if (r == NULL) return NULL;
    r->bytes = calloc(capacityFrames, bytesPerFrame);
    if (r->bytes == NULL) {
        free(r);
        return NULL;
    }
    r->capacity      = capacityFrames;
    r->bytesPerFrame = bytesPerFrame;
    return r;
}

void GT10RingDestroy(GT10Ring *r) {
    if (r == NULL) return;
    free(r->bytes);
    free(r);
}

void GT10RingReset(GT10Ring *r) {
    atomic_store(&r->end, 0);
    atomic_store(&r->start, 0);
    atomic_store(&r->floor, 0);
    atomic_store(&r->dropped, 0);
}

int64_t GT10RingEnd(const GT10Ring *r) { return atomic_load(&r->end); }

uint64_t GT10RingDropped(const GT10Ring *r) { return atomic_load(&r->dropped); }

// Copy between the ring and a flat buffer, splitting at the wrap. A NULL flat
// buffer with toRing silences the range.
static void Span(GT10Ring *r, int64_t position, uint8_t *flat, uint32_t frames, int toRing) {
    const uint32_t mask = r->capacity - 1;
    uint32_t offset     = (uint32_t)((uint64_t)position & mask);
    while (frames > 0) {
        uint32_t n = r->capacity - offset;
        if (n > frames) n = frames;
        const size_t bytes = (size_t)n * r->bytesPerFrame;
        uint8_t *slot      = r->bytes + (size_t)offset * r->bytesPerFrame;
        if (toRing) {
            if (flat != NULL)
                memcpy(slot, flat, bytes);
            else
                memset(slot, 0, bytes);
        } else {
            memcpy(flat, slot, bytes);
        }
        if (flat != NULL) flat += bytes;
        frames -= n;
        offset = 0;
    }
}

void GT10RingWrite(GT10Ring *r, int64_t position, const uint8_t *src, uint32_t frames) {
    if (r == NULL || src == NULL || frames == 0 || position < 0) return;

    const int64_t end = atomic_load(&r->end);
    uint64_t dropped  = 0;

    // Append only: a reader may be copying any published frame.
    if (position < end) {
        const int64_t overlap = end - position;
        if (overlap >= frames) {
            atomic_fetch_add(&r->dropped, frames);
            return;
        }
        src += (size_t)overlap * r->bytesPerFrame;
        frames -= (uint32_t)overlap;
        position = end;
        dropped += (uint64_t)overlap;
    }

    const int64_t limit = atomic_load(&r->floor) + r->capacity;
    if (position >= limit) {
        atomic_fetch_add(&r->dropped, dropped + frames);
        return;
    }
    uint32_t n = frames;
    if (position + n > limit) n = (uint32_t)(limit - position);
    dropped += frames - n;

    if (position > end) {
        // A gap. When it spans a whole lap nothing old is still in the window,
        // so move the window. Otherwise silence the skipped frames, which all
        // lie below limit.
        if (position - end >= r->capacity) {
            atomic_store(&r->start, position);
        } else {
            Span(r, end, NULL, (uint32_t)(position - end), 1);
        }
    }

    Span(r, position, (uint8_t *)src, n, 1);
    atomic_store(&r->end, position + n);
    if (dropped > 0) atomic_fetch_add(&r->dropped, dropped);
}

uint32_t GT10RingRead(GT10Ring *r, int64_t position, uint8_t *dst, uint32_t frames) {
    if (r == NULL || dst == NULL || frames == 0) return 0;
    memset(dst, 0, (size_t)frames * r->bytesPerFrame);

    // Raise the floor before reading anything, and never lower it.
    int64_t floor = atomic_load(&r->floor);
    if (position > floor) {
        floor = position;
        atomic_store(&r->floor, floor);
    }

    // end before start: start is stored before the data that end publishes.
    const int64_t end   = atomic_load(&r->end);
    const int64_t start = atomic_load(&r->start);
    int64_t lo          = end - r->capacity;
    if (lo < start) lo = start;
    if (lo < floor) lo = floor;

    const int64_t from = position > lo ? position : lo;
    const int64_t to   = position + frames < end ? position + frames : end;
    if (from >= to) return 0;
    Span(r, from, dst + (size_t)(from - position) * r->bytesPerFrame, (uint32_t)(to - from), 0);
    return (uint32_t)(to - from);
}
