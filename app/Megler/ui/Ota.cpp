#include "pch.h"

#include "ui/Ota.h"

#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/OwnUpdate.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Security.Cryptography.h>

#include <ctime>
#include <optional>

namespace megler::app::ui::ota {

using namespace winrt;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Web::Http;

namespace {

constexpr const wchar_t* releasesUrl = L"https://api.github.com/repos/Computershik73/megler-w10m/releases?per_page=30";
/// GitHub is asked at most this often at start (its API answers 60 asks an hour from one address without a sign-in).
constexpr int64_t askEverySeconds = 6 * 3600;
/// Room for Windows to install the package once it is here, besides the package itself.
constexpr uint64_t installRoom = uint64_t(1200) << 20;
/// A newer Megler fetched with the browser: Pictures\Megler is looked in for its file this long from Download or Copy
/// Link, and this often.
constexpr uint64_t awaitMs = uint64_t(2) * 3600 * 1000, lookEveryMs = 10 * 1000;
/// What a release's assets' direct links begin with.
constexpr const char* downloadsUrl = "https://github.com/Computershik73/megler-w10m/releases/download/";

/// The newest Megler for this one on GitHub, newer than it.
struct Found {
    std::string version, tag, file, url, certificateUrl, sha256, notes;
    uint64_t size = 0;
};

enum class Step { idle, asking, found, offering, downloading, installing };

std::mutex gMutex;
Step gStep = Step::idle;
std::optional<Found> gFound;
std::string gSummary;           // Settings' line
std::string gDeclined;          // the version "Later" was said to, this start
bool gAskNow = false;           // Check Now: ask at the next poll, and offer even what "Later" was said to
bool gForced = false;           // ... the ask under way is one of those
bool gTidied = false;
std::atomic<uint64_t> gGot{0}, gTotal{0};
std::atomic<bool> gStop{false}; // the dialog's Stop: the download under way ends at its worker's next look
bool gGaveWay = false;          // ... ended so for the browser's download of it (awaitFile), not by Stop
std::mutex gSpeedMutex;
ownUpdate::Speed gSpeed;        // how fast the package comes (gSpeedMutex)

/// What went wrong with a newer Megler, said in a dialog at the next poll that has none up: it went into Settings' line
/// and the log alone, and a download that failed looked like one that never began.
struct Failure {
    std::string title, text;
    bool download = false; // its download, which can be tried again
    Found f;
};
std::optional<Failure> gFailure;

/// A newer Megler fetched with the browser (In Browser): how is shown, and from Download or Copy Link on its file is
/// looked for in Pictures\Megler (gMutex).
struct Awaited {
    Found f;
    std::wstring folder;          // Pictures\Megler, as the dialog names it
    bool asked = false;           // Download or Copy Link was pressed: the file is looked for,
    uint64_t until = 0, next = 0; // ... until then, and next then (GetTickCount64)
    bool looking = false;         // a look is under way on a worker thread
    bool show = false;            // its dialog is to be shown at the next poll that has none up
    std::string handed;           // the file Windows was asked to install, by its name, size and time: asked once for each
    std::string state;            // where the file stands, as the dialog and the Library's card say it
};
std::optional<Awaited> gAwaited;

std::wstring updateFolder() { return platform::localStateDirectory() + L"\\update"; }
std::wstring askedFile() { return platform::localStateDirectory() + L"\\ota-asked.txt"; }
std::wstring noteFile() { return platform::localStateDirectory() + L"\\update-browser.txt"; }

std::string userAgent() { return "Megler/" + appPackage::meglerVersion() + " (" + platform::deviceFamily() + ")"; }

/// A client that keeps nothing it fetches in Megler's internet cache (its package's AC\INetCache): HttpClient keeps what it
/// can there, and the 373 MB package of 639 that the OTA downloaded stayed in that cache on a Lumia 650 (package 663).
HttpClient uncachedClient() {
    using namespace winrt::Windows::Web::Http::Filters;
    HttpBaseProtocolFilter filter;
    filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
    filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::MostRecent);
    return HttpClient(filter);
}

std::string clock() {
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_s(&local, &now);
    return format("%02d:%02d", local.tm_hour, local.tm_min);
}

void setSummary(const std::string& line) {
    std::lock_guard<std::mutex> lock(gMutex);
    gSummary = line;
}

/// What this Megler is on, as its texts name it.
const char* device() { return platform::deviceFamily() == "Windows.Mobile" ? "phone" : platform::isXbox() ? "console" : "computer"; }

/// Windows's word on an error without the line's end and the full stop it comes with.
std::string said(hresult_error const& e) {
    std::string text = narrow(e.message());
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '.')) text.pop_back();
    return text;
}

/// When GitHub was last asked at start (seconds since 1970), 0 never.
int64_t lastAsked() {
    std::string text;
    return files::read(askedFile(), text) ? _atoi64(text.c_str()) : 0;
}

/// A release's tag, "v0.1.0.637", as its version, "0.1.0.637"; empty when it is not four numbers.
std::string versionOf(const std::string& tag) {
    const std::string v = !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
    unsigned n[4] = {};
    char extra = 0;
    return sscanf_s(v.c_str(), "%u.%u.%u.%u%c", &n[0], &n[1], &n[2], &n[3], &extra, 1) == 4 ? v : std::string();
}

/// Whether a newer Megler can be fetched with the browser: not on an Xbox, where no file gets to Pictures\Megler by hand
/// and the console's Device Portal wants Megler.cer with the package.
bool browserRoute() { return !platform::isXbox(); }

/// Settings' line while a file fetched with the browser is waited for.
std::string waitingLine(const std::string& version) { return "Megler " + version + ": waiting for its file in Pictures\\Megler"; }

