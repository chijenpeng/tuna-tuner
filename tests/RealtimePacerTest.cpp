#include "../tools/RealtimePacer.h"

#include <iostream>

int main() {
    using namespace std::chrono;
    const RealtimePacer::Time start {};
    constexpr int rate = 48000, block = 256;
    const auto blockTime = microseconds(block * 1000000 / rate);
    const auto stamp = [&](int sample) { return start + microseconds(int64_t(sample) * 1000000 / rate); };
    RealtimePacer strict(rate, start), rebased(rate, start, true);
    // Ordinary wake jitter must not accumulate extra time or move the timeline.
    for (int offset = 0; offset < rate; offset += block) {
        const auto wake = stamp(offset) + microseconds(500);
        const auto expected = stamp(offset + block);
        if (strict.beginBlock(offset, block, wake) != expected
            || rebased.beginBlock(offset, block, wake) != expected) return 1;
    }
    const auto stalled = stamp(rate) + milliseconds(200);
    const auto strictNext = strict.beginBlock(rate, block, stalled);
    const auto rebasedNext = rebased.beginBlock(rate, block, stalled);
    if (strictNext >= stalled || rebasedNext != stalled + blockTime
        || rebased.rebases() != 1 || rebased.rebasedMicros() != 200000
        || rebased.qualifies() || !strict.qualifies()) return 1;
    // A slower-than-budget caller cannot gain an unqualified pass by rebasing.
    RealtimePacer slow(rate, start, true);
    for (int offset = 0; offset < rate; offset += block)
        slow.beginBlock(offset, block, stamp(offset * 2));
    if (slow.qualifies() || slow.rebasedMicros() < 900000
        || slow.elapsedRatio(rate, start + seconds(2)) != 2.0) return 1;
    std::cout << "PASS: nominal cadence, late-caller diagnostics, no rebased timing qualification\n";
}
