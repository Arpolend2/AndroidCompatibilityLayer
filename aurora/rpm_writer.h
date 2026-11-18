#ifndef RPM_WRITER_H
#define RPM_WRITER_H

#include <cstdint>
#include <string>
#include <vector>

// Dependency-free writer of unsigned RPM v3 packages (gzip/cpio payload).
// It does not need rpmbuild, so it can run inside the application sandbox.
namespace RpmWriter {

struct File {
    std::string path;     // absolute install path, e.g. /usr/share/applications/a.desktop
    std::string content;
    uint16_t mode = 0644; // permission bits only
};

struct Package {
    struct Requirement {
        std::string name;
        std::string version;
        uint32_t flags = 0;
    };
    std::string name;
    std::string version;
    std::string release = "1";
    std::string summary;
    std::string description;
    std::string license = "Proprietary";
    std::string arch = "noarch";
    uint32_t buildTime = 0;
    std::vector<File> files;
    std::vector<Requirement> requires;
};

// Returns the complete .rpm file contents.
std::string build(const Package &package);

struct StreamFile {
    std::string path;
    std::string sourcePath;
    std::string content;
    uint16_t mode = 0644;
};

// Writes an RPM without holding the entire payload in memory.
bool writeStreaming(const Package &package, const std::vector<StreamFile> &files,
                    const std::string &outputPath, std::string *error);

} // namespace RpmWriter

#endif
