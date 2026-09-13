// Live hardware probe. Run explicitly with `make usb-probe`, never from the
// unit-test target. The plug-in must be uninstalled first, because interface
// opens are exclusive.
//
//   (no args)       checkpoints 1 to 3: find, configure, open IF0 and IF1 on alt 0
//   --monitor [s]   watch arrival and removal
//   --pipes         checkpoint 4a: alt 1 switch and pipe validation, no transfer
//   --read [file]   checkpoint 4b: one bounded isochronous read on IF1
//   --capture s f   checkpoint 4c: stream IF1 for s seconds into WAV file f
//   --play-once     checkpoint 5a: one 4-frame transfer of a quiet tone on IF0
//   --play s        checkpoint 5b: stream a 440 Hz tone on IF0 for s seconds

#include "../Driver/GT10USBDevice.h"
#include "../Driver/GT10USBStream.h"
#include "../Sources/GT10Ring.h"

#include <dispatch/dispatch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { kReadFrames = 8, kReadLeadFrames = 16, kReadWaitMs = 1000 };

static void HeldChanged(bool held, void *ctx) {
    (void)ctx;
    printf("monitor: interfaces %s\n", held ? "HELD" : "RELEASED");
    fflush(stdout);
}

// Watch arrivals and removals for a bounded time, so an unplug and replug can
// be observed reacquiring.
static int RunMonitor(int seconds) {
    dispatch_queue_t q = dispatch_queue_create("gt10.usbprobe", NULL);
    GT10USBMonitor *m  = GT10USBMonitorStart(q, HeldChanged, NULL);
    if (m == NULL) {
        printf("monitor failed to start\n");
        return 1;
    }
    printf("monitor running for %ds, unplug and replug the pedal to test\n", seconds);
    fflush(stdout);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
                       GT10USBMonitorStop(m);
                       printf("monitor stopped\n");
                       exit(0);
                   });
    dispatch_main();
    return 0;
}

static GT10USBDevice *Acquire(void) {
    GT10USBDevice *dev = GT10USBDeviceFind();
    if (dev == NULL) {
        printf("not found: GT-10 in Advanced mode (0582:00da)\n");
        return NULL;
    }
    printf("found\n");
    if (GT10USBDeviceEnsureConfigured(dev) != GT10_OK) {
        printf("configure failed\n");
        GT10USBDeviceFree(dev);
        return NULL;
    }
    printf("configured\n");
    const int opened = GT10USBDeviceOpenInterfaces(dev);
    printf("open interfaces: %s\n",
           GT10USBDeviceHasInterfaces(dev) ? "IF0 and IF1 held" : "incomplete");
    if (opened != GT10_OK) {
        GT10USBDeviceFree(dev);
        return NULL;
    }
    return dev;
}

static void PrintPipe(const char *name, const GT10PipeInfo *p, int32_t firstError) {
    printf("%s: alt %u dir %u ep %u type %u sync %u usage %u interval %u maxPacket %u"
           " firstError 0x%08x\n",
           name, p->alt, p->direction, p->endpointNumber, p->transferType, p->syncType,
           p->usageType, p->interval, p->maxPacketSize, (uint32_t)firstError);
}

// Returns 0 when both interfaces are back on alt 0.
static int Disable(GT10USBDevice *dev) {
    const int r = GT10USBDeviceDisableStreams(dev);
    printf("restore alt 0: %s\n", r == GT10_OK ? "confirmed" : "UNCONFIRMED");
    return r == GT10_OK ? 0 : 1;
}

static int EnableAndReport(GT10USBDevice *dev) {
    GT10StreamReport rep;
    const int r = GT10USBDeviceEnableStreams(dev, &rep);
    printf("device speed %u\n", rep.speed);
    PrintPipe("IF0 pipe", &rep.pipes[0], rep.firstError[0]);
    PrintPipe("IF1 pipe", &rep.pipes[1], rep.firstError[1]);
    printf("enable streams: %d (%s)\n", r,
           r == GT10_OK            ? "both pipes match FINDINGS"
           : r == GT10_ALT_UNKNOWN ? "FAILED, alt 0 unconfirmed"
                                   : "failed, alt 0 confirmed");
    return r;
}

