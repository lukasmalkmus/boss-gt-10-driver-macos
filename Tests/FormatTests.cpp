// Offline checks for the UAC-1 decoder. No hardware, no USB, no kernel calls.
// The reference bytes are the GT-10's own Advanced-mode descriptors, captured
// 2026-09-12 and recorded in FINDINGS.md.

#include "../Sources/USBAudioFormat.h"

#include <stdio.h>
#include <string.h>

static int gFailures = 0;
static int gChecks   = 0;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        gChecks++;                                                                                 \
        if (!(cond)) {                                                                             \
            gFailures++;                                                                           \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);                                          \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

// GT-10 IF0/IF1 alt 1, verbatim.
static const uint8_t kGT10[] = {
    0x07, 0x24, 0x01, 0x01, 0x00, 0x01, 0x00, 0x0b, 0x24,
    0x02, 0x01, 0x02, 0x03, 0x18, 0x01, 0x44, 0xAC, 0x00,
};

// The vendor marker the pedal puts on alt 0. A decoder must step over an
// unknown class descriptor rather than give up at it.
static const uint8_t kRolandMarker[] = {0x06, 0x24, 0xF1, 0x01, 0x00, 0x00};

static void TestRealDevice() {
    printf("real GT-10 descriptors\n");
    USBAudioStreamFormat f = {};
    CHECK(USBAudioParseStreamFormat(kGT10, sizeof(kGT10), &f), "must parse");
    CHECK(f.channels == 2, "channels %u, want 2", f.channels);
    CHECK(f.subframeBytes == 3, "subframe %u, want 3", f.subframeBytes);
    CHECK(f.bitResolution == 24, "bits %u, want 24", f.bitResolution);
    CHECK(f.sampleRate == 44100, "rate %u, want 44100", f.sampleRate);
    CHECK(f.SampleFrameBytes() == 6, "frame %u, want 6", f.SampleFrameBytes());
}

static void TestDescriptorWalk() {
    printf("descriptor walk\n");
    USBAudioStreamFormat f = {};

    uint8_t lead[sizeof(kRolandMarker) + sizeof(kGT10)];
    memcpy(lead, kRolandMarker, sizeof(kRolandMarker));
    memcpy(lead + sizeof(kRolandMarker), kGT10, sizeof(kGT10));
    CHECK(USBAudioParseStreamFormat(lead, sizeof(lead), &f), "must skip the vendor marker");
    CHECK(f.sampleRate == 44100, "rate after skip %u", f.sampleRate);

    uint8_t swapped[sizeof(kGT10)];
    memcpy(swapped, kGT10 + 7, 11);
    memcpy(swapped + 11, kGT10, 7);
    f = {};
    CHECK(USBAudioParseStreamFormat(swapped, sizeof(swapped), &f),
          "order of the two descriptors must not matter");
    CHECK(f.channels == 2, "channels after swap %u", f.channels);

    // 44100 is 44 AC 00, so its top byte is zero and a 16-bit read would still
    // pass. 96000 is 00 77 01 and needs all three bytes.
    uint8_t wide[sizeof(kGT10)];
    memcpy(wide, kGT10, sizeof(wide));
    wide[15] = 0x00;
    wide[16] = 0x77;
    wide[17] = 0x01;
    f        = {};
    CHECK(USBAudioParseStreamFormat(wide, sizeof(wide), &f), "96 kHz must parse");
    CHECK(f.sampleRate == 96000, "rate %u, want 96000", f.sampleRate);
}

static void TestMalformed() {
    printf("malformed input\n");
    USBAudioStreamFormat f = {};
    uint8_t buf[sizeof(kGT10)];

    // A zero length cannot advance the walk.
    memcpy(buf, kGT10, sizeof(buf));
    buf[0] = 0x00;
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(buf), &f), "bLength 0 must reject");

    memcpy(buf, kGT10, sizeof(buf));
    buf[0] = 0x01;
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(buf), &f), "bLength 1 must reject");

    // Claims 11 bytes, only 9 supplied.
    CHECK(!USBAudioParseStreamFormat(kGT10, 16, &f), "truncated must reject");

    memcpy(buf, kGT10, sizeof(buf));
    buf[10] = 0x02;  // bFormatType TYPE_II
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(buf), &f), "TYPE_II must reject");

    memcpy(buf, kGT10, sizeof(buf));
    buf[14] = 0x02;  // two rates declared, bLength still says one
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(buf), &f),
          "rate count against bLength must reject");

    memcpy(buf, kGT10, sizeof(buf));
    buf[14] = 0x00;  // continuous range
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(buf), &f), "continuous must reject");

    memcpy(buf, kGT10, sizeof(buf));
    buf[5] = 0x02;  // format tag not PCM
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(buf), &f), "non-PCM must reject");

    memcpy(buf, kGT10, sizeof(buf));
    buf[11] = 0x00;  // zero channels
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(buf), &f), "0 channels must reject");

    memcpy(buf, kGT10, sizeof(buf));
    buf[12] = 0x00;  // zero subframe
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(buf), &f), "0 subframe must reject");

    memcpy(buf, kGT10, sizeof(buf));
    buf[13] = 0x20;  // 32 bits inside a 3 byte subframe
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(buf), &f),
          "bits wider than the subframe must reject");

    memcpy(buf, kGT10, sizeof(buf));
    buf[15] = 0x00;
    buf[16] = 0x00;
    buf[17] = 0x00;  // rate 0
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(buf), &f), "rate 0 must reject");

    CHECK(!USBAudioParseStreamFormat(kGT10, 7, &f), "AS_GENERAL alone must reject");
    CHECK(!USBAudioParseStreamFormat(kGT10 + 7, 11, &f), "format alone must reject");
}

