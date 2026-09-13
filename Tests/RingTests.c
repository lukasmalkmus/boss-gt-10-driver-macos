#include "../Sources/GT10Ring.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
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

enum { kBPF = 6 };

// Frame n holds the bytes of n, so any misplaced frame is visible.
static void Fill(uint8_t *buf, int64_t first, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        const uint32_t v = (uint32_t)(first + i) + 1;
        for (int b = 0; b < kBPF; b++)
            buf[i * kBPF + b] = (uint8_t)(v >> (b * 4));
    }
}

static bool FrameIs(const uint8_t *buf, uint32_t index, int64_t frame) {
    uint8_t want[kBPF];
    Fill(want, frame, 1);
    return memcmp(buf + index * kBPF, want, kBPF) == 0;
}

static bool FrameSilent(const uint8_t *buf, uint32_t index) {
    static const uint8_t zero[kBPF];
    return memcmp(buf + index * kBPF, zero, kBPF) == 0;
}

static bool FramesAre(const uint8_t *buf, uint32_t index, int64_t frame, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (!FrameIs(buf, index + i, frame + i)) return false;
    return true;
}

static void Write(GT10Ring *r, int64_t pos, uint32_t n) {
    uint8_t in[64 * kBPF];
    Fill(in, pos, n);
    GT10RingWrite(r, pos, in, n);
}

static void TestCreate(void) {
    CHECK(GT10RingCreate(0, kBPF) == NULL);
    CHECK(GT10RingCreate(100, kBPF) == NULL);
    CHECK(GT10RingCreate(64, 0) == NULL);
    CHECK(GT10RingCreate(1u << 31, kBPF) == NULL);
    GT10Ring *r = GT10RingCreate(64, kBPF);
    CHECK(r != NULL && GT10RingEnd(r) == 0 && GT10RingDropped(r) == 0);
    GT10RingDestroy(r);
    GT10RingDestroy(NULL);
}

static void TestAppendAndRead(void) {
    GT10Ring *r = GT10RingCreate(16, kBPF);
    uint8_t out[64 * kBPF];

    memset(out, 0xEE, sizeof out);
    CHECK(GT10RingRead(r, 0, out, 4) == 0);
    CHECK(FrameSilent(out, 0) && FrameSilent(out, 3));

    // Reads trail the writer, so it may keep appending across the wrap.
    Write(r, 0, 10);
    CHECK(GT10RingRead(r, 4, out, 6) == 6 && FramesAre(out, 0, 4, 6));
    Write(r, 10, 10);
    CHECK(GT10RingEnd(r) == 20);
    CHECK(GT10RingRead(r, 4, out, 16) == 16 && FramesAre(out, 0, 4, 16));

    // Past the end: silence for the unwritten part only.
    memset(out, 0xEE, sizeof out);
    CHECK(GT10RingRead(r, 18, out, 4) == 2);
    CHECK(FramesAre(out, 0, 18, 2) && FrameSilent(out, 2) && FrameSilent(out, 3));
    CHECK(GT10RingRead(r, 40, out, 4) == 0);
    GT10RingDestroy(r);
}

static void TestNoRewrite(void) {
    GT10Ring *r = GT10RingCreate(16, kBPF);
    uint8_t in[8 * kBPF], out[16 * kBPF];
    Write(r, 0, 12);

    // A published frame is never changed.
    Fill(in, 100, 4);
    GT10RingWrite(r, 4, in, 4);
    CHECK(GT10RingEnd(r) == 12 && GT10RingDropped(r) == 4);
    CHECK(GT10RingRead(r, 4, out, 4) == 4 && FramesAre(out, 0, 4, 4));

    // A write overlapping the end keeps only the new part.
    Write(r, 10, 4);
    CHECK(GT10RingEnd(r) == 14 && GT10RingDropped(r) == 6);
    CHECK(GT10RingRead(r, 10, out, 4) == 4 && FramesAre(out, 0, 10, 4));
    GT10RingDestroy(r);
}

static void TestFloorBoundsWriter(void) {
    GT10Ring *r = GT10RingCreate(16, kBPF);
    uint8_t out[16 * kBPF];

    // The reader has read up to 0, so the writer stops at one capacity.
    Write(r, 0, 12);
    Write(r, 12, 8);
    CHECK(GT10RingEnd(r) == 16 && GT10RingDropped(r) == 4);
    CHECK(GT10RingRead(r, 0, out, 16) == 16 && FramesAre(out, 0, 0, 16));

    // Everything written ahead of the limit is dropped.
    Write(r, 20, 4);
    CHECK(GT10RingEnd(r) == 16 && GT10RingDropped(r) == 8);

    // Once the reader moves on, the writer resumes and silences the frames it
    // had to skip.
    CHECK(GT10RingRead(r, 10, out, 4) == 4);
    Write(r, 24, 2);
    CHECK(GT10RingEnd(r) == 26);
    memset(out, 0xEE, sizeof out);
    CHECK(GT10RingRead(r, 16, out, 10) == 10);
    CHECK(FrameSilent(out, 0) && FrameSilent(out, 7) && FramesAre(out, 8, 24, 2));
    GT10RingDestroy(r);
}