/// LocalState\update-browser.txt: the Megler fetched with the browser -- its version, its package's name, size and direct
/// link, a line each --, and when Download or Copy Link was pressed (seconds since 1970; 0 not yet). It outlives this
/// start: the next takes the look up, and removes the file from Pictures\Megler once that Megler is installed.
struct Note {
    Found f;
    int64_t asked = 0;
};

std::optional<Note> readNote() {
    std::string text;
    if (!files::read(noteFile(), text)) return std::nullopt;
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    // (Only what Megler itself would have written: a version, this Megler's package of it, a link to a release's asset.)
    if (lines.size() < 5 || lines[0].empty() || versionOf(lines[0]) != lines[0] ||
        ownUpdate::versionInName(lines[1], packageSuffix()) != lines[0] || lines[3].rfind(downloadsUrl, 0) != 0)
        return std::nullopt;
    Note note;
    note.f.version = lines[0];
    note.f.tag = "v" + lines[0];
    note.f.file = lines[1];
    note.f.size = uint64_t(_atoi64(lines[2].c_str()));
    note.f.url = lines[3];
    note.asked = _atoi64(lines[4].c_str());
    return note;
}

void writeNote(const Found& f, int64_t asked) {
    files::write(noteFile(), f.version + "\r\n" + f.file + "\r\n" + std::to_string(f.size) + "\r\n" + f.url + "\r\n" + std::to_string(asked) + "\r\n");
}

/// The newest release (not a draft, not a pre-release) with a package for this Megler, from GitHub's list of them: its
/// asset named Megler_<version>_<suffix>.appx (or .msix), and Megler.cer beside it. Nothing where none has one.
std::optional<Found> newestFor(const std::string& json, const std::string& suffix, std::string& error) {
    JsonArray releases{nullptr};
    if (!JsonArray::TryParse(widen(json), releases)) {
        error = "GitHub's answer could not be read";
        return std::nullopt;
    }
    std::optional<Found> best;
    for (const IJsonValue& value : releases) {
        if (value.ValueType() != JsonValueType::Object) continue;
        const JsonObject release = value.GetObject();
        if (release.GetNamedBoolean(L"draft", false) || release.GetNamedBoolean(L"prerelease", false)) continue;
        Found f;
        f.tag = narrow(release.GetNamedString(L"tag_name", L""));
        f.version = versionOf(f.tag);
        if (f.version.empty() || (best && !appPackage::olderVersion(best->version, f.version))) continue;
        f.notes = narrow(release.GetNamedString(L"body", L""));
        const std::string appx = "Megler_" + f.version + "_" + suffix + ".appx", msix = "Megler_" + f.version + "_" + suffix + ".msix";
        for (const IJsonValue& item : release.GetNamedArray(L"assets", JsonArray())) {
            if (item.ValueType() != JsonValueType::Object) continue;
            const JsonObject asset = item.GetObject();
            const std::string name = narrow(asset.GetNamedString(L"name", L"")), url = narrow(asset.GetNamedString(L"browser_download_url", L""));
            if (name == "Megler.cer") f.certificateUrl = url;
            if (name != appx && name != msix) continue;
            f.file = name;
            f.url = url;
            f.size = uint64_t(asset.GetNamedNumber(L"size", 0));
            // GitHub's own SHA-256 of what was uploaded ("sha256:<hex>"); a release of before it said one has none.
            const IJsonValue digest = asset.GetNamedValue(L"digest", JsonValue::CreateNullValue());
            if (digest.ValueType() == JsonValueType::String) {
                const std::string d = narrow(digest.GetString());
                if (d.rfind("sha256:", 0) == 0) f.sha256 = d.substr(7);
            }
        }
        if (!f.file.empty() && !f.url.empty()) best = f;
    }
    return best;
}

/// What the release says is new: its "What's new" section, bold and list marks taken off, at most about 700 characters.
std::string whatsNew(const std::string& notes) {
    size_t at = notes.find("## What's new");
    if (at == std::string::npos) return {};
    at = notes.find('\n', at);
    if (at == std::string::npos) return {};
    size_t end = notes.find("\n## ", at);
    std::string part = notes.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1), out;
    for (size_t i = 0; i < part.size(); i++) {
        if (part.compare(i, 2, "**") == 0) {
            i++;
            continue;
        }
        if (part[i] == '`' || part[i] == '\r') continue;
        out += part[i];
    }
    // A sub-heading ("### Fixed") a line of its own, as a list item is, its marks taken off and a colon after it: the dialog
    // said "### Fixed" as it is written.
    std::string headed;
    for (size_t i = 0; i < out.size();) {
        size_t end = out.find('\n', i);
        if (end == std::string::npos) end = out.size();
        std::string line = out.substr(i, end - i);
        const size_t mark = line.find_first_not_of(' ');
        if (mark != std::string::npos && line[mark] == '#') {
            const size_t words = line.find_first_not_of("# ", mark);
            line = words == std::string::npos ? std::string() : "- " + line.substr(words) + ":";
        }
        headed += line;
        if (end < out.size()) headed += '\n';
        i = end + 1;
    }
    out.swap(headed);
    // Lines of a list item joined, the items one to a line.
    std::string joined;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == '\n') {
            size_t next = i + 1;
            while (next < out.size() && out[next] == ' ') next++;
            if (next < out.size() && out[next] == '-') {
                joined += "\n";
                i = next;
                continue;
            }
            joined += ' ';
            i = next - 1;
            continue;
        }
        joined += out[i];
    }
    while (!joined.empty() && (joined.front() == ' ' || joined.front() == '\n' || joined.front() == '-')) joined.erase(joined.begin());
    if (joined.size() > 700) joined = joined.substr(0, joined.rfind(' ', 700)) + "…";
    return joined;
}