static int RunPipes(void) {
    GT10USBDevice *dev = Acquire();
    if (dev == NULL) return 1;
    const int r = EnableAndReport(dev);
    int rc      = r == GT10_OK ? 0 : 1;
    if (r == GT10_OK) rc |= Disable(dev);
    GT10USBDeviceFree(dev);
    return rc;
}

static long CountFill(const uint8_t *p, uint32_t from, uint32_t to) {
    long n = 0;
    for (uint32_t i = from; i < to; i++)
        n += p[i] == kGT10ReadFill;
    return n;
}

// Distinguishes the two possible buffer layouts: each frame at offset
// i * capacity with an untouched tail, or all frames packed from offset 0.
static void AnalyzeLayout(const GT10ReadReport *rep) {
    uint32_t cap   = rep->numFrames ? rep->bufferBytes / rep->numFrames : 0;
    uint32_t total = 0;
    long slotTail = 0, slotTailFill = 0, slotDataFill = 0, slotData = 0;
    for (uint32_t i = 0; i < rep->numFrames; i++) {
        uint32_t act = rep->frameAct[i] <= cap ? rep->frameAct[i] : cap;
        slotData += act;
        slotDataFill += CountFill(rep->buffer, i * cap, i * cap + act);
        slotTail += cap - act;
        slotTailFill += CountFill(rep->buffer, i * cap + act, (i + 1) * cap);
        total += act;
    }
    long packedTail     = (long)rep->bufferBytes - total;
    long packedTailFill = CountFill(rep->buffer, total, rep->bufferBytes);
    printf("layout: received %u bytes\n", total);
    printf("  slotted: tails %ld/%ld fill bytes, data %ld/%ld fill bytes\n", slotTailFill, slotTail,
           slotDataFill, slotData);
    printf("  packed:  tail %ld/%ld fill bytes\n", packedTailFill, packedTail);
    if (slotTail == 0) printf("  inconclusive: every frame was full\n");
}

