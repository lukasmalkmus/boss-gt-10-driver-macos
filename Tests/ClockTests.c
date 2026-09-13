#include "../Sources/GT10Clock.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

enum { kPeriod = 16384 };
static const double kNominal = 1000000.0;  // ticks per bus frame, 1 tick = 1 ns

static void TestMapping(void) {
    GT10Clock c;
    GT10ClockInit(&c, kNominal, kPeriod);
    CHECK(GT10ClockTicksPerFrame(&c) == kNominal);

    // Anchored by the first observation, nominal until the baseline is long.
    GT10ClockObserve(&c, 5000, 900000000);
    CHECK(GT10ClockHostOfFrame(&c, 5000) == 900000000);
    CHECK(GT10ClockHostOfFrame(&c, 5010) == 910000000);
    CHECK(GT10ClockHostOfFrame(&c, 4990) == 890000000);
    GT10ClockObserve(&c, 5999, 900000000ULL + 999ULL * 1000100ULL);
    CHECK(GT10ClockTicksPerFrame(&c) == kNominal);

    // Past the minimum baseline the measured rate replaces the nominal one.
    GT10ClockObserve(&c, 7000, 900000000ULL + 2000ULL * 1000100ULL);
    CHECK(fabs(GT10ClockTicksPerFrame(&c) - 1000100.0) < 1e-6);
    CHECK(GT10ClockHostOfFrame(&c, 6000) == 900000000ULL + 1000ULL * 1000100ULL);

    // An older observation never shortens the baseline.
    GT10ClockObserve(&c, 6500, 1);
    CHECK(fabs(GT10ClockTicksPerFrame(&c) - 1000100.0) < 1e-6);

    // Jitter on one late observation moves the rate by jitter / baseline only.
    GT10ClockObserve(&c, 105000, 900000000ULL + 100000ULL * 1000100ULL + 200000ULL);
    CHECK(fabs(GT10ClockTicksPerFrame(&c) - 1000100.0) < 3.0);
}

static void TestZeroTimeStamps(void) {
    GT10Clock c;
    GT10ClockInit(&c, kNominal, kPeriod);
    int64_t zs  = -1;
    uint64_t zh = 0;

    // Nothing is published before the mapping exists.
    CHECK(!GT10ClockPacket(&c, 100, 44, &zs, &zh));
    GT10ClockInit(&c, kNominal, kPeriod);
    GT10ClockObserve(&c, 100, 50000000);

    // An empty first packet publishes nothing, so sample 0 still lies at the
    // start of the first packet that carries audio.
    CHECK(!GT10ClockPacket(&c, 99, 0, &zs, &zh) && c.samples == 0);
    CHECK(GT10ClockPacket(&c, 100, 44, &zs, &zh) && zs == 0 && zh == 50000000);

    // An empty packet publishes nothing and costs no samples.
    CHECK(!GT10ClockPacket(&c, 101, 0, &zs, &zh) && c.samples == 44);

    // Walk to the packet holding sample 16384 and check its interpolation.
    int64_t published = 0;
    uint64_t frame    = 102;
    uint64_t hostAt   = 0;
    int64_t sampleAt  = 0;
    while (c.samples < 2 * kPeriod + 100) {
        const uint32_t n     = (frame % 10 == 0) ? 45 : 44;
        const int64_t before = c.samples;
        if (GT10ClockPacket(&c, frame, n, &zs, &zh)) {
            published++;
            if (zs == kPeriod) {
                hostAt   = zh;
                sampleAt = before;
                CHECK(before <= kPeriod && kPeriod < before + n);
                const uint64_t want = 50000000 + (frame - 100) * 1000000 +
                                      (uint64_t)((double)(kPeriod - before) / n * 1000000.0);
                CHECK(zh == want);
            }
        }
        frame++;
    }
    CHECK(published == 2);
    CHECK(sampleAt > 0 && hostAt > 50000000);
    CHECK(c.nextZero == 3 * kPeriod);
}