/// GitHub asked (a worker thread): what it has for this Megler, newer than it, or why it could not be asked.
void ask() {
    const std::string suffix = packageSuffix(), current = appPackage::meglerVersion();
    std::string error;
    std::optional<Found> found;
    try {
        HttpClient client = uncachedClient();
        client.DefaultRequestHeaders().UserAgent().TryParseAdd(widen(userAgent()));
        client.DefaultRequestHeaders().Accept().TryParseAdd(L"application/vnd.github+json");
        HttpResponseMessage response = client.GetAsync(Uri(releasesUrl)).get();
        if (!response.IsSuccessStatusCode()) {
            error = format("GitHub answered %d", int(response.StatusCode()));
        } else {
            found = newestFor(narrow(response.Content().ReadAsStringAsync().get()), suffix, error);
        }
    } catch (hresult_error const& e) {
        error = format("GitHub could not be reached (0x%08x %s)", unsigned(e.code()), narrow(e.message()).c_str());
    }
    files::write(askedFile(), std::to_string(int64_t(std::time(nullptr))));
    // flags\ota-again (a test's): the newest is offered though it is not newer than this Megler, so that what follows an
    // offer -- its download, its dialogs, the browser's way -- is tried without a release made for it.
    const bool newer = found && appPackage::olderVersion(current, found->version);
    const bool again = found && !newer && files::exists(platform::localStateDirectory() + L"\\flags\\ota-again");
    if (again) shellLog::write("ota: flags\\ota-again: Megler " + found->version + " is offered though this is " + current);
    std::lock_guard<std::mutex> lock(gMutex);
    if (!error.empty()) {
        shellLog::write("ota: " + error);
        gSummary = error + ", at " + clock();
        gStep = Step::idle;
        return;
    }
    if (!newer && !again) {
        shellLog::write("ota: the newest Megler on GitHub for " + suffix + " is " + (found ? found->version : std::string("none")) + "; this is " + current);
        gSummary = "Megler " + current + " is the newest for " + suffix + ", asked GitHub at " + clock();
        gStep = Step::idle;
        return;
    }
    shellLog::write(format("ota: Megler %s is on GitHub for %s (%s, %s%s); this is %s", found->version.c_str(), suffix.c_str(), found->file.c_str(),
                           formattedByteCount(found->size).c_str(), found->sha256.empty() ? ", no SHA-256 said" : "", current.c_str()));
    gSummary = "Megler " + found->version + " is on GitHub, " + formattedByteCount(found->size) + " for " + suffix;
    gFound = found;
    gStep = Step::found;
}

/// `op` waited for on this worker thread, a Stop looked for meanwhile: false where one was said (the operation cancelled).
/// (Not C++/WinRT's wait_for in a loop: each call of it sets the operation's Completed handler, which is set once.)
template <typename Async> bool waitFor(Async const& op) {
    struct Ended {
        winrt::handle event{CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_MODIFY_STATE | SYNCHRONIZE)};
    };
    auto ended = std::make_shared<Ended>();
    op.Completed([ended](auto const&, AsyncStatus) { SetEvent(ended->event.get()); });
    while (WaitForSingleObjectEx(ended->event.get(), 200, FALSE) == WAIT_TIMEOUT) {
        if (!gStop) continue;
        op.Cancel();
        WaitForSingleObjectEx(ended->event.get(), 5000, FALSE);
        return false;
    }
    return !gStop;
}

/// `got` bytes of the package have come: the screens' count and the speed's measure.
void came(uint64_t got) {
    gGot = got;
    std::lock_guard<std::mutex> lock(gSpeedMutex);
    gSpeed.add(GetTickCount64(), got);
}

/// A file downloaded into `path` (a worker thread), as it comes: its size and SHA-256 checked against GitHub's. `counted`:
/// the package itself, whose bytes the screens count (not Megler.cer after it, which set the count back to its 800 bytes).
/// False with no `error` where Stop was said. What had come of a download that failed or was stopped is not kept.
bool fetch(const std::string& url, const std::wstring& path, uint64_t size, const std::string& sha256, bool counted, std::string& error) {
    const std::wstring part = path + L".part";
    struct Leftover {
        std::wstring path;
        bool keep = false;
        ~Leftover() {
            if (!keep) files::remove(path);
        }
    } leftover{part};
    try {
        HttpClient client = uncachedClient();
        client.DefaultRequestHeaders().UserAgent().TryParseAdd(widen(userAgent()));
        auto asked = client.GetAsync(Uri(widen(url)), HttpCompletionOption::ResponseHeadersRead);
        if (!waitFor(asked)) return false;
        HttpResponseMessage response = asked.GetResults();
        if (!response.IsSuccessStatusCode()) {
            error = format("GitHub answered %d for the file", int(response.StatusCode()));
            return false;
        }
        auto opening = response.Content().ReadAsInputStreamAsync();
        if (!waitFor(opening)) return false;
        IInputStream input = opening.GetResults();
        auto hash = winrt::Windows::Security::Cryptography::Core::HashAlgorithmProvider::OpenAlgorithm(
                        winrt::Windows::Security::Cryptography::Core::HashAlgorithmNames::Sha256())
                        .CreateHash();
        uint64_t got = 0;
        if (counted) came(0);
        {
            file_handle out{CreateFile2(part.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr)};
            if (!out) {
                error = format("%s could not be made (error %lu)", narrow(part).c_str(), GetLastError());
                return false;
            }
            Buffer buffer(1u << 20);
            for (;;) {
                auto reading = input.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial);
                if (!waitFor(reading)) return false;
                IBuffer chunk = reading.GetResults();
                const uint32_t n = chunk.Length();
                if (!n) break;
                DWORD written = 0;
                if (!WriteFile(out.get(), chunk.data(), n, &written, nullptr) || written != n) {
                    error = format("the file could not be written (error %lu: is the %s full?)", GetLastError(), device());
                    return false;
                }
                hash.Append(chunk);
                got += n;
                if (counted) came(got);
            }
        }
        if (size && got != size) {
            error = format("the file came short: %s of %s", formattedByteCount(got).c_str(), formattedByteCount(size).c_str());
            return false;
        }
        const std::string sum = narrow(winrt::Windows::Security::Cryptography::CryptographicBuffer::EncodeToHexString(hash.GetValueAndReset()));
        if (!sha256.empty() && _stricmp(sum.c_str(), sha256.c_str()) != 0) {
            error = "the file's SHA-256 is " + sum + ", not GitHub's " + sha256;
            return false;
        }
        files::remove(path);
        if (!files::move(part, path)) {
            error = "the file could not be put in its place";
            return false;
        }
        leftover.keep = true;
        shellLog::write(format("ota: %s downloaded, %s, SHA-256 %s%s", narrow(path).c_str(), formattedByteCount(got).c_str(), sum.c_str(),
                               sha256.empty() ? " (GitHub said none)" : ", as GitHub says"));
        return true;
    } catch (hresult_error const& e) {
        error = format("the download broke off (0x%08x %s)", unsigned(e.code()), said(e).c_str());
        return false;
    }
}

