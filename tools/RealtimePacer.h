#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

// Diagnostic caller clock, independent of the engine. Strict mode preserves the
// fixed wall-clock timeline, including catch-up after a late caller. The optional
// rebased mode discards whole-block caller stalls and must not qualify a real-time
// result once any time has been discarded.
class RealtimePacer {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    RealtimePacer(int sampleRate, Time start, bool rebase = false)
        : rate(sampleRate), origin(start), anchor(start), allowRebase(rebase) {}

    Time beginBlock(int64_t offset, int count, Time now) {
        const auto ideal = anchor + duration(offset);
        const auto late = std::chrono::duration_cast<std::chrono::microseconds>(now - ideal);
        lastLate = late.count();
        maxLate = std::max(maxLate, lastLate);
        if (allowRebase && late >= duration(count)) {
            anchor += now - ideal;
            rebased += std::chrono::duration_cast<std::chrono::microseconds>(now - ideal).count();
            ++rebaseCount;
        }
        return anchor + duration(offset + count);
    }

    int64_t maxLatenessMicros() const { return maxLate; }
    int64_t lastLatenessMicros() const { return lastLate; }
    int64_t rebasedMicros() const { return rebased; }
    int rebases() const { return rebaseCount; }
    bool qualifies() const { return rebaseCount == 0; }
    double elapsedRatio(int64_t samples, Time end) const {
        return std::chrono::duration<double>(end - origin).count() * rate / samples;
    }

private:
    std::chrono::microseconds duration(int64_t samples) const {
        return std::chrono::microseconds(samples * 1000000 / rate);
    }
    int rate;
    Time origin, anchor;
    bool allowRebase;
    int64_t maxLate = 0, lastLate = 0, rebased = 0;
    int rebaseCount = 0;
};
