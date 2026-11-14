// Megler's own updates over the air, from its releases on GitHub (Computershik73/megler-w10m; the owner's wish,
// 2026-11-05).
//
// Megler asks GitHub's API for the releases when it starts, at most once in six hours (Settings: at start, or only when
// asked), and takes the newest that has a package for this Megler: a release's assets are named by the Windows and the
// processor they are for (docs/RELEASES.md), and only that name is taken:
//   Megler_<version>_ARM.appx     Windows 10 Mobile, 32-bit ARM (every release so far)
//   Megler_<version>_ARM64.appx   desktop Windows 10 and 11 on 64-bit ARM, and on x64 and x86 likewise:
//   Megler_<version>_x64.appx, Megler_<version>_x86.appx
//   Megler_<version>_Xbox_x64.appx  an Xbox in Developer Mode, installed through the console's Device Portal
//   Megler_<version>_RT_ARM.appx  desktop Windows 10 on 32-bit ARM: build 15035 on Windows RT's tablets and on Lumias
// A 32-bit ARM package of a Windows but the phone's is named otherwise (_RT_ARM), so a phone never takes it, nor a PC
// the phone's; a 32-bit ARM Megler on desktop Windows takes the ARM64 one on an ARM64 computer, which runs it, and the
// RT_ARM one on a 32-bit ARM Windows. A newer one is offered in a dialog, with its size and what the release says is
// new; taken, it is downloaded into LocalState\update, its size and its SHA-256 checked against what GitHub says of the
// asset, and installed as Megler's own update from Pictures\Megler is (shell/AppPackage.h, installOwnUpdate): Windows's
// deployment engine (on an Xbox, the console's Device Portal) closes this Megler, and Megler is opened again once Windows
// has installed it. Not in an app's own package. Off the UI thread, all of it but the dialogs.
//
// While it is downloaded a dialog says how far it is, how fast it comes and how long it has left (shell/OwnUpdate.h), with
// Stop and Hide; hidden, the Library's card says the same and brings the dialog back, as Settings' Check Now does. What
// went wrong -- no room, a download that broke off, a package Windows refused -- is said in a dialog too, a download's
// with Try Again. (Until 0.1.0.699 "Update" closed the offer and nothing followed it but a card on the Library's tab,
// without a speed; a failure went into Settings' line and the log.)
//
// Where its download in Megler is too slow, the package can be fetched with the browser (the owner's wish, 2026-11-14):
// In Browser -- in the offer, in the download's dialog and in a failed download's -- shows a dialog with the package's
// own direct link (the asset's, which a browser saves as a file; not the release's page), Download, which opens it in
// the browser, Copy Link, for another browser, a download manager or a computer, and where the file is to be put:
// Pictures\Megler, named as the phone's File Explorer shows it. From Download or Copy Link on, Megler's own download of
// it gives way, and Megler looks in Pictures\Megler every ten seconds for two hours: once the file is there, whole and
// of the size GitHub says, Windows is asked to install it as any update put there is (appPackage::installCopiedUpdate,
// which finds it at every start too). Windows checks the package's signature as it installs it; its SHA-256 is not
// compared on this way. LocalState\update-browser.txt keeps which version is fetched so, and since when: the next start
// takes the look up where this one left it, and once that Megler is installed its file is removed from Pictures\Megler.
// Not on an Xbox, whose packages its Device Portal installs.
//
// LocalState\flags\ota-again (a test's): the newest release is offered though it is not newer than this Megler, so that
// what follows an offer can be tried without a release made for it. (All but the end of the browser's way: a file in
// Pictures\Megler that is not newer than this Megler is not installed.)
#pragma once

#include <cstdint>
#include <string>

namespace megler::app::ui {
class AppShell;
}

namespace megler::app::ui::ota {

/// The part of a package's name that says which Windows and processor it is for, as this Megler needs: "ARM" on a phone,
/// "ARM64", "x64" or "x86" on desktop Windows, "Xbox_x64" on an Xbox, "RT_ARM" on desktop Windows on 32-bit ARM; empty
/// where GitHub has none for it (and nothing is asked). appPackage::ownPackageSuffix, which the packages put into
/// Pictures\Megler are named by too.
std::string packageSuffix();
/// At every poll (UI thread): GitHub asked when it is time, and a newer Megler offered; once taken, downloaded, checked
/// and installed.
void tick(AppShell& shell);
/// Settings' Check Now: GitHub asked at the next poll, and a newer Megler offered even if "Later" was said to it.
void checkNow();
/// Whether GitHub is asked at start (Settings): yes unless turned off.
bool atStart();
void setAtStart(bool on);
/// Where it stands, in a line, for Settings.
std::string summary();
/// Where a newer Megler that was taken stands: being downloaded (what has come, of how much, how fast), installed, or
/// waited for in Pictures\Megler, fetched with the browser (`state`: where its file stands, a line without its full stop;
/// empty before the first look).
struct Progress {
    enum class Kind { none, downloading, installing, waiting } kind = Kind::none;
    std::string version, file, state;
    uint64_t got = 0, total = 0;
    double bytesPerSecond = -1; // as ownUpdate::Speed::at says it: 0 nothing coming, less than 0 not known yet
};
Progress progress();
/// The dialog of a newer Megler being downloaded (how far, how fast, how long yet; Stop, In Browser, Hide) or installed,
/// or the browser's way's where its file is waited for; where one is and no other dialog is up: shown when Update is
/// pressed, and again from the Library's card and Settings' Check Now.
void showProgress(AppShell& shell);
/// While a newer Megler is downloaded or installed: the screen is kept on (ui/AppShell), for a phone that locks suspends
/// Megler, and its download with it.
bool busy();

} // namespace megler::app::ui::ota