static void PrintPeak(const GT10ReadReport *rep) {
    uint32_t cap = rep->numFrames ? rep->bufferBytes / rep->numFrames : 0;
    int32_t peak = 0;
    for (uint32_t i = 0; i < rep->numFrames; i++) {
        uint32_t act     = rep->frameAct[i] <= cap ? rep->frameAct[i] : cap;
        const uint8_t *s = rep->buffer + i * cap;
        for (uint32_t b = 0; b + 3 <= act; b += 3) {
            int32_t v =
                (int32_t)((uint32_t)s[b] | (uint32_t)s[b + 1] << 8 | (uint32_t)s[b + 2] << 16);
            if (v & 0x800000) v -= 0x1000000;
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
    }
    printf("peak |sample| under the slotted layout: %d of 8388608\n", peak);
}

static int RunRead(const char *path) {
    GT10USBDevice *dev = Acquire();
    if (dev == NULL) return 1;
    if (EnableAndReport(dev) != GT10_OK) {
        GT10USBDeviceFree(dev);
        return 1;
    }

    printf("submitting: %d frames, lead %d frames, wait %d ms\n", kReadFrames, kReadLeadFrames,
           kReadWaitMs);
    fflush(stdout);
    GT10ReadReport rep;
    GT10USBDeviceReadOnce(dev, kReadFrames, kReadLeadFrames, kReadWaitMs, &rep);

    if (rep.setupFailure != NULL) {
        printf("not submitted: %s failed, 0x%08x\n", rep.setupFailure, (uint32_t)rep.setupReturn);
        const int rc = Disable(dev);
        GT10USBDeviceFree(dev);
        return 1 | rc;
    }

    static const char *names[] = {"rejected", "completed", "aborted", "ABANDONED"};
    printf("outcome %s, submit 0x%08x, completion 0x%08x, bus frame %llu, start %llu\n",
           names[rep.outcome], (uint32_t)rep.submitReturn, (uint32_t)rep.completionReturn,
           (unsigned long long)rep.busFrame, (unsigned long long)rep.frameStart);

    if (rep.outcome == GT10ReadAbandoned) {
        // The kernel may still own the buffer and frame list. Leave without
        // freeing or restoring anything.
        fflush(stdout);
        _exit(3);
    }

    for (uint32_t i = 0; i < rep.numFrames; i++) {
        const uint8_t *s = rep.buffer + i * (rep.bufferBytes / rep.numFrames);
        printf("  frame %u: status 0x%08x req %u act %u  %02x %02x %02x %02x %02x %02x\n", i,
               (uint32_t)rep.frameStatus[i], rep.frameReq[i], rep.frameAct[i], s[0], s[1], s[2],
               s[3], s[4], s[5]);
    }
    if (rep.outcome != GT10ReadRejected) {
        AnalyzeLayout(&rep);
        PrintPeak(&rep);
        if (path != NULL) {
            FILE *f = fopen(path, "wb");
            if (f != NULL) {
                fwrite(rep.buffer, 1, rep.bufferBytes, f);
                fclose(f);
                printf("buffer written to %s\n", path);
            }
        }
    }
    const bool accepted = GT10ReadAccepted(&rep, 6);
    printf("capture %s\n", accepted ? "ACCEPTED" : "NOT accepted");
    free(rep.buffer);

    int rc = accepted ? 0 : 1;
    rc |= Disable(dev);
    GT10USBDeviceFree(dev);
    return rc;
}

// ---- checkpoints 4c and 5 ----

static void PutLE(FILE *f, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++)
        fputc((int)((v >> (8 * i)) & 0xFF), f);
}

// 24-bit stereo 44100 Hz PCM. The sizes are patched once the data length is known.
static void WavHeader(FILE *f, uint32_t dataBytes) {
    fwrite("RIFF", 1, 4, f);
    PutLE(f, 36 + dataBytes, 4);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    PutLE(f, 16, 4);
    PutLE(f, 1, 2);
    PutLE(f, 2, 2);
    PutLE(f, 44100, 4);
    PutLE(f, 44100 * 6, 4);
    PutLE(f, 6, 2);
    PutLE(f, 24, 2);
    fwrite("data", 1, 4, f);
    PutLE(f, dataBytes, 4);
}

static void PrintStats(const GT10StreamStats *st, const GT10Ring *ring) {
    const uint64_t span = st->lastBusFrame >= st->firstBusFrame && st->captureTransfers
                              ? st->lastBusFrame - st->firstBusFrame + 1
                              : 0;
    const double rate =
        span ? (double)(st->capturedFrames + st->estimatedFrames) * 1000.0 / (double)span : 0.0;
    printf("clock rejected %llu | ", (unsigned long long)st->clockRejected);
    printf("running %d rt %d err 0x%08x | cap xfers %llu frames %llu est %llu lost %llu lostStatus "
           "%llu rate %.2f Hz (%+.0f ppm) lat<=%llu | "
           "play xfers %llu frames %llu frameErr %llu | ooo %llu late %llu | ring dropped %llu\n",
           st->running, st->realtime, (uint32_t)st->firstError,
           (unsigned long long)st->captureTransfers, (unsigned long long)st->capturedFrames,
           (unsigned long long)st->estimatedFrames, (unsigned long long)st->lostPackets,
           (unsigned long long)st->lostFrameStatus, rate,
           span ? (rate - 44100.0) / 44100.0 * 1e6 : 0.0, (unsigned long long)st->maxLatencyFrames,
           (unsigned long long)st->playbackTransfers, (unsigned long long)st->playedFrames,
           (unsigned long long)st->playbackFrameErrors, (unsigned long long)st->outOfOrder,
           (unsigned long long)st->lateSubmits,
           (unsigned long long)(ring ? GT10RingDropped(ring) : 0));
}