/// The newer Megler downloaded, checked and installed (a worker thread). Windows's deployment engine closes this Megler.
void downloadAndInstall(Found f) {
    const std::wstring folder = updateFolder(), appx = folder + L"\\" + widen(f.file), certificate = folder + L"\\Megler.cer";
    std::string error;
    ULARGE_INTEGER free = {};
    files::makeDirectories(folder);
    if (GetDiskFreeSpaceExW(folder.c_str(), &free, nullptr, nullptr) && free.QuadPart < f.size + installRoom) {
        error = format("The %s has %s free: %s are needed for the file and for Windows to install it", device(),
                       formattedByteCount(free.QuadPart).c_str(), formattedByteCount(f.size + installRoom).c_str());
    } else if (!fetch(f.url, appx, f.size, f.sha256, true, error)) {
        error = "Megler " + f.version + " was not downloaded: " + error;
    } else if (!f.certificateUrl.empty() && !fetch(f.certificateUrl, certificate, 0, {}, false, error)) {
        error = "Megler.cer was not downloaded: " + error;
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gStop) {
            // Stop, or the browser asked for it instead: nothing of it is kept, and it is not offered again by itself this
            // start (Check Now offers it).
            const bool awaited = gGaveWay;
            gGaveWay = false;
            files::remove(appx);
            shellLog::write(format("ota: Megler %s: its download stopped %s, at %s of %s", f.version.c_str(),
                                   awaited ? "for the browser's" : "by the user", formattedByteCount(gGot).c_str(),
                                   formattedByteCount(f.size).c_str()));
            gSummary = awaited ? waitingLine(f.version) : "Megler " + f.version + ": its download was stopped";
            gDeclined = f.version;
            gStep = Step::idle;
            return;
        }
        if (!error.empty()) {
            shellLog::write("ota: " + error);
            gSummary = error;
            gFailure = Failure{"Megler " + f.version + " Was Not Downloaded", error + ". This Megler stays as it is.", true, f};
            gStep = Step::idle;
            return;
        }
        gStep = Step::installing;
        gSummary = "Megler " + f.version + " is being installed";
    }
    const appPackage::InstallResult result = appPackage::installOwnUpdate(appx, certificate, f.version, "downloaded from GitHub (" + f.tag + ")");
    // Refused for access (Megler has not got packageManagement): the package goes to Pictures\Megler, where the file manager
    // opens it with Windows's own installer and Megler tries it again at each start, and the Library says why and what to do.
    // Left in LocalState\update, it was out of the user's reach, and the next offer would have downloaded it again.
    if (!result.ok && result.accessDenied) appPackage::keepRefusedUpdate(appx, certificate, f.version);
    std::lock_guard<std::mutex> lock(gMutex);
    // (Installed, this Megler is closed by Windows before it gets here, as a rule.)
    gSummary = result.ok             ? "Megler " + f.version + " installed: open Megler again"
               : result.accessDenied ? appPackage::selfUpdateFailure()
                                     : "Megler " + f.version + " was not installed: " + result.said;
    if (!result.ok) gFailure = Failure{"Megler " + f.version + " Was Not Installed", gSummary, false, f};
    gStep = Step::idle;
}

/// The files of 4 MB or more in Megler's internet cache (AC\INetCache, beside LocalState) removed: the packages HttpClient kept
/// there before the OTA asked it not to (uncachedClient). Windows never let 639's go: 373 MB of a Lumia 650's last 600.
void tidyInternetCache() {
    const std::wstring local = platform::localStateDirectory();
    const size_t slash = local.find_last_of(L'\\');
    if (slash == std::wstring::npos) return;
    const std::wstring cache = local.substr(0, slash) + L"\\AC\\INetCache";
    uint64_t freed = 0;
    int removed = 0;
    for (const auto& folder : files::listFolders(cache, L"*")) {
        const std::wstring dir = cache + L"\\" + folder;
        for (const auto& name : files::list(dir, L"*")) {
            const std::wstring path = dir + L"\\" + name;
            const uint64_t size = files::size(path);
            if (size >= (uint64_t(4) << 20) && files::remove(path)) {
                freed += size;
                removed++;
            }
        }
    }
    if (removed) shellLog::write(format("ota: %d files HttpClient had kept in Megler's internet cache removed, %s", removed, formattedByteCount(freed).c_str()));
}

