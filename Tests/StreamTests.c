#include "../Driver/GT10StreamCore.h"

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

enum { kUnderrun = (int32_t)0xE00002E7, kOverrun = (int32_t)0xE00002E8 };

// Records what the sink receives: a flat copy of the audio, a count of lost
// packet marks, and each call's first byte so offsets are checkable.
typedef struct {
    uint8_t audio[64 * 288];
    uint32_t frames;
    uint32_t calls;
    int silentCalls;
    uint8_t firstByte[64];
    uint32_t index[64];
} Sink;

static void Record(void *ctx, uint32_t index, const uint8_t *bytes, uint32_t frames) {
    Sink *s = ctx;
    if (s->calls < 64) s->index[s->calls] = index;
    const uint32_t n = frames * kGT10BytesPerFrame;
    if (bytes != NULL)
        memcpy(s->audio + s->frames * kGT10BytesPerFrame, bytes, n);
    else if (frames == 0)
        s->silentCalls++;
    if (s->calls < 64) s->firstByte[s->calls] = bytes != NULL ? bytes[0] : 0;
    s->frames += frames;
    s->calls++;
}

// Slot i holds bytes of value i + 1 up to act, and 0xA5 after it, like the
// probe's prefilled capture buffer.
static void FillSlots(uint8_t *buf, GT10IsocFrame *frames, uint32_t n, const uint16_t *act) {
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        frames[i].status   = kUnderrun;
        frames[i].reqCount = 288;
        frames[i].actCount = act[i];
        memset(buf + off, 0xA5, 288);
        memset(buf + off, (int)(i + 1), act[i]);
        off += 288;
    }
}

static void TestExtract(void) {
    uint8_t buf[8 * 288];
    GT10IsocFrame frames[8];
    uint16_t act[8] = {264, 264, 270, 264, 264, 264, 264, 264};
    FillSlots(buf, frames, 8, act);

    Sink s;
    memset(&s, 0, sizeof s);
    GT10CaptureResult r = GT10ExtractCapture(buf, sizeof buf, frames, 8, Record, &s);
    CHECK(r.sampleFrames == 7 * 44 + 45 && s.frames == r.sampleFrames);
    CHECK(r.lostPackets == 0 && r.packets == 8 && s.silentCalls == 0);
    // Each packet starts at its own slot, and no fill byte leaks into audio.
    CHECK(s.firstByte[0] == 1 && s.firstByte[2] == 3 && s.firstByte[7] == 8);
    bool noFill = true;
    for (uint32_t i = 0; i < s.frames * kGT10BytesPerFrame; i++)
        noFill = noFill && s.audio[i] != 0xA5;
    CHECK(noFill);

    // A lost packet is marked, not invented, and the next slot still lines up.
    FillSlots(buf, frames, 8, act);
    frames[1].status = kOverrun;
    memset(&s, 0, sizeof s);
    r = GT10ExtractCapture(buf, sizeof buf, frames, 8, Record, &s);
    CHECK(r.lostPackets == 1 && s.silentCalls == 1 && s.frames == 6 * 44 + 45);
    CHECK(s.firstByte[1] == 0 && s.firstByte[2] == 3);
    CHECK(s.index[1] == 1 && s.index[2] == 2 && s.index[7] == 7);
    CHECK(r.sampleFrames == 6 * 44 + 45);

    // Partial sample frames and oversize counts are lost packets, not audio.
    FillSlots(buf, frames, 8, act);
    frames[3].actCount = 265;
    frames[4].actCount = 294;
    memset(&s, 0, sizeof s);
    r = GT10ExtractCapture(buf, sizeof buf, frames, 8, Record, &s);
    CHECK(r.lostPackets == 2 && s.firstByte[5] == 6);

    // Success status passes like underrun. An empty packet sends nothing.
    FillSlots(buf, frames, 8, act);
    frames[0].status   = 0;
    frames[6].actCount = 0;
    memset(&s, 0, sizeof s);
    r = GT10ExtractCapture(buf, sizeof buf, frames, 8, Record, &s);
    CHECK(r.lostPackets == 0 && r.sampleFrames == 6 * 44 + 45 && s.calls == 7);
    // The call after the empty packet still names its own packet.
    CHECK(s.index[5] == 5 && s.index[6] == 7);

    // A frame list that claims more than the buffer holds never reads past it.
    FillSlots(buf, frames, 8, act);
    frames[7].reqCount = 289;
    memset(&s, 0, sizeof s);
    r = GT10ExtractCapture(buf, 7 * 288 + 288, frames, 8, Record, &s);
    CHECK(r.lostPackets == 1 && s.firstByte[7] == 0);
    memset(&s, 0, sizeof s);
    r = GT10ExtractCapture(buf, 4 * 288, frames, 8, Record, &s);
    CHECK(r.lostPackets == 4 && r.packets == 8);

    // More frames than the result can describe are capped.
    GT10IsocFrame many[kGT10MaxTransferFrames + 8];
    memset(many, 0, sizeof many);
    memset(&s, 0, sizeof s);
    r = GT10ExtractCapture(buf, sizeof buf, many, kGT10MaxTransferFrames + 8, Record, &s);
    CHECK(r.packets == kGT10MaxTransferFrames);

    memset(&s, 0, sizeof s);
    r = GT10ExtractCapture(NULL, sizeof buf, frames, 8, Record, &s);
    CHECK(r.packets == 0 && s.calls == 0);
    r = GT10ExtractCapture(buf, sizeof buf, NULL, 8, Record, &s);
    CHECK(r.packets == 0);
    r = GT10ExtractCapture(buf, sizeof buf, frames, 8, NULL, &s);
    CHECK(r.packets == 0);
}