static void TestReadsMoveForward(void) {
    GT10Ring *r = GT10RingCreate(16, kBPF);
    uint8_t out[16 * kBPF];
    Write(r, 0, 12);
    CHECK(GT10RingRead(r, 8, out, 2) == 2);

    // Behind the furthest read: the writer may already reuse those slots.
    memset(out, 0xEE, sizeof out);
    CHECK(GT10RingRead(r, 4, out, 8) == 4);
    CHECK(FrameSilent(out, 0) && FrameSilent(out, 3) && FramesAre(out, 4, 8, 4));
    GT10RingDestroy(r);
}

static void TestLapJump(void) {
    GT10Ring *r = GT10RingCreate(16, kBPF);
    uint8_t out[32 * kBPF];
    Write(r, 0, 16);
    CHECK(GT10RingRead(r, 90, out, 1) == 0);  // the reader jumps ahead

    Write(r, 100, 4);
    CHECK(GT10RingEnd(r) == 104);
    memset(out, 0xEE, sizeof out);
    CHECK(GT10RingRead(r, 90, out, 14) == 4);
    CHECK(FrameSilent(out, 0) && FrameSilent(out, 9) && FramesAre(out, 10, 100, 4));
    GT10RingDestroy(r);
}

static void TestArguments(void) {
    GT10Ring *r = GT10RingCreate(8, kBPF);
    uint8_t in[kBPF], out[8 * kBPF];
    Write(r, 0, 8);
    Fill(in, 500, 1);
    GT10RingWrite(r, -1, in, 1);
    GT10RingWrite(r, 8, NULL, 1);
    GT10RingWrite(r, 8, in, 0);
    CHECK(GT10RingEnd(r) == 8 && GT10RingDropped(r) == 0);
    CHECK(GT10RingRead(r, 0, NULL, 1) == 0);
    CHECK(GT10RingRead(r, 0, out, 0) == 0);
    CHECK(GT10RingRead(r, 0, out, 8) == 8 && FramesAre(out, 0, 0, 8));

    CHECK(GT10RingRead(r, 6, out, 2) == 2);
    GT10RingReset(r);
    CHECK(GT10RingEnd(r) == 0 && GT10RingDropped(r) == 0 && GT10RingRead(r, 0, out, 1) == 0);
    // A reset also forgets how far the reader got.
    Write(r, 0, 8);
    CHECK(GT10RingRead(r, 0, out, 8) == 8 && FramesAre(out, 0, 0, 8));
    GT10RingDestroy(r);
}

// The writer appends as fast as it can. The reader trails close to the oldest
// edge of the window. Every frame the reader keeps must hold its own position.
typedef struct {
    GT10Ring *ring;
    _Atomic bool done;
} Shared;

static void *Writer(void *arg) {
    Shared *s   = arg;
    int64_t pos = 0;
    for (int i = 0; i < 400000; i++) {
        const uint32_t n = 1 + (uint32_t)(i % 37);
        Write(s->ring, pos, n);
        pos += n;
    }
    atomic_store(&s->done, true);
    return NULL;
}

static void TestConcurrent(void) {
    enum { kCap = 256 };
    Shared s = {GT10RingCreate(kCap, kBPF), false};
    pthread_t t;
    pthread_create(&t, NULL, Writer, &s);
    uint8_t out[kCap * kBPF];
    long bad = 0, validSeen = 0;
    while (!atomic_load(&s.done)) {
        const int64_t e   = GT10RingEnd(s.ring);
        const int64_t pos = e > kCap - 8 ? e - (kCap - 8) : 0;
        const uint32_t n  = kCap - 8;
        GT10RingRead(s.ring, pos, out, n);
        for (uint32_t i = 0; i < n; i++) {
            if (FrameSilent(out, i)) continue;
            validSeen++;
            if (!FrameIs(out, i, pos + i)) bad++;
        }
    }
    pthread_join(t, NULL);
    CHECK(bad == 0);
    CHECK(validSeen > 0);
    GT10RingDestroy(s.ring);
}

int main(void) {
    TestCreate();
    TestAppendAndRead();
    TestNoRewrite();
    TestFloorBoundsWriter();
    TestReadsMoveForward();
    TestLapJump();
    TestArguments();
    TestConcurrent();
    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