/// What the last start's downloads left, of this Megler or older (the folder keeps nothing once its Megler is installed),
/// and what Megler's internet cache kept of them.
void tidy() {
    tidyInternetCache();
    const std::wstring folder = updateFolder();
    const std::string current = appPackage::meglerVersion();
    for (const auto& name : files::list(folder, L"*")) {
        const std::string n = narrow(name);
        const size_t under = n.find('_', 7);
        const std::string version = n.rfind("Megler_", 0) == 0 && under != std::string::npos ? n.substr(7, under - 7) : std::string();
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".part") == 0) {
            files::remove(folder + L"\\" + name);
        } else if (!version.empty() && !appPackage::olderVersion(current, version)) {
            files::remove(folder + L"\\" + name);
            shellLog::write("ota: " + n + " removed: this Megler is " + current);
        }
    }
}

/// Whether the phone's connection is one that is paid for by the byte.
bool metered() {
    try {
        using namespace winrt::Windows::Networking::Connectivity;
        const ConnectionProfile profile = NetworkInformation::GetInternetConnectionProfile();
        if (!profile) return false;
        const NetworkCostType cost = profile.GetConnectionCost().NetworkCostType();
        return cost == NetworkCostType::Fixed || cost == NetworkCostType::Variable;
    } catch (...) {
        return false;
    }
}

/// Download or Copy Link was pressed: the file is looked for in Pictures\Megler from now on, for two hours, and Megler's
/// own download of it gives way -- two of them at once would share a phone's connection, and its memory.
void awaitFile(AppShell& shell, const Found& f) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gAwaited || gAwaited->f.version != f.version) return;
        const uint64_t now = GetTickCount64();
        gAwaited->asked = true;
        gAwaited->until = now + awaitMs;
        gAwaited->next = now + 3000;
        gDeclined = f.version;
        if (gStep == Step::downloading && gFound && gFound->version == f.version) {
            if (!gStop) {
                gGaveWay = true;
                gStop = true;
                shellLog::write("ota: Megler " + f.version + ": its download here gives way to the browser's");
            }
        } else if (gStep == Step::idle) {
            gSummary = waitingLine(f.version);
        }
    }
    shell.runInBackground([f] { writeNote(f, int64_t(std::time(nullptr))); });
}

/// How to get the newer Megler with the browser, in a dialog: its package's direct link, where the file is to be put, and
/// under them where things stand. `did`: what the press before this did.
void showBrowser(AppShell& shell, std::string did = {}) {
    Found f;
    std::wstring folder;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gAwaited) return;
        f = gAwaited->f;
        folder = gAwaited->folder;
    }
    if (shell.dialogShown()) return;
    const bool phone = platform::isMobile();
    AppShell::DialogSpec spec;
    spec.title = "Megler " + f.version + " with the Browser";
    spec.message = ownUpdate::browserText(f.file, f.url, f.size, ownUpdate::folderPlace(narrow(folder), phone), phone);
    spec.buttons = {"Close", "Copy Link", "Download"};
    spec.live = [version = f.version, did](AppShell::DialogSpec::Live& live) {
        live.bar = false;
        bool asked = false, downloading = false;
        std::string state;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            // (Installed by Megler's own download meanwhile, or not waited for any more: closed, -2.)
            if (!gAwaited || gAwaited->f.version != version || gStep == Step::installing) return false;
            asked = gAwaited->asked;
            state = gAwaited->state;
            downloading = gStep == Step::downloading && gFound && gFound->version == version && !gStop;
        }
        if (downloading) {
            const Progress now = progress();
            live.text = "Megler's own download goes on meanwhile (" + ownUpdate::progressLine(now.got, now.total, now.bytesPerSecond) +
                        "): Download and Copy Link stop it.";
        } else if (appPackage::selfUpdating()) {
            live.text = "Windows is installing it. Megler is closed when Windows takes it: open it again after that.";
        } else {
            live.text = did;
            if (asked && !state.empty()) live.text += (live.text.empty() ? "" : " ") + state + ".";
        }
        return true;
    };
    std::weak_ptr<AppShell> weak = shell.weak_from_this();
    shell.dialog(std::move(spec), [weak, f](int button, std::string) {
        auto self = weak.lock();
        if (!self) return;
        if (button == -2) {
            showProgress(*self);
            return;
        }
        if (button != 1 && button != 2) {
            // Close, Back, a tap beside it. A file looked for is looked for on, and the Library's card shows this again; one
            // never asked for is not waited for.
            std::lock_guard<std::mutex> lock(gMutex);
            if (gAwaited && gAwaited->f.version == f.version && !gAwaited->asked) gAwaited.reset();
            return;
        }
        std::string did;
        if (button == 2) {
            // Download: the link opened as the browser's own, which saves what it gives as a file.
            try {
                winrt::Windows::System::Launcher::LaunchUriAsync(Uri(widen(f.url)));
                did = "The browser was asked for the file.";
            } catch (hresult_error const& e) {
                did = format("The browser could not be asked (0x%08x %s): copy the link.", unsigned(e.code()), said(e).c_str());
            }
        } else {
            try {
                using namespace winrt::Windows::ApplicationModel::DataTransfer;
                DataPackage data;
                data.SetText(hs(f.url));
                Clipboard::SetContent(data);
                did = "The link is copied.";
            } catch (hresult_error const& e) {
                did = format("The link could not be copied (0x%08x %s).", unsigned(e.code()), said(e).c_str());
            }
        }
        shellLog::write("ota: Megler " + f.version + " with the browser: " + did);
        awaitFile(*self, f);
        showBrowser(*self, did);
    });
}