// An abandoned engine means the kernel may still own transfer memory, so leave
// at once without freeing.
static int Finish(GT10USBDevice *dev, GT10Stream **stream, GT10StreamStats *finalStats) {
    const int stopped = GT10StreamStop(stream, finalStats);
    if (stopped != GT10_OK) {
        printf("engine ABANDONED: callbacks never arrived, exiting without cleanup\n");
        fflush(stdout);
        _exit(3);
    }
    const int rc = Disable(dev);
    GT10USBDeviceFree(dev);
    return rc;
}

// Reading up to the producer position, not the ring's end, keeps the reader's
// floor moving even after a stall made the ring drop frames.
static bool Drain(GT10Ring *ring, int64_t *readPos, int64_t producer, FILE *wav,
                  uint64_t *dataBytes) {
    uint8_t buf[4096 * 6];
    bool ok = true;
    while (producer > *readPos) {
        const uint32_t n = producer - *readPos > 4096 ? 4096 : (uint32_t)(producer - *readPos);
        GT10RingRead(ring, *readPos, buf, n);
        ok &= fwrite(buf, 6, n, wav) == n;
        *dataBytes += (uint64_t)n * 6;
        *readPos += n;
    }
    return ok;
}

static void SleepMs(unsigned ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int RunCapture(int seconds, const char *path) {
    FILE *wav = fopen(path, "wb");
    if (wav == NULL) {
        printf("cannot open %s\n", path);
        return 1;
    }
    WavHeader(wav, 0);
    GT10Ring *ring     = GT10RingCreate(1u << 16, 6);
    GT10USBDevice *dev = ring ? Acquire() : NULL;
    if (dev == NULL || EnableAndReport(dev) != GT10_OK) {
        if (dev) GT10USBDeviceFree(dev);
        fclose(wav);
        return 1;
    }

    GT10StreamConfig cfg = {.capture = true, .captureRing = ring};
    bool abandoned       = false;
    GT10Stream *stream   = GT10StreamStart(dev, &cfg, &abandoned);
    if (stream == NULL) {
        printf("engine did not start\n");
        // An abandoned start left transfers with the kernel. Restoring alt 0
        // or closing the interfaces under one is what panics the machine.
        if (abandoned) {
            printf("the engine did not settle, leaving the interfaces alone\n");
            fclose(wav);
            return 1;
        }
        Disable(dev);
        GT10USBDeviceFree(dev);
        fclose(wav);
        return 1;
    }
    printf("capturing %d s into %s, play the guitar now\n", seconds, path);

    // The file is written here, never on the engine thread.
    int64_t readPos    = 0;
    uint64_t dataBytes = 0;
    bool fileOk        = true;
    GT10StreamStats st;
    for (int tick = 0; tick < seconds * 100; tick++) {
        SleepMs(10);
        fileOk &= Drain(ring, &readPos, GT10StreamCapturePosition(stream), wav, &dataBytes);
        if (tick % 100 == 99) {
            GT10StreamGetStats(stream, &st);
            PrintStats(&st, ring);
            if (!st.running) break;
        }
    }

    int rc = Finish(dev, &stream, &st);
    fileOk &= Drain(ring, &readPos, st.capturePosition, wav, &dataBytes);
    PrintStats(&st, ring);
    fileOk &= fseek(wav, 0, SEEK_SET) == 0;
    WavHeader(wav, (uint32_t)dataBytes);
    fileOk &= ferror(wav) == 0;
    fileOk &= fclose(wav) == 0;
    printf("wrote %llu frames to %s%s\n", (unsigned long long)(dataBytes / 6), path,
           fileOk ? "" : " (FILE ERROR)");
    const uint64_t dropped = GT10RingDropped(ring);
    GT10RingDestroy(ring);

    const bool clean = st.firstError == 0 && st.lostPackets == 0 && st.lostFrameStatus == 0 &&
                       st.outOfOrder == 0 && st.lateSubmits == 0 && st.capturedFrames > 0 &&
                       dropped == 0 && fileOk && dataBytes == (uint64_t)st.capturePosition * 6;
    printf("capture %s\n", clean ? "CLEAN" : "NOT clean");
    return rc | (clean ? 0 : 1);
}

// 440 Hz at -12 dBFS, a pure function of the sample position.
static void RenderTone(void *ctx, int64_t position, uint8_t *dst, uint32_t frames) {
    const double amplitude = *(double *)ctx * 8388607.0;
    for (uint32_t i = 0; i < frames; i++) {
        const double phase = 2.0 * M_PI * 440.0 * (double)((position + i) % 44100) / 44100.0;
        const int32_t v    = (int32_t)lrint(amplitude * sin(phase));
        for (int c = 0; c < 2; c++) {
            dst[i * 6 + c * 3]     = (uint8_t)v;
            dst[i * 6 + c * 3 + 1] = (uint8_t)(v >> 8);
            dst[i * 6 + c * 3 + 2] = (uint8_t)(v >> 16);
        }
    }
}

static int RunPlay(int seconds, bool once) {
    GT10USBDevice *dev = Acquire();
    if (dev == NULL || EnableAndReport(dev) != GT10_OK) {
        if (dev) GT10USBDeviceFree(dev);
        return 1;
    }
    static double amplitude = 0.25;
    if (once) amplitude = 0.05;
    GT10StreamConfig cfg = {
        .render = RenderTone, .renderContext = &amplitude, .playbackTransfers = once ? 1 : 0};
    bool abandoned     = false;
    GT10Stream *stream = GT10StreamStart(dev, &cfg, &abandoned);
    if (stream == NULL) {
        printf("engine did not start\n");
        if (abandoned) {
            printf("the engine did not settle, leaving the interfaces alone\n");
            return 1;
        }
        Disable(dev);
        GT10USBDeviceFree(dev);
        return 1;
    }
    printf(once ? "one playback transfer\n"
                : "playing 440 Hz for %d s, listen on the pedal output\n",
           seconds);

    GT10StreamStats st;
    const int ticks = once ? 200 : seconds * 100;
    for (int tick = 0; tick < ticks; tick++) {
        SleepMs(10);
        GT10StreamGetStats(stream, &st);
        if (once && (st.playbackTransfers > 0 || !st.running)) break;
        if (!once && tick % 100 == 99) PrintStats(&st, NULL);
        if (!st.running) break;
    }

    int rc = Finish(dev, &stream, &st);
    PrintStats(&st, NULL);
    for (int i = 0; i < 4; i++) {
        printf("  last playback frame %d: status 0x%08x req %u act %u\n", i,
               (uint32_t)st.lastPlaybackStatus[i], st.lastPlaybackReq[i], st.lastPlaybackAct[i]);
    }
    const bool clean =
        st.firstError == 0 && st.playbackTransfers > 0 && st.playbackFrameErrors == 0;
    printf("playback %s\n", clean ? "CLEAN" : "NOT clean");
    return rc | (clean ? 0 : 1);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1 && strcmp(argv[1], "--monitor") == 0) {
        return RunMonitor(argc > 2 ? atoi(argv[2]) : 30);
    }
    if (argc > 1 && strcmp(argv[1], "--pipes") == 0) return RunPipes();
    if (argc > 1 && strcmp(argv[1], "--read") == 0) return RunRead(argc > 2 ? argv[2] : NULL);
    if (argc > 3 && strcmp(argv[1], "--capture") == 0) return RunCapture(atoi(argv[2]), argv[3]);
    if (argc > 1 && strcmp(argv[1], "--play-once") == 0) return RunPlay(0, true);
    if (argc > 2 && strcmp(argv[1], "--play") == 0) return RunPlay(atoi(argv[2]), false);

    GT10USBDevice *dev = Acquire();
    if (dev == NULL) return 1;
    GT10USBDeviceFree(dev);
    return 0;
}