static void TestPacer() {
    printf("packet pacing\n");
    USBAudioPacer p = {};

    // One second at 44.1 kHz must place exactly 44100 sample frames, or capture
    // and playback drift apart by 100 frames every second.
    USBAudioPacerInit(&p, 44100, 1000);
    uint32_t total = 0, maxFrames = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t n = USBAudioPacerNextFrames(&p);
        total += n;
        if (n > maxFrames) maxFrames = n;
    }
    CHECK(total == 44100, "one second placed %u frames, want 44100", total);
    CHECK(maxFrames == 45, "largest interval %u frames, want 45", maxFrames);

    // 45 frames is 270 bytes. The endpoint advertises 288, so a correct pacer
    // never overruns wMaxPacketSize.
    CHECK(maxFrames * 6 <= 288, "%u bytes exceeds wMaxPacketSize 288", maxFrames * 6);

    // The tenth interval carries the extra frame.
    USBAudioPacerInit(&p, 44100, 1000);
    for (int i = 0; i < 9; i++) {
        CHECK(USBAudioPacerNextFrames(&p) == 44, "interval %d must be 44", i);
    }
    CHECK(USBAudioPacerNextFrames(&p) == 45, "interval 10 must be 45");

    // A rate that divides evenly must never vary.
    USBAudioPacerInit(&p, 48000, 1000);
    total = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t n = USBAudioPacerNextFrames(&p);
        CHECK(n == 48, "48 kHz interval %d gave %u", i, n);
        total += n;
    }
    CHECK(total == 48000, "48 kHz second placed %u", total);
}

static void TestSpanIntegrity() {
    printf("span integrity\n");
    USBAudioStreamFormat f = {};
    uint8_t buf[64];

    // A stray byte after a valid pair cannot be a descriptor.
    memcpy(buf, kGT10, sizeof(kGT10));
    buf[sizeof(kGT10)] = 0x00;
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(kGT10) + 1, &f),
          "trailing stray byte must reject");

    // A trailing descriptor that lies about its length.
    memcpy(buf, kGT10, sizeof(kGT10));
    buf[sizeof(kGT10)]     = 0xFF;
    buf[sizeof(kGT10) + 1] = 0x24;
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(kGT10) + 2, &f),
          "trailing oversized bLength must reject");

    // An unknown but well formed descriptor is not garbage, and is skipped.
    memcpy(buf, kGT10, sizeof(kGT10));
    memcpy(buf + sizeof(kGT10), kRolandMarker, sizeof(kRolandMarker));
    CHECK(USBAudioParseStreamFormat(buf, sizeof(kGT10) + sizeof(kRolandMarker), &f),
          "trailing vendor marker must still parse");

    memcpy(buf, kGT10, sizeof(kGT10));
    memcpy(buf + sizeof(kGT10), kGT10, 7);
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(kGT10) + 7, &f),
          "duplicate AS_GENERAL must reject");

    memcpy(buf, kGT10, sizeof(kGT10));
    memcpy(buf + sizeof(kGT10), kGT10 + 7, 11);
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(kGT10) + 11, &f),
          "duplicate FORMAT_TYPE must reject");

    // AS_GENERAL from one alt setting, FORMAT_TYPE from the next.
    static const uint8_t kIface[] = {0x09, 0x04, 0x01, 0x01, 0x01, 0xFF, 0x02, 0x01, 0x00};
    memcpy(buf, kGT10, 7);
    memcpy(buf + 7, kIface, sizeof(kIface));
    memcpy(buf + 7 + sizeof(kIface), kGT10 + 7, 11);
    CHECK(!USBAudioParseStreamFormat(buf, 7 + sizeof(kIface) + 11, &f),
          "span crossing an interface boundary must reject");

    // UAC-1 Table 4-19 fixes AS_GENERAL at 7 bytes.
    static const uint8_t kFatGeneral[] = {0x08, 0x24, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00};
    memcpy(buf, kFatGeneral, sizeof(kFatGeneral));
    memcpy(buf + sizeof(kFatGeneral), kGT10 + 7, 11);
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(kFatGeneral) + 11, &f),
          "oversized AS_GENERAL must reject");

    CHECK(!USBAudioParseStreamFormat(kGT10, 6, &f), "truncated AS_GENERAL must reject");
}

