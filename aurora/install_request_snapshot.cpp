#include "install_request_snapshot.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif

namespace {

struct Fd {
    explicit Fd(int value = -1) : value(value) {}
    ~Fd() { if (value >= 0) close(value); }
    int value;
};

bool fail(std::string *error, const char *message)
{
    if (error) *error = message;
    return false;
}

bool bundleName(const std::string &name)
{
    std::string lower;
    for (unsigned char c : name) lower += c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : char(c);
    for (const char *suffix : {".apks", ".xapk", ".apkm"}) {
        const size_t n = std::strlen(suffix);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0) return true;
    }
    return false;
}

bool invalidName(const std::string &name)
{
    if (name.empty() || name.size() > 255) return true;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return true;
    return false;
}

}

InstallRequestSnapshot::InstallRequestSnapshot() : m_fd(-1) {}
InstallRequestSnapshot::~InstallRequestSnapshot() { clear(); }

void InstallRequestSnapshot::clear()
{
    if (m_fd >= 0) close(m_fd);
    m_fd = -1;
}

std::string InstallRequestSnapshot::path() const
{
    return m_fd < 0 ? std::string() : "/proc/self/fd/" + std::to_string(m_fd);
}

bool InstallRequestSnapshot::capture(const std::string &root, const std::string &source,
                                     const std::string &displayName, uint64_t maxBytes, std::string *error)
{
    clear();
    if (error) error->clear();
    if (root.empty() || root[0] != '/' || root.back() == '/' ||
        root.find('\0') != std::string::npos || source.find('\0') != std::string::npos ||
        source.size() <= root.size() + 1 || source.compare(0, root.size(), root) != 0 ||
        source[root.size()] != '/' || !maxBytes)
        return fail(error, "The APK is not inside the sender's data directory.");
    if (invalidName(displayName) || bundleName(displayName) || bundleName(source))
        return fail(error, "Unsupported or invalid install request name.");

    const std::string relative = source.substr(root.size() + 1);
    std::vector<std::string> components;
    size_t start = 0;
    while (start < relative.size()) {
        const size_t end = relative.find('/', start);
        const std::string part = relative.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..")
            return fail(error, "Invalid APK path.");
        components.push_back(part);
        if (end == std::string::npos) break;
        start = end + 1;
        if (start == relative.size()) return fail(error, "Invalid APK path.");
    }
    Fd dir(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (dir.value < 0) return fail(error, "Cannot open the sender's data directory.");
    for (size_t i = 0; i + 1 < components.size(); ++i) {
        const int next = openat(dir.value, components[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) return fail(error, "Cannot open an APK directory safely.");
        close(dir.value);
        dir.value = next;
    }
    Fd input(openat(dir.value, components.back().c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (input.value < 0) return fail(error, "Cannot open the APK safely.");
    struct stat st;
    if (fstat(input.value, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_size < 0 || static_cast<uint64_t>(st.st_size) > maxBytes)
        return fail(error, "The APK is not an allowed regular file or is too large.");

#if defined(SYS_memfd_create)
    Fd output(static_cast<int>(syscall(SYS_memfd_create, "megler-install-request", MFD_CLOEXEC | MFD_ALLOW_SEALING)));
#else
    Fd output;
#endif
    if (output.value < 0) return fail(error, "Cannot create a sealed APK snapshot.");
    char buffer[65536];
    uint64_t total = 0;
    for (;;) {
        ssize_t n = read(input.value, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return fail(error, "Cannot read the requested APK.");
        if (!n) break;
        if (static_cast<uint64_t>(n) > maxBytes - total)
            return fail(error, "The requested APK is too large.");
        total += static_cast<uint64_t>(n);
        for (ssize_t offset = 0; offset < n;) {
            const ssize_t written = write(output.value, buffer + offset, static_cast<size_t>(n - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return fail(error, "Cannot save the APK snapshot.");
            offset += written;
        }
    }
    if (fcntl(output.value, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0)
        return fail(error, "Cannot seal the APK snapshot.");
    m_fd = output.value;
    output.value = -1;
    return true;
}