static void TestLateAnchor(void) {
    GT10Clock c;
    GT10ClockInit(&c, kNominal, kPeriod);
    int64_t zs  = -1;
    uint64_t zh = 0;
    // Packets before the anchor count samples but publish nothing.
    for (int i = 0; i < 400; i++)
        CHECK(!GT10ClockPacket(&c, (uint64_t)i, 44, &zs, &zh));
    CHECK(c.samples == 17600);
    GT10ClockObserve(&c, 400, 1000000000);
    // The boundaries at 0 and 16384 are behind. The next one is 32768.
    int64_t first = -1;
    for (uint64_t f = 400; f < 800 && first < 0; f++) {
        const int64_t before = c.samples;
        if (GT10ClockPacket(&c, f, 44, &zs, &zh)) {
            first = zs;
            CHECK(before <= zs && zs < before + 44);
            CHECK(zh >= 1000000000);
        }
    }
    CHECK(first == 2 * kPeriod);
}

static void TestPublish(void) {
    GT10TimeStamp *ts = GT10TimeStampCreate();
    int64_t s         = -1;
    uint64_t h        = 0;
    CHECK(!GT10TimeStampRead(ts, &s, &h));
    GT10TimeStampPublish(ts, 16384, 777);
    CHECK(GT10TimeStampRead(ts, &s, &h) && s == 16384 && h == 777);
    // Repeated reads return the same pair until the next publication.
    CHECK(GT10TimeStampRead(ts, &s, &h) && s == 16384 && h == 777);
    GT10TimeStampPublish(ts, 32768, 999);
    CHECK(GT10TimeStampRead(ts, &s, &h) && s == 32768 && h == 999);

    // A clear removes both slots, so no older timestamp survives it.
    GT10TimeStampClear(ts);
    CHECK(!GT10TimeStampRead(ts, &s, &h));
    GT10TimeStampPublish(ts, 0, 5);
    CHECK(GT10TimeStampRead(ts, &s, &h) && s == 0 && h == 5);
    GT10TimeStampDestroy(ts);
}

// A writer publishes pairs that encode one counter twice. Every pair a reader
// gets must be consistent. When publications are spaced the way capture spaces
// them, no read may fail and none may go back more than one publication.
typedef struct {
    GT10TimeStamp *ts;
    _Atomic bool done;
    uint64_t publications;
    useconds_t interval;
} Shared;

static void *Publisher(void *arg) {
    Shared *sh = arg;
    for (uint64_t i = 1; i <= sh->publications; i++) {
        GT10TimeStampPublish(sh->ts, (int64_t)i, i * 3);
        if (sh->interval) usleep(sh->interval);
    }
    atomic_store(&sh->done, true);
    return NULL;
}

static void RunConcurrent(uint64_t publications, useconds_t interval, long *bad, long *failed,
                          long *regressions, long *ok) {
    Shared sh = {GT10TimeStampCreate(), false, publications, interval};
    GT10TimeStampPublish(sh.ts, 0, 0);
    pthread_t t;
    pthread_create(&t, NULL, Publisher, &sh);
    int64_t last = 0;
    *bad = *failed = *regressions = *ok = 0;
    while (!atomic_load(&sh.done)) {
        int64_t s;
        uint64_t h;
        if (!GT10TimeStampRead(sh.ts, &s, &h)) {
            (*failed)++;
            continue;
        }
        (*ok)++;
        if (h != (uint64_t)s * 3) (*bad)++;
        if (s + 1 < last) (*regressions)++;
        if (s > last) last = s;
    }
    pthread_join(t, NULL);
    GT10TimeStampDestroy(sh.ts);
}

static void TestConcurrentPublish(void) {
    long bad, failed, regressions, ok;
    RunConcurrent(3000000, 0, &bad, &failed, &regressions, &ok);
    CHECK(bad == 0 && ok > 0);

    RunConcurrent(2000, 500, &bad, &failed, &regressions, &ok);
    CHECK(bad == 0 && ok > 0);
    CHECK(failed == 0);
    CHECK(regressions == 0);
}

int main(void) {
    TestMapping();
    TestZeroTimeStamps();
    TestLateAnchor();
    TestPublish();
    TestConcurrentPublish();
    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
