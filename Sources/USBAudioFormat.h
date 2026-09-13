// USB Audio 1.0 stream format decoding for the BOSS GT-10 in Advanced mode.
//
// Written from the USB Device Class Definition for Audio Devices 1.0, sections
// 4.5.2 and 4.5.3. Not derived from any GPL source. The pedal marks its audio
// interfaces vendor class 255, but the class-specific descriptors inside them
// are ordinary UAC-1, so a plain UAC-1 decoder reads them.
//
// Scope is deliberately narrower than UAC-1: PCM only, TYPE_I only, and a
// single discrete sample rate. Those are supported-subset limits, not spec
// requirements. Anything outside them is rejected rather than guessed at,
// because the output becomes isochronous transfer parameters.

#ifndef USB_AUDIO_FORMAT_H
#define USB_AUDIO_FORMAT_H

#include <stddef.h>
#include <stdint.h>

struct USBAudioStreamFormat {
    uint8_t channels;
    uint8_t subframeBytes;
    uint8_t bitResolution;
    uint32_t sampleRate;

    uint32_t SampleFrameBytes() const { return (uint32_t)channels * subframeBytes; }
};

// Decodes the class-specific descriptors of one audio streaming alternate
// setting. The span must cover exactly that alt setting: a standard interface
// descriptor inside it means the caller passed more than one, and is rejected
// rather than silently combining two interfaces.
//
// Every byte of the span must parse. A valid pair followed by garbage is a
// device this decoder does not understand, so it fails rather than accepting a
// prefix.
bool USBAudioParseStreamFormat(const uint8_t *desc, size_t length, USBAudioStreamFormat *out);

// Largest byte count any one service interval can carry for this format.
// Decoding success alone is not permission to stream: 96 kHz decodes fine and
// then needs 576 bytes, which the GT-10's 288 byte endpoint cannot carry. Call
// this against wMaxPacketSize before configuring a transfer.
uint32_t USBAudioMaxPacketBytes(const USBAudioStreamFormat &format, uint32_t intervalsPerSecond);

// Paces packets across service intervals when the sample rate does not divide
// evenly into them. At 44100 Hz over 1 ms intervals the device takes 44 sample
// frames most intervals and 45 on every tenth. Integer only, so it holds exact
// phase where a float accumulator would drift over hours.
//
// The accumulator carries fractional phase, so re-initializing mid-stream
// discards it. Init on start, not on every rate observation.
struct USBAudioPacer {
    uint32_t sampleRate;
    uint32_t intervalsPerSecond;  // 1000 for full-speed 1 ms frames
    uint64_t accumulator;
};

// False on a zero rate or zero interval count. Without this the caller cannot
// tell a bad configuration from an interval that legitimately carries no
// frames, which happens whenever sampleRate is below intervalsPerSecond.
bool USBAudioPacerInit(USBAudioPacer *pacer, uint32_t sampleRate, uint32_t intervalsPerSecond);
uint32_t USBAudioPacerNextFrames(USBAudioPacer *pacer);

#endif
