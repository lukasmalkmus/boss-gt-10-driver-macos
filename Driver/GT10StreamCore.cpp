#include "GT10StreamCore.h"

#include "GT10USBDevice.h"

GT10FrameClass GT10ClassifyFrameStatus(int32_t status) {
    switch ((uint32_t)status) {
    case 0x00000000:  // kIOReturnSuccess
    case 0xE00002E7:  // kIOReturnUnderrun
        return GT10FrameGood;
    case 0xE00002E8:  // kIOReturnOverrun
    case 0xE0004001:  // kIOUSBCRCErr
    case 0xE0004002:  // kIOUSBBitstufErr
    case 0xE0004003:  // kIOUSBDataToggleErr
    case 0xE0004006:  // kIOUSBPIDCheckErr
    case 0xE0004007:  // kIOUSBWrongPIDErr
    case 0xE000400C:  // kIOUSBBufferOverrunErr
    case 0xE000400D:  // kIOUSBBufferUnderrunErr
    case 0xE000400E:  // kIOUSBNotSent1Err
    case 0xE000400F:  // kIOUSBNotSent2Err
    case 0xE0004010:  // kIOUSBLinkErr
        return GT10FrameLost;
    default: return GT10FrameTerminal;
    }
}

GT10FrameClass GT10WorstFrameStatus(const GT10IsocFrame *frames, uint32_t numFrames,
                                    int32_t *firstStatus) {
    GT10FrameClass worst = GT10FrameGood;
    if (firstStatus != nullptr) *firstStatus = 0;
    if (frames == nullptr) return GT10FrameTerminal;
    for (uint32_t i = 0; i < numFrames; i++) {
        const GT10FrameClass c = GT10ClassifyFrameStatus(frames[i].status);
        if (c > worst) {
            worst = c;
            if (firstStatus != nullptr) *firstStatus = frames[i].status;
        }
    }
    return worst;
}

GT10CaptureResult GT10ExtractCapture(const uint8_t *buffer, uint32_t bufferBytes,
                                     const GT10IsocFrame *frames, uint32_t numFrames,
                                     GT10CaptureSink sink, void *ctx) {
    GT10CaptureResult r = {};
    if (buffer == nullptr || frames == nullptr || sink == nullptr) return r;
    if (numFrames > kGT10MaxTransferFrames) numFrames = kGT10MaxTransferFrames;

    uint64_t offset = 0;
    for (uint32_t i = 0; i < numFrames; i++) {
        const GT10IsocFrame &f = frames[i];
        const uint64_t slotEnd = offset + f.reqCount;
        const bool fits        = slotEnd <= bufferBytes;
        if (fits && GT10FrameAcceptable(f.status, f.reqCount, f.actCount, kGT10BytesPerFrame)) {
            const uint32_t n = f.actCount / kGT10BytesPerFrame;
            if (n > 0) sink(ctx, i, buffer + offset, n);
            r.sampleFrames += n;
        } else {
            sink(ctx, i, nullptr, 0);
            r.lostPackets++;
        }
        r.packets++;
        offset = slotEnd;
    }
    return r;
}

int64_t GT10PredictPosition(int64_t positionAfter, uint64_t lastFrame, uint64_t frame) {
    int64_t frames = (int64_t)(frame - (lastFrame + 1));
    // One bus frame per millisecond, so a difference beyond a day is a bogus
    // observation rather than a long gap. Multiplying one out would overflow.
    constexpr int64_t kMaxDelta = 86400000;
    if (frames > kMaxDelta) frames = kMaxDelta;
    if (frames < -kMaxDelta) frames = -kMaxDelta;
    // 44100 / 1000 = 441 / 10, rounded half away from zero.
    const int64_t scaled = frames * 441;
    return positionAfter + (scaled >= 0 ? (scaled + 5) / 10 : (scaled - 5) / 10);
}

uint32_t GT10PlaybackFrames(uint32_t nominal, int64_t position, int64_t target) {
    constexpr int64_t kDeadBand   = 2;
    constexpr uint32_t kMaxFrames = 48;  // 288 bytes
    int64_t n                     = nominal;
    if (target - position > kDeadBand) n += 1;
    if (position - target > kDeadBand) n -= 1;
    if (n < 1) n = 1;
    if (n > kMaxFrames) n = kMaxFrames;
    return (uint32_t)n;
}