/// In Browser: how to get the newer Megler with the browser is shown. Its download here, where one is under way, goes on
/// until the browser is asked for it.
void inBrowser(AppShell& shell, const Found& f) {
    bool installing = false;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        installing = gStep == Step::installing;
        // (The offer is answered: not made again this start, unless Check Now asks.)
        if (gStep == Step::offering) {
            gStep = Step::idle;
            gDeclined = f.version;
        }
    }
    if (installing) {
        showProgress(shell);
        return;
    }
    shellLog::write("ota: Megler " + f.version + ": how to get it with the browser shown (" + f.url + ")");
    auto folder = std::make_shared<std::wstring>();
    std::weak_ptr<AppShell> weak = shell.weak_from_this();
    shell.runInBackground(
        [f, folder] {
            *folder = appPackage::copiedUpdateFolder();
            // (Written now, with no press yet: a file fetched from what this shows is removed once its Megler is installed.)
            const std::optional<Note> note = readNote();
            writeNote(f, note && note->f.version == f.version ? note->asked : 0);
        },
        [weak, f, folder] {
            auto self = weak.lock();
            if (!self) return;
            {
                std::lock_guard<std::mutex> lock(gMutex);
                if (!gAwaited || gAwaited->f.version != f.version) gAwaited = Awaited{f};
                gAwaited->folder = *folder;
                // (Another dialog came up meanwhile: this one at the next poll that has none.)
                gAwaited->show = self->dialogShown();
                if (gAwaited->show) return;
            }
            showBrowser(*self);
        });
}

/// Pictures\Megler looked in for the file awaited, on a worker thread: once it is there, whole and of GitHub's size,
/// Windows is asked to install it (once for each file: `handedBefore` is the one it was asked for).
void look(AppShell& shell, const Found& f, const std::string& handedBefore) {
    auto state = std::make_shared<std::string>();
    auto handed = std::make_shared<std::string>(handedBefore);
    shell.runInBackground(
        [f, state, handed] {
            const std::optional<appPackage::CopiedUpdate> at = appPackage::copiedUpdate(f.version);
            if (!at) {
                *state = f.file + " is not in Pictures\\Megler yet";
                return;
            }
            const std::string identity = format("%s, %llu bytes, changed %llu", at->name.c_str(), static_cast<unsigned long long>(at->size),
                                                static_cast<unsigned long long>(at->changed));
            if (identity == *handed) {
                const std::string failed = appPackage::selfUpdateFailure();
                *state = appPackage::selfUpdating() ? std::string("Windows is installing it")
                         : !failed.empty()          ? failed
                                                    : at->name + " is there, and Windows was asked to install it";
            } else if (f.size && at->size < f.size) {
                *state = at->name + " is there, " + formattedByteCount(at->size) + " of " + formattedByteCount(f.size) + " so far";
            } else if (f.size && at->size > f.size) {
                // (Not a download of that asset, whatever it is: not handed to Windows by this look.)
                *state = at->name + " there is not GitHub's file (its size is another): not taken";
            } else if (!at->whole) {
                *state = at->name + " is there, still being written";
            } else if (appPackage::installCopiedUpdate()) {
                *state = at->name + " is there: Windows is asked to install it";
                *handed = identity;
                shellLog::write("ota: " + at->name + " has come to Pictures\\Megler (" + formattedByteCount(at->size) +
                                "): Windows is asked to install it");
            } else {
                // (A look there, or an install, is under way: asked at the next look.)
                *state = at->name + " is there";
            }
        },
        [f, state, handed] {
            std::lock_guard<std::mutex> lock(gMutex);
            if (!gAwaited || gAwaited->f.version != f.version) return;
            gAwaited->looking = false;
            while (!state->empty() && state->back() == '.') state->pop_back();
            gAwaited->state = *state;
            gAwaited->handed = *handed;
            if (GetTickCount64() < gAwaited->until) return;
            // Two hours on: not looked for any more by this start (the next finds it there all the same).
            const bool was = !gAwaited->handed.empty();
            gAwaited.reset();
            shellLog::write("ota: Megler " + f.version + ": its file is not looked for in Pictures\\Megler any more, two hours on");
            if (gStep == Step::idle && !was)
                gSummary = "Megler " + f.version + " did not come to Pictures\\Megler in two hours: Megler looks there again when it starts";
        });
}

/// At start (a worker thread), by LocalState\update-browser.txt: once the Megler fetched with the browser is installed, its
/// file is removed from Pictures\Megler; while it is not, the look for it is taken up for what is left of its two hours.
void restoreAwaited() {
    const std::optional<Note> note = readNote();
    if (!note) {
        files::remove(noteFile());
        return;
    }
    const std::string version = note->f.version, current = appPackage::meglerVersion();
    if (!appPackage::olderVersion(current, version)) {
        const int removed = appPackage::removeCopiedUpdates(version);
        files::remove(noteFile());
        shellLog::write(format("ota: Megler %s was fetched with the browser, and this is %s: %d of its files removed from Pictures\\Megler",
                               version.c_str(), current.c_str(), removed));
        return;
    }
    const int64_t since = int64_t(std::time(nullptr)) - note->asked;
    if (!note->asked || since < 0 || since >= int64_t(awaitMs / 1000)) return;
    const std::wstring folder = appPackage::copiedUpdateFolder();
    std::lock_guard<std::mutex> lock(gMutex);
    if (gAwaited) return;
    const uint64_t now = GetTickCount64();
    Awaited awaited{note->f};
    awaited.folder = folder;
    awaited.asked = true;
    awaited.until = now + (awaitMs - uint64_t(since) * 1000);
    awaited.next = now + lookEveryMs;
    gAwaited = awaited;
    // (Not offered by itself meanwhile.)
    gDeclined = version;
    if (gStep == Step::idle && gSummary.empty()) gSummary = waitingLine(version);
    shellLog::write(format("ota: Megler %s is fetched with the browser, asked for %lld minutes ago: its file is looked for in Pictures\\Megler",
                           version.c_str(), static_cast<long long>(since / 60)));
}

