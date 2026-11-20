#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_SNAPSHOT_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_SNAPSHOT_H

#include <cstdint>
#include <string>

// Parent-side, one-request snapshot. The root must be the sender's own trusted
// data directory, not the installed-apps directory or a guest-supplied path.
// Call off the UI and Megler callback threads. The caller must enforce a bounded
// queue and cancel on sender exit.
// Each object is confined to one thread. Name checks do not prove an APK is
// standalone; the caller must inspect the snapshot and state the limitations.
class InstallRequestSnapshot
{
public:
    InstallRequestSnapshot();
    ~InstallRequestSnapshot();
    InstallRequestSnapshot(const InstallRequestSnapshot &) = delete;
    InstallRequestSnapshot &operator=(const InstallRequestSnapshot &) = delete;

    // Both paths are absolute host paths. Rejects links, special files and
    // files exceeding maxBytes. Bundles require explicit opt-in.
    bool capture(const std::string &root, const std::string &source,
                 const std::string &displayName, uint64_t maxBytes, std::string *error,
                 bool allowBundle = false);
    // Valid only while this object lives; suitable as ApkStore::install source.
    std::string path() const;
    void clear();

private:
    int m_fd;
};

#endif