static void TestClassify(void) {
    CHECK(GT10ClassifyFrameStatus(0) == GT10FrameGood);
    CHECK(GT10ClassifyFrameStatus(kUnderrun) == GT10FrameGood);
    CHECK(GT10ClassifyFrameStatus(kOverrun) == GT10FrameLost);
    CHECK(GT10ClassifyFrameStatus((int32_t)0xE0004001) == GT10FrameLost);      // CRC
    CHECK(GT10ClassifyFrameStatus((int32_t)0xE0004010) == GT10FrameLost);      // link
    CHECK(GT10ClassifyFrameStatus((int32_t)0xE00002EE) == GT10FrameTerminal);  // IsoTooOld
    CHECK(GT10ClassifyFrameStatus((int32_t)0xE00002EF) == GT10FrameTerminal);  // IsoTooNew
    CHECK(GT10ClassifyFrameStatus((int32_t)0xE00002EB) == GT10FrameTerminal);  // Aborted
    CHECK(GT10ClassifyFrameStatus((int32_t)0xE00002C0) == GT10FrameTerminal);  // NoDevice
    CHECK(GT10ClassifyFrameStatus((int32_t)0xE00002D8) ==
          GT10FrameTerminal);  // NotReady, never written
    CHECK(GT10ClassifyFrameStatus(12345) == GT10FrameTerminal);

    GT10IsocFrame f[4] = {{0, 288, 264}, {kUnderrun, 288, 264}, {kOverrun, 288, 0}, {0, 288, 264}};
    int32_t first      = -1;
    CHECK(GT10WorstFrameStatus(f, 4, &first) == GT10FrameLost && first == kOverrun);
    f[3].status = (int32_t)0xE00002EE;
    CHECK(GT10WorstFrameStatus(f, 4, &first) == GT10FrameTerminal && first == (int32_t)0xE00002EE);
    f[2].status = 0;
    f[3].status = 0;
    CHECK(GT10WorstFrameStatus(f, 4, &first) == GT10FrameGood && first == 0);
    // The first status of the worst class, not the last one.
    f[1].status = (int32_t)0xE00002C0;
    f[3].status = (int32_t)0xE00002EB;
    CHECK(GT10WorstFrameStatus(f, 4, &first) == GT10FrameTerminal && first == (int32_t)0xE00002C0);
    CHECK(GT10WorstFrameStatus(NULL, 4, &first) == GT10FrameTerminal);
    CHECK(GT10WorstFrameStatus(f, 0, NULL) == GT10FrameGood);
}

static void TestPredict(void) {
    // Just past the last captured frame the prediction is the position itself.
    CHECK(GT10PredictPosition(1000, 99, 100) == 1000);
    // 44.1 frames per bus frame, rounded.
    CHECK(GT10PredictPosition(1000, 99, 101) == 1044);
    CHECK(GT10PredictPosition(1000, 99, 110) == 1441);
    CHECK(GT10PredictPosition(1000, 99, 105) == 1221);  // 220.5 rounds up
    CHECK(GT10PredictPosition(1000, 99, 95) == 779);    // -220.5 rounds away from zero
    // A difference beyond a day is clamped, so the multiply cannot overflow.
    CHECK(GT10PredictPosition(0, 0, 1ULL << 62) == 3810240000);
    CHECK(GT10PredictPosition(1000, 99, 1100) == 1000 + 44100);
    // Behind the last captured frame the prediction runs backwards.
    CHECK(GT10PredictPosition(1000, 99, 90) == 1000 - 441);
    CHECK(GT10PredictPosition(1000, 99, 99) == 956);
}

static void TestPlaybackFrames(void) {
    CHECK(GT10PlaybackFrames(44, 1000, 1000) == 44);
    CHECK(GT10PlaybackFrames(45, 1000, 1002) == 45);  // inside the dead band
    CHECK(GT10PlaybackFrames(44, 1000, 998) == 44);
    CHECK(GT10PlaybackFrames(44, 1000, 1003) == 45);  // behind: send one more
    CHECK(GT10PlaybackFrames(44, 1000, 997) == 43);   // ahead: send one fewer
    CHECK(GT10PlaybackFrames(48, 0, 5000) == 48);     // never above 288 bytes
    CHECK(GT10PlaybackFrames(1, 5000, 0) == 1);       // never empty
}

int main(void) {
    TestExtract();
    TestClassify();
    TestPredict();
    TestPlaybackFrames();
    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