static void TestRateSelection() {
    printf("rate selection\n");
    USBAudioStreamFormat f = {};
    uint8_t buf[32];

    // Two declared rates with an honest bLength. Taking the first silently is
    // how a device gets streamed at the wrong rate.
    static const uint8_t kTwoRates[] = {0x0e, 0x24, 0x02, 0x01, 0x02, 0x03, 0x18,
                                        0x02, 0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00};
    memcpy(buf, kGT10, 7);
    memcpy(buf + 7, kTwoRates, sizeof(kTwoRates));
    CHECK(!USBAudioParseStreamFormat(buf, 7 + sizeof(kTwoRates), &f),
          "two declared rates must reject");

    // Continuous range, bSamFreqType 0, with the length UAC-1 gives it.
    static const uint8_t kContinuous[] = {0x0e, 0x24, 0x02, 0x01, 0x02, 0x03, 0x18,
                                          0x00, 0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00};
    memcpy(buf, kGT10, 7);
    memcpy(buf + 7, kContinuous, sizeof(kContinuous));
    CHECK(!USBAudioParseStreamFormat(buf, 7 + sizeof(kContinuous), &f),
          "well formed continuous range must reject");

    // High byte of wFormatTag must be read.
    memcpy(buf, kGT10, sizeof(kGT10));
    buf[6] = 0x01;
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(kGT10), &f), "format tag 0x0101 must reject");

    memcpy(buf, kGT10, sizeof(kGT10));
    buf[12] = 0x05;
    CHECK(!USBAudioParseStreamFormat(buf, sizeof(kGT10), &f),
          "subframe wider than 4 bytes must reject");
}

static void TestPacketCapacity() {
    printf("packet capacity\n");
    USBAudioStreamFormat gt10 = {2, 3, 24, 44100};
    CHECK(USBAudioMaxPacketBytes(gt10, 1000) == 270, "GT-10 peak %u bytes, want 270",
          USBAudioMaxPacketBytes(gt10, 1000));
    CHECK(USBAudioMaxPacketBytes(gt10, 1000) <= 288, "GT-10 must fit its endpoint");

    // Decoding 96 kHz succeeds, but the pedal's 288 byte endpoint cannot carry
    // it. Decode success is not permission to stream.
    USBAudioStreamFormat wide = {2, 3, 24, 96000};
    CHECK(USBAudioMaxPacketBytes(wide, 1000) == 576, "96 kHz peak %u bytes, want 576",
          USBAudioMaxPacketBytes(wide, 1000));
    CHECK(USBAudioMaxPacketBytes(wide, 1000) > 288, "96 kHz must not fit 288");

    // Divides evenly, so the peak is the steady value and exactly fills 288.
    USBAudioStreamFormat r48 = {2, 3, 24, 48000};
    CHECK(USBAudioMaxPacketBytes(r48, 1000) == 288, "48 kHz peak must be 288");

    CHECK(USBAudioMaxPacketBytes(gt10, 0) == 0, "zero intervals must give 0");
    CHECK(gt10.SampleFrameBytes() == 6, "stereo 24 bit frame must be 6 bytes");
    USBAudioStreamFormat mono16 = {1, 2, 16, 44100};
    CHECK(mono16.SampleFrameBytes() == 2, "mono 16 bit frame must be 2 bytes");
}

static void TestPacerContract() {
    printf("pacer contract\n");
    USBAudioPacer p = {};

    CHECK(!USBAudioPacerInit(&p, 0, 1000), "zero sample rate must fail init");
    CHECK(!USBAudioPacerInit(&p, 44100, 0), "zero intervals must fail init");
    CHECK(USBAudioPacerInit(&p, 44100, 1000), "valid arguments must init");

    // High speed, 8 microframes per millisecond.
    CHECK(USBAudioPacerInit(&p, 44100, 8000), "8 kHz interval init");
    uint32_t total = 0, small = 0, large = 0;
    for (int i = 0; i < 8000; i++) {
        uint32_t n = USBAudioPacerNextFrames(&p);
        total += n;
        if (n == 5) small++;
        if (n == 6) large++;
    }
    CHECK(total == 44100, "microframe second placed %u, want 44100", total);
    CHECK(small + large == 8000, "microframes gave counts outside 5 and 6");

    // A rate below the interval rate legitimately yields empty intervals.
    CHECK(USBAudioPacerInit(&p, 500, 1000), "sub interval rate init");
    total            = 0;
    uint32_t empties = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t n = USBAudioPacerNextFrames(&p);
        if (n == 0) empties++;
        total += n;
    }
    CHECK(total == 500, "sub interval second placed %u, want 500", total);
    CHECK(empties == 500, "expected 500 empty intervals, got %u", empties);

    // Init clears fractional phase. Without the reset the ninth call after a
    // re-init would carry the old accumulator and emit 45 early.
    CHECK(USBAudioPacerInit(&p, 44100, 1000), "init");
    for (int i = 0; i < 9; i++)
        USBAudioPacerNextFrames(&p);
    CHECK(USBAudioPacerInit(&p, 44100, 1000), "re-init");
    CHECK(USBAudioPacerNextFrames(&p) == 44, "re-init must clear fractional phase");
}

int main(void) {
    TestRealDevice();
    TestDescriptorWalk();
    TestMalformed();
    TestPacer();
    TestSpanIntegrity();
    TestRateSelection();
    TestPacketCapacity();
    TestPacerContract();

    printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