/// The dialog's Stop: the download under way is ended (its worker looks five times a second). False where none is under
/// way: it is being installed by now, which cannot be stopped.
bool stop() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gStep != Step::downloading) return false;
    gStop = true;
    return true;
}

/// Update, or Try Again: the download begun on its worker thread, and its dialog shown.
void startDownload(AppShell& shell, const Found& f) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gStep != Step::idle && gStep != Step::offering) return;
        gStep = Step::downloading;
        gFound = f;
        gFailure.reset();
        if (gAwaited && gAwaited->f.version != f.version) gAwaited.reset(); // (an older one's file is not waited for)
        gSummary = "Megler " + f.version + " is being downloaded";
        gStop = false;
        gGaveWay = false;
        gGot = 0;
        gTotal = f.size;
    }
    {
        std::lock_guard<std::mutex> lock(gSpeedMutex);
        gSpeed.reset();
    }
    shellLog::write("ota: Megler " + f.version + " taken: downloading " + f.url);
    shell.runInBackground([f] { downloadAndInstall(f); });
    showProgress(shell);
}

void offer(AppShell& shell, const Found& f) {
    const bool phone = platform::deviceFamily() == "Windows.Mobile", route = browserRoute();
    AppShell::DialogSpec spec;
    spec.title = "Megler " + f.version;
    const std::string news = whatsNew(f.notes);
    spec.message = format("A newer Megler is on GitHub: %s (this one is %s). Its package for this %s (%s) is %s%s: Megler downloads it, checks it and "
                          "installs it. Megler closes while Windows installs it, a few minutes%s; then open Megler again.%s",
                          f.version.c_str(), appPackage::meglerVersion().c_str(), device(), packageSuffix().c_str(),
                          formattedByteCount(f.size).c_str(), metered() ? ", over a connection paid for by the byte" : "",
                          phone ? " (leave the phone on)" : "",
                          route ? " If its download here is too slow, In Browser tells how to get the file with the browser instead." : "") +
                   (news.empty() ? std::string() : "\n\nWhat's new:\n" + news);
    // (The confirming one last; In Browser between them where there is that way.)
    if (route)
        spec.buttons = {"Later", "In Browser", "Update"};
    else
        spec.buttons = {"Later", "Update"};
    std::weak_ptr<AppShell> weak = shell.weak_from_this();
    shell.dialog(std::move(spec), [weak, f, route](int button, std::string) {
        auto self = weak.lock();
        if (self && button == (route ? 2 : 1)) {
            startDownload(*self, f);
            return;
        }
        if (self && route && button == 1) {
            inBrowser(*self, f);
            return;
        }
        // Later (or dismissed): not offered again this start, unless Check Now asks.
        std::lock_guard<std::mutex> lock(gMutex);
        gDeclined = f.version;
        gStep = Step::idle;
        shellLog::write("ota: Megler " + f.version + " not now");
    });
}

/// What went wrong, in a dialog: a download's with Try Again, and with In Browser where there is that way.
void say(AppShell& shell, const Failure& failure) {
    AppShell::DialogSpec spec;
    spec.title = failure.title;
    spec.message = failure.text;
    if (!failure.download) {
        spec.buttons = {"OK"};
        shell.dialog(std::move(spec), nullptr);
        return;
    }
    const bool route = browserRoute();
    if (route)
        spec.buttons = {"Close", "In Browser", "Try Again"};
    else
        spec.buttons = {"Close", "Try Again"};
    std::weak_ptr<AppShell> weak = shell.weak_from_this();
    const Found f = failure.f;
    shell.dialog(std::move(spec), [weak, f, route](int button, std::string) {
        auto self = weak.lock();
        if (!self) return;
        if (button == (route ? 2 : 1))
            startDownload(*self, f);
        else if (route && button == 1)
            inBrowser(*self, f);
    });
}

} // namespace

std::string packageSuffix() { return appPackage::ownPackageSuffix(); }

bool atStart() { return settings::megler(settings::otaAtStart).value_or("") != "off"; }

void setAtStart(bool on) {
    settings::setMegler(settings::otaAtStart, on ? "" : "off");
    shellLog::write(std::string("ota: GitHub asked ") + (on ? "at start" : "only when asked"));
}

void checkNow() {
    std::lock_guard<std::mutex> lock(gMutex);
    gAskNow = true;
    if (gStep == Step::idle) gSummary = "Asking GitHub…";
}

std::string summary() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gSummary.empty()) return gSummary;
    return packageSuffix().empty() ? "GitHub has no Megler for this Windows and processor"
                                   : "Megler " + appPackage::meglerVersion() + " takes Megler_<version>_" + packageSuffix() + ".appx";
}

Progress progress() {
    Progress now;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if ((gStep != Step::downloading && gStep != Step::installing) || !gFound) {
            // (Its file fetched with the browser and waited for; taken by Windows, it is the Library's other card.)
            if (gAwaited && gAwaited->asked && !appPackage::selfUpdating()) {
                now.kind = Progress::Kind::waiting;
                now.version = gAwaited->f.version;
                now.file = gAwaited->f.file;
                now.state = gAwaited->state;
            }
            return now;
        }
        now.kind = gStep == Step::downloading ? Progress::Kind::downloading : Progress::Kind::installing;
        now.version = gFound->version;
        now.file = gFound->file;
    }
    now.got = gGot;
    now.total = gTotal;
    std::lock_guard<std::mutex> lock(gSpeedMutex);
    now.bytesPerSecond = gSpeed.at(GetTickCount64());
    return now;
}

