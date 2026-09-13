#include "USBAudioFormat.h"

namespace {

constexpr uint8_t kInterfaceDesc = 0x04;
constexpr uint8_t kCSInterface   = 0x24;
constexpr uint8_t kASGeneral     = 0x01;
constexpr uint8_t kFormatType    = 0x02;
constexpr uint8_t kFormatTypeI   = 0x01;
constexpr uint16_t kFormatTagPCM = 0x0001;

// UAC-1 Table 4-19 fixes AS_GENERAL at 7 bytes. TYPE_I discrete is 8 plus 3
// per declared rate.
constexpr uint8_t kASGeneralLength    = 7;
constexpr uint8_t kFormatTypeIMinimum = 8;

// A subframe wider than 4 bytes is outside TYPE_I, and 0 channels would make
// the frame size zero and divide by it downstream.
constexpr uint8_t kMaxSubframeBytes = 4;

uint32_t ReadSampleRate24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

}  // namespace

bool USBAudioParseStreamFormat(const uint8_t *desc, size_t length, USBAudioStreamFormat *out) {
    if (!desc || !out) return false;

    const uint8_t *general = nullptr;
    const uint8_t *format  = nullptr;
    uint8_t generalLength  = 0;
    uint8_t formatLength   = 0;

    size_t offset = 0;
    while (offset < length) {
        // Subtraction rather than offset + bLength, which can wrap.
        const size_t remaining = length - offset;

        const uint8_t bLength = desc[offset];
        // Below 2 the walk cannot advance. Passing both halves also proves
        // remaining >= 2, which is what makes the type byte below in bounds.
        if (bLength < 2 || bLength > remaining) return false;

        const uint8_t bDescriptorType = desc[offset + 1];
        if (bDescriptorType == kInterfaceDesc) return false;

        if (bDescriptorType == kCSInterface && bLength >= 3) {
            const uint8_t subtype = desc[offset + 2];
            if (subtype == kASGeneral) {
                if (general) return false;
                general       = desc + offset;
                generalLength = bLength;
            } else if (subtype == kFormatType) {
                if (format) return false;
                format       = desc + offset;
                formatLength = bLength;
            }
        }
        offset += bLength;
    }

    if (!general || generalLength != kASGeneralLength) return false;
    if (!format) return false;

    const uint16_t formatTag = (uint16_t)general[5] | ((uint16_t)general[6] << 8);
    if (formatTag != kFormatTagPCM) return false;

    if (formatLength < kFormatTypeIMinimum) return false;
    if (format[3] != kFormatTypeI) return false;

    const uint8_t channels      = format[4];
    const uint8_t subframeBytes = format[5];
    const uint8_t bitResolution = format[6];
    const uint8_t rateCount     = format[7];

    // Count 0 means a continuous range. More than one rate would need a
    // selection policy this decoder does not have, and taking the first
    // silently is how a device ends up streamed at the wrong rate.
    if (rateCount != 1) return false;
    if (formatLength != kFormatTypeIMinimum + 3) return false;

    if (channels == 0) return false;
    if (subframeBytes == 0 || subframeBytes > kMaxSubframeBytes) return false;
    if (bitResolution == 0 || bitResolution > subframeBytes * 8) return false;

    const uint32_t rate = ReadSampleRate24(format + kFormatTypeIMinimum);
    if (rate == 0) return false;

    out->channels      = channels;
    out->subframeBytes = subframeBytes;
    out->bitResolution = bitResolution;
    out->sampleRate    = rate;
    return true;
}

uint32_t USBAudioMaxPacketBytes(const USBAudioStreamFormat &format, uint32_t intervalsPerSecond) {
    if (intervalsPerSecond == 0) return 0;

    uint32_t frames = format.sampleRate / intervalsPerSecond;
    if (format.sampleRate % intervalsPerSecond) frames += 1;
    return frames * format.SampleFrameBytes();
}

bool USBAudioPacerInit(USBAudioPacer *pacer, uint32_t sampleRate, uint32_t intervalsPerSecond) {
    if (!pacer || sampleRate == 0 || intervalsPerSecond == 0) return false;

    pacer->sampleRate         = sampleRate;
    pacer->intervalsPerSecond = intervalsPerSecond;
    pacer->accumulator        = 0;
    return true;
}

uint32_t USBAudioPacerNextFrames(USBAudioPacer *pacer) {
    if (!pacer || pacer->intervalsPerSecond == 0) return 0;

    uint32_t frames = pacer->sampleRate / pacer->intervalsPerSecond;
    pacer->accumulator += pacer->sampleRate % pacer->intervalsPerSecond;
    if (pacer->accumulator >= pacer->intervalsPerSecond) {
        pacer->accumulator -= pacer->intervalsPerSecond;
        frames += 1;
    }
    return frames;
}
