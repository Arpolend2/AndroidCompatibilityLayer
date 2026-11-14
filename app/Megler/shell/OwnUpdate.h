// Megler's own update, as its screens tell of it (ui/Ota.cpp, which downloads it from GitHub; tested by
// core/tests/mg_shell_windows.cpp): how fast its download comes, how long it has left, and the line that says both.
//
// A newer Megler taken in the dialog that offered it was downloaded with nothing said of it but in a card of the Library:
// what had come of how much, no speed, and nothing on another tab (the owner's word, 2026-11-14). The download's dialog
// and the Library's card say this line now.
//
// And the way round a download that is too slow, the file fetched with the browser by its direct link and put into
// Pictures\Megler (shell/AppPackage.h, installCopiedUpdate): which names there are Megler's packages for one Windows and
// processor, whether a file ends as a whole package does, how the folder is named to its owner, and the text that says
// what to do.
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

/// The version in the name of a package of Megler for the Windows and processor `suffix` says ("ARM", "x64", "RT_ARM"):
/// "0.1.0.700" of "Megler_0.1.0.700_ARM.appx" (or .msix), and of what a browser names a second download of it,
/// "Megler_0.1.0.700_ARM (1).appx". Empty for any other name: another suffix's ("Megler_0.1.0.700_RT_ARM.appx" is not
/// ARM's, nor "..._ARM64.appx"), a download that is not over (".appx.part", ".crdownload"), a version that is not four
/// numbers; and for no suffix.
inline std::string versionInName(const std::string& name, const std::string& suffix) {
    const std::string head = "Megler_";
    if (suffix.empty() || name.compare(0, head.size(), head) != 0) return {};
    size_t at = head.size();
    for (int part = 0; part < 4; part++) {
        const size_t from = at;
        while (at < name.size() && name[at] >= '0' && name[at] <= '9') at++;
        if (at == from || at - from > 9) return {};
        if (part == 3) break;
        if (at >= name.size() || name[at] != '.') return {};
        at++;
    }
    const std::string version = name.substr(head.size(), at - head.size());
    if (name.compare(at, suffix.size() + 1, "_" + suffix) != 0) return {};
    at += suffix.size() + 1;
    // " (1)", "(2)": a browser's mark on a file saved beside one of the same name.
    size_t mark = at;
    if (mark < name.size() && name[mark] == ' ') mark++;
    if (mark < name.size() && name[mark] == '(') {
        size_t digit = mark + 1;
        while (digit < name.size() && name[digit] >= '0' && name[digit] <= '9') digit++;
        if (digit > mark + 1 && digit < name.size() && name[digit] == ')') at = digit + 1;
    }
    const std::string ending = name.substr(at);
    return ending == ".appx" || ending == ".msix" ? version : std::string();
}

/// Whether a file whose last `size` bytes are `tail` ends as a zip archive does, and so as a whole package: with the
/// archive's end record ("PK\5\6", 22 bytes and its comment, which reaches the file's end). A file still being copied --
/// its size set first, its end not written yet -- or cut short does not.
inline bool endsAsZip(const unsigned char* tail, size_t size) {
    if (!tail || size < 22) return false;
    for (size_t at = size - 22;; at--) {
        if (tail[at] == 'P' && tail[at + 1] == 'K' && tail[at + 2] == 5 && tail[at + 3] == 6 &&
            at + 22 + (size_t(tail[at + 20]) | size_t(tail[at + 21]) << 8) == size)
            return true;
        if (!at) return false;
    }
}

/// Pictures\Megler at `path`, as its owner finds it: on a phone as the phone's File Explorer shows the way to it ("This
/// device", or "SD card" for a folder that is not on C:), on a computer the path itself.
inline std::string folderPlace(const std::string& path, bool phone) {
    if (!phone) return path.empty() ? std::string("the folder Megler in Pictures") : path;
    const bool card = path.size() >= 2 && path[1] == ':' && path[0] != 'C' && path[0] != 'c';
    return std::string(card ? "SD card" : "This device") + " › Pictures › Megler";
}

/// What the dialog of a newer Megler fetched with the browser says (ui/Ota.cpp; its buttons are Download and Copy Link):
/// the package `file` of `size` bytes, its direct link `url` on a line of its own, and what to do with the file -- it goes
/// to `place` (folderPlace), where Megler finds it.
inline std::string browserText(const std::string& file, const std::string& url, uint64_t size, const std::string& place, bool phone) {
    std::string text = file + (size ? " (" + formattedByteCount(size) + ")" : std::string()) + " by its direct link:\n" + url + "\n\n";
    text += "1. Download opens the link in the browser, which saves the file to Downloads. Copy Link is for another browser, a download "
            "manager or a computer.\n";
    text += phone ? "2. In File Explorer, move the file from Downloads to " + place + ": hold it, then Move to.\n"
                  : "2. Move the file from Downloads to " + place + ", or save it there at once.\n";
    text += "3. Come back to Megler, or open it again: it finds the file there and has Windows install it. Megler closes meanwhile: open "
            "it again after that.\n\n";
    if (phone) text += "Downloaded on a computer, the file goes to the phone's Pictures\\Megler over USB. ";
    text += "Megler looks there for two hours after Download or Copy Link, and whenever it starts; it removes the file once that Megler is "
            "installed.";
    return text;
}

} // namespace megler::app::ownUpdate