void showProgress(AppShell& shell) {
    const Progress at = progress();
    // (A file fetched with the browser and waited for: that way's dialog.)
    if (at.kind == Progress::Kind::waiting) {
        showBrowser(shell);
        return;
    }
    if (at.kind == Progress::Kind::none || shell.dialogShown()) return;
    const bool installing = at.kind == Progress::Kind::installing, phone = platform::deviceFamily() == "Windows.Mobile";
    const bool route = browserRoute();
    AppShell::DialogSpec spec;
    spec.title = "Megler " + at.version;
    if (installing) {
        spec.message = format("%s is downloaded and checked, and Windows is installing it. Megler is closed when Windows takes it, %s: %sopen "
                              "Megler again after that.",
                              at.file.c_str(), platform::isXbox() ? "in a few minutes" : "as a rule within a minute",
                              phone ? "leave the phone on, and " : "");
        spec.buttons = {"Hide"};
    } else {
        spec.message = format("%s is coming from GitHub. Megler then checks it and installs it, and is closed while Windows installs it: open "
                              "Megler again after that. Keep Megler on the screen%s meanwhile: Windows may hold up an app that is not shown, "
                              "and its download with it.%s",
                              at.file.c_str(), phone ? " and the phone on" : "",
                              route ? " If it comes too slowly, In Browser tells how to get the file with the browser instead." : "");
        if (route)
            spec.buttons = {"Stop", "In Browser", "Hide"};
        else
            spec.buttons = {"Stop", "Hide"};
    }
    // The dialog is of one step: over, it closes by itself (-2), and the next step's is shown in its place.
    spec.live = [kind = at.kind, version = at.version](AppShell::DialogSpec::Live& live) {
        const Progress now = progress();
        if (now.kind != kind || now.version != version) return false;
        if (kind == Progress::Kind::installing) {
            live.text = "Windows is installing it";
            return true;
        }
        live.fraction = now.total ? double(now.got) / double(now.total) : -1;
        live.text = ownUpdate::progressLine(now.got, now.total, now.bytesPerSecond);
        return true;
    };
    std::weak_ptr<AppShell> weak = shell.weak_from_this();
    shell.dialog(std::move(spec), [weak, installing, route](int button, std::string) {
        auto self = weak.lock();
        if (!self) return;
        if (button == -2) {
            // Closed by itself: installing is shown in its turn; what went wrong is said at the next poll (tick).
            showProgress(*self);
        } else if (button == 0 && !installing) {
            // Stop; too late for it where the package is being installed by now, which is shown then.
            if (!stop()) showProgress(*self);
        } else if (button == 1 && !installing && route) {
            // In Browser: this download goes on until the browser is asked for the file.
            std::optional<Found> f;
            {
                std::lock_guard<std::mutex> lock(gMutex);
                f = gFound;
            }
            if (f) inBrowser(*self, *f);
        }
        // (Hide, Back, a tap beside it: the download goes on, and the Library's card and Settings' Check Now show this again.)
    });
}

bool busy() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gStep == Step::downloading || gStep == Step::installing;
}

void tick(AppShell& shell) {
    if (appPackage::isSingleApp() || packageSuffix().empty()) return;
    if (!gTidied) {
        gTidied = true;
        shell.runInBackground([] {
            tidy();
            restoreAwaited();
        });
    }
    std::unique_lock<std::mutex> lock(gMutex);
    if (gFailure && !shell.dialogShown()) {
        const Failure failure = *gFailure;
        gFailure.reset();
        lock.unlock();
        say(shell, failure);
        return;
    }
    if (gAwaited && gAwaited->show && !shell.dialogShown()) {
        gAwaited->show = false;
        lock.unlock();
        showBrowser(shell);
        return;
    }
    // A file fetched with the browser: Pictures\Megler looked in every ten seconds -- not while Megler's own download of it
    // goes on, nor while Windows installs one.
    if (gAwaited && gAwaited->asked && !gAwaited->looking && gStep != Step::downloading && gStep != Step::installing &&
        !appPackage::selfUpdating() && GetTickCount64() >= gAwaited->next) {
        gAwaited->looking = true;
        gAwaited->next = GetTickCount64() + lookEveryMs;
        const Found f = gAwaited->f;
        const std::string handed = gAwaited->handed;
        lock.unlock();
        look(shell, f, handed);
        return;
    }
    switch (gStep) {
    case Step::idle: {
        const bool now = gAskNow;
        if (!now && (!atStart() || std::time(nullptr) - lastAsked() < askEverySeconds)) return;
        // (Not while Megler's own update from Pictures\Megler is being installed.)
        if (appPackage::selfUpdating()) return;
        gAskNow = false;
        gForced = now;
        gStep = Step::asking;
        lock.unlock();
        shell.runInBackground([] { ask(); });
        return;
    }
    case Step::found: {
        if (shell.dialogShown() || appPackage::selfUpdating() || !gFound) return;
        if (gFound->version == gDeclined && !gForced) {
            gStep = Step::idle;
            return;
        }
        // Not downloaded again by itself where Windows refused its last try for access: its package waits in Pictures\Megler,
        // tried at each start (appPackage::keepRefusedUpdate). Check Now offers it all the same.
        if (appPackage::updateRefusedForAccess(gFound->version) && !gForced) {
            // (What the Library says, where this start's try from Pictures\Megler said it; else the way round without it.)
            const std::string failed = appPackage::selfUpdateFailure();
            gSummary = !failed.empty() ? failed : appPackage::ownUpdateRefusedText(gFound->version, gFound->file, false);
            gStep = Step::idle;
            return;
        }
        gStep = Step::offering;
        const Found f = *gFound;
        lock.unlock();
        offer(shell, f);
        return;
    }
    default:
        return;
    }
}

} // namespace megler::app::ui::ota
