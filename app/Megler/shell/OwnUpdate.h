// Megler's own update, as its screens tell of it (ui/Ota.cpp, which downloads it from GitHub; tested by
// core/tests/mg_shell_windows.cpp): how fast its download comes, how long it has left, and the line that says both.
//
// A newer Megler taken in the dialog that offered it was downloaded with nothing said of it but in a card of the Library:
// what had come of how much, no speed, and nothing on another tab (the owner's word, 2026-11-14). The download's dialog
// and the Library's card say this line now.
#pragma once

#include "shell/Strings.h"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace megler::app::ownUpdate {

/// How fast a download comes: the mean over its last five seconds, of what had come at moments of one clock (milliseconds).
class Speed {
public:
    /// What is older than this is not this moment's speed; nothing coming for as long is a download that stands.
    static constexpr uint64_t windowMs = 5000;
    /// Moments closer than this are one (a fast download's chunks come hundreds to the second).
    static constexpr uint64_t apartMs = 250;
    /// The speed is said once this much time is measured.
    static constexpr uint64_t enoughMs = 1000;

    void reset() { samples_.clear(); }

    /// `bytes` had come in all at `ms`.
    void add(uint64_t ms, uint64_t bytes) {
        if (!samples_.empty() && ms < samples_.back().first) ms = samples_.back().first;
        // After a pause the speed is measured anew: the pause is not how fast it comes now.
        if (!samples_.empty() && ms - samples_.back().first >= windowMs) samples_.clear();
        if (samples_.size() >= 2 && ms - samples_[samples_.size() - 2].first < apartMs) {
            samples_.back() = {ms, bytes};
        } else {
            samples_.emplace_back(ms, bytes);
        }
        while (samples_.size() > 2 && ms - samples_[1].first >= windowMs) samples_.pop_front();
    }

    /// Bytes a second at `ms`: 0 where nothing has come for five seconds, less than 0 while it is not known yet.
    double at(uint64_t ms) const {
        if (samples_.empty()) return -1;
        if (ms >= samples_.back().first && ms - samples_.back().first >= windowMs) return 0;
        const uint64_t span = samples_.back().first - samples_.front().first;
        if (samples_.size() < 2 || span < enoughMs) return -1;
        return double(samples_.back().second - samples_.front().second) * 1000.0 / double(span);
    }

private:
    std::deque<std::pair<uint64_t, uint64_t>> samples_; // (milliseconds, bytes come by then), oldest first
};

/// The time `seconds` are, as what a download has left is told: "less than a minute", "about 4 minutes", "about 2 hours
/// 5 minutes".
inline std::string timeLeft(double seconds) {
    if (seconds < 60) return "less than a minute";
    if (seconds >= 24 * 3600.0) return "more than a day";
    const unsigned minutes = unsigned((seconds + 30) / 60), hours = minutes / 60, rest = minutes % 60;
    if (!hours) return minutes == 1 ? "about a minute" : format("about %u minutes", minutes);
    std::string out = hours == 1 ? "about an hour" : format("about %u hours", hours);
    if (rest) out += rest == 1 ? " 1 minute" : format(" %u minutes", rest);
    return out;
}

/// A download in a line: "12.3 MB of 371 MB (3%), 1.25 MB/s, about 5 minutes left". `total` 0: not known; `bytesPerSecond`
/// as Speed::at gives it.
inline std::string progressLine(uint64_t got, uint64_t total, double bytesPerSecond) {
    std::string line = formattedByteCount(got);
    if (total) line += " of " + formattedByteCount(total) + format(" (%u%%)", unsigned(got >= total ? 100 : got * 100 / total));
    if (bytesPerSecond >= 1) {
        line += ", " + formattedByteCount(uint64_t(bytesPerSecond)) + "/s";
        if (total > got) line += ", " + timeLeft(double(total - got) / bytesPerSecond) + " left";
    } else if (bytesPerSecond >= 0) {
        line += ", nothing coming now";
    }
    return line;
}

} // namespace megler::app::ownUpdate
