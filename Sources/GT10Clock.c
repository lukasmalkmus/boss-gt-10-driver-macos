#include "GT10Clock.h"

#include <stdatomic.h>
#include <stdlib.h>

void GT10ClockInit(GT10Clock *c, double nominalTicksPerFrame, uint32_t period) {
    *c                      = (GT10Clock){0};
    c->nominalTicksPerFrame = nominalTicksPerFrame;
    c->period               = period;
}

void GT10ClockObserve(GT10Clock *c, uint64_t frame, uint64_t hostTime) {
    if (!c->anchored) {
        c->anchored    = true;
        c->anchorFrame = frame;
        c->anchorHost  = hostTime;
    }
    if (frame > c->latestFrame || c->latestHost == 0) {
        c->latestFrame = frame;
        c->latestHost  = hostTime;
    }
}

double GT10ClockTicksPerFrame(const GT10Clock *c) {
    if (!c->anchored || c->latestFrame < c->anchorFrame + kGT10ClockMinBaseline ||
        c->latestHost <= c->anchorHost) {
        return c->nominalTicksPerFrame;
    }
    return (double)(c->latestHost - c->anchorHost) / (double)(c->latestFrame - c->anchorFrame);
}

uint64_t GT10ClockHostOfFrame(const GT10Clock *c, uint64_t frame) {
    const double tpf   = GT10ClockTicksPerFrame(c);
    const double delta = (double)((int64_t)(frame - c->anchorFrame)) * tpf;
    return (uint64_t)((double)c->anchorHost + delta);
}

bool GT10ClockPacket(GT10Clock *c, uint64_t frame, uint32_t frames, int64_t *zeroSample,
                     uint64_t *zeroHost) {
    bool published = false;
    // A late anchor can leave nextZero behind the sample count. Skip forward to
    // the next boundary instead of timestamping a packet already past.
    if (c->nextZero < c->samples) {
        const int64_t behind = c->samples - c->nextZero;
        c->nextZero += (behind + c->period - 1) / c->period * c->period;
    }
    if (c->anchored && frames > 0 && c->nextZero < c->samples + frames) {
        const int64_t k       = c->nextZero - c->samples;
        const double fraction = (double)k / (double)frames;
        *zeroSample           = c->nextZero;
        *zeroHost =
            GT10ClockHostOfFrame(c, frame) + (uint64_t)(fraction * GT10ClockTicksPerFrame(c));
        c->nextZero += c->period;
        published = true;
    }
    c->samples += frames;
    return published;
}

// Two seqlocks whose fields are all atomic. Atomic stores kept their order
// across cores on Apple Silicon (atomic-then-plain did not), so a reader that
// sees any new field also sees the odd version stored before it. The writer
// always fills the slot that is not newest, then flips newest, so the slot a
// reader picks is only rewritten after two more publications.
typedef struct {
    _Atomic uint64_t version;  // odd while being written
    _Atomic bool valid;
    _Atomic int64_t sample;
    _Atomic uint64_t host;
} Slot;

struct GT10TimeStamp {
    Slot slot[2];
    _Atomic uint32_t newest;
};

enum { kAttemptsPerSlot = 4 };

GT10TimeStamp *GT10TimeStampCreate(void) { return calloc(1, sizeof(GT10TimeStamp)); }

void GT10TimeStampDestroy(GT10TimeStamp *ts) { free(ts); }

static void WriteSlot(Slot *slot, bool valid, int64_t sample, uint64_t host) {
    const uint64_t v = atomic_load(&slot->version);
    atomic_store(&slot->version, v + 1);
    atomic_store(&slot->valid, valid);
    atomic_store(&slot->sample, sample);
    atomic_store(&slot->host, host);
    atomic_store(&slot->version, v + 2);
}

void GT10TimeStampPublish(GT10TimeStamp *ts, int64_t sample, uint64_t hostTime) {
    const uint32_t w = 1 - atomic_load(&ts->newest);
    WriteSlot(&ts->slot[w], true, sample, hostTime);
    atomic_store(&ts->newest, w);
}

void GT10TimeStampClear(GT10TimeStamp *ts) {
    WriteSlot(&ts->slot[0], false, 0, 0);
    WriteSlot(&ts->slot[1], false, 0, 0);
}

static int ReadSlot(const Slot *slot, int64_t *sample, uint64_t *host) {
    for (int i = 0; i < kAttemptsPerSlot; i++) {
        const uint64_t v1 = atomic_load(&slot->version);
        if (v1 & 1) continue;
        const bool valid = atomic_load(&slot->valid);
        const int64_t s  = atomic_load(&slot->sample);
        const uint64_t h = atomic_load(&slot->host);
        if (atomic_load(&slot->version) != v1) continue;
        if (!valid) return 0;
        *sample = s;
        *host   = h;
        return 1;
    }
    return -1;  // contended
}

bool GT10TimeStampRead(const GT10TimeStamp *ts, int64_t *sample, uint64_t *hostTime) {
    const uint32_t n = atomic_load(&ts->newest);
    if (ReadSlot(&ts->slot[n], sample, hostTime) == 1) return true;
    return ReadSlot(&ts->slot[1 - n], sample, hostTime) == 1;
}
