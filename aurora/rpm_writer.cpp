#include "rpm_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <limits>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>

namespace RpmWriter {
namespace {

// ---------- hashes ----------

uint32_t crc32(const std::string &data)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::string md5(const std::string &data)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    std::string msg = data;
    msg.push_back(char(0x80));
    while (msg.size() % 64 != 56) msg.push_back(0);
    uint64_t bits = uint64_t(data.size()) * 8;
    for (int i = 0; i < 8; ++i) msg.push_back(char((bits >> (8 * i)) & 0xFF));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            const unsigned char *p = reinterpret_cast<const unsigned char *>(msg.data()) + off + i * 4;
            M[i] = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t F;
            int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + rol(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    std::string out;
    for (uint32_t v : {a0, b0, c0, d0})
        for (int i = 0; i < 4; ++i) out.push_back(char((v >> (8 * i)) & 0xFF));
    return out;
}

std::string sha256(const std::string &data)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg = data;
    msg.push_back(char(0x80));
    while (msg.size() % 64 != 56) msg.push_back(0);
    uint64_t bits = uint64_t(data.size()) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(char((bits >> (8 * i)) & 0xFF));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const unsigned char *p = reinterpret_cast<const unsigned char *>(msg.data()) + off + i * 4;
            w[i] = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; --i) out.push_back(char((v >> (8 * i)) & 0xFF));
    return out;
}

std::string hex(const std::string &raw)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

// ---------- byte helpers ----------

void be16(std::string &s, uint16_t v) { s.push_back(char(v >> 8)); s.push_back(char(v & 0xFF)); }
void be32(std::string &s, uint32_t v)
{
    for (int i = 3; i >= 0; --i) s.push_back(char((v >> (8 * i)) & 0xFF));
}
void le32(std::string &s, uint32_t v)
{
    for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF));
}
void padTo(std::string &s, size_t alignment)
{
    while (s.size() % alignment) s.push_back(0);
}

// ---------- payload ----------

std::string hex8(uint32_t v)
{
    char buffer[9];
    std::snprintf(buffer, sizeof(buffer), "%08X", v);
    return buffer;
}

std::string cpio(const std::vector<File> &files, uint32_t mtime)
{
    std::string out;
    uint32_t inode = 1;
    auto entry = [&](const std::string &name, uint32_t mode, const std::string &data, uint32_t ino) {
        out += "070701";
        const uint32_t fields[] = {ino, mode, 0, 0, 1, mtime, uint32_t(data.size()), 0, 0, 0, 0,
                                   uint32_t(name.size() + 1), 0};
        for (uint32_t f : fields) out += hex8(f);
        out += name;
        out.push_back(0);
        padTo(out, 4);
        out += data;
        padTo(out, 4);
    };
    for (const File &f : files) entry("." + f.path, 0100000u | f.mode, f.content, inode++);
    entry("TRAILER!!!", 0, std::string(), 0);
    return out;
}

std::string gzipStored(const std::string &data)
{
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
    size_t pos = 0;
    do {
        const size_t chunk = std::min<size_t>(65535, data.size() - pos);
        const bool last = pos + chunk >= data.size();
        out.push_back(last ? 1 : 0);
        out.push_back(char(chunk & 0xFF));
        out.push_back(char(chunk >> 8));
        out.push_back(char(~chunk & 0xFF));
        out.push_back(char((~chunk >> 8) & 0xFF));
        out.append(data, pos, chunk);
        pos += chunk;
    } while (pos < data.size());
    le32(out, crc32(data));
    le32(out, uint32_t(data.size()));
    return out;
}

// ---------- header ----------

enum Type { Int16 = 3, Int32 = 4, String = 6, Bin = 7, StringArray = 8, I18N = 9 };

class Header {
public:
    void str(uint32_t tag, const std::string &v, Type type = String)
    {
        add(tag, type, 1, 1, v + '\0');
    }
    void strings(uint32_t tag, const std::vector<std::string> &v)
    {
        std::string d;
        for (const std::string &s : v) d += s + '\0';
        add(tag, StringArray, uint32_t(v.size()), 1, d);
    }
    void int32(uint32_t tag, const std::vector<uint32_t> &v)
    {
        std::string d;
        for (uint32_t x : v) be32(d, x);
        add(tag, Int32, uint32_t(v.size()), 4, d);
    }
    void int16(uint32_t tag, const std::vector<uint16_t> &v)
    {
        std::string d;
        for (uint16_t x : v) be16(d, x);
        add(tag, Int16, uint32_t(v.size()), 2, d);
    }
    void bin(uint32_t tag, const std::string &v) { add(tag, Bin, uint32_t(v.size()), 1, v); }

    std::string serialize(uint32_t regionTag = 0) const
    {
        std::string index, store;
        for (const auto &kv : m_entries) {
            const Entry &e = kv.second;
            padTo(store, e.align);
            be32(index, kv.first);
            be32(index, e.type);
            be32(index, uint32_t(store.size()));
            be32(index, e.count);
            store += e.data;
        }
        if (regionTag) {
            std::string region;
            be32(region, regionTag);
            be32(region, Bin);
            be32(region, uint32_t(store.size()));
            be32(region, 16);
            index.insert(0, region);
            be32(store, regionTag);
            be32(store, Bin);
            be32(store, uint32_t(-int64_t(m_entries.size() + 1) * 16));
            be32(store, 16);
        }
        std::string out("\x8e\xad\xe8\x01\x00\x00\x00\x00", 8);
        be32(out, uint32_t(m_entries.size() + (regionTag ? 1 : 0)));
        be32(out, uint32_t(store.size()));
        return out + index + store;
    }

private:
    struct Entry {
        Type type;
        uint32_t count;
        size_t align;
        std::string data;
    };
    void add(uint32_t tag, Type type, uint32_t count, size_t align, const std::string &data)
    {
        m_entries[tag] = Entry{type, count, align, data};
    }
    std::map<uint32_t, Entry> m_entries; // tag-sorted, as rpm requires
};

} // namespace

std::string build(const Package &package)
{
    std::vector<File> files = package.files;
    std::sort(files.begin(), files.end(), [](const File &a, const File &b) { return a.path < b.path; });

    std::vector<std::string> dirNames, baseNames, digests, empty, users;
    std::vector<uint32_t> sizes, mtimes, flags, devices, inodes, dirIndexes;
    std::vector<uint16_t> modes, rdevs;
    uint32_t totalSize = 0;
    uint32_t inode = 1;
    for (const File &f : files) {
        const size_t slash = f.path.rfind('/');
        const std::string dir = f.path.substr(0, slash + 1);
        auto it = std::find(dirNames.begin(), dirNames.end(), dir);
        if (it == dirNames.end()) {
            dirNames.push_back(dir);
            it = dirNames.end() - 1;
        }
        dirIndexes.push_back(uint32_t(it - dirNames.begin()));
        baseNames.push_back(f.path.substr(slash + 1));
        digests.push_back(hex(sha256(f.content)));
        sizes.push_back(uint32_t(f.content.size()));
        modes.push_back(uint16_t(0100000u | f.mode));
        rdevs.push_back(0);
        mtimes.push_back(package.buildTime);
        flags.push_back(0);
        devices.push_back(1);
        inodes.push_back(inode++);
        empty.push_back("");
        users.push_back("root");
        totalSize += uint32_t(f.content.size());
    }

    const std::string nvr = package.version + "-" + package.release;
    Header h;
    h.str(1000, package.name);
    h.str(1001, package.version);
    h.str(1002, package.release);
    h.str(1004, package.summary, I18N);
    h.str(1005, package.description, I18N);
    h.int32(1006, {package.buildTime});
    h.int32(1009, {totalSize});
    h.str(1014, package.license);
    h.str(1016, "Unspecified", I18N);
    h.str(1021, "linux");
    h.str(1022, package.arch);
    if (!files.empty()) {
        h.int32(1028, sizes);
        h.int16(1030, modes);
        h.int16(1033, rdevs);
        h.int32(1034, mtimes);
        h.strings(1035, digests);
        h.strings(1036, empty);
        h.int32(1037, flags);
        h.strings(1039, users);
        h.strings(1040, users);
    }
    h.strings(1047, {package.name});
    h.str(1064, "4.14.3");
    if (!files.empty()) {
        h.int32(1095, devices);
        h.int32(1096, inodes);
        h.strings(1097, empty);
    }
    h.int32(1112, {8}); // RPMSENSE_EQUAL
    h.strings(1113, {nvr});
    if (!files.empty()) {
        h.int32(1116, dirIndexes);
        h.strings(1117, baseNames);
        h.strings(1118, dirNames);
    }
    h.str(1124, "cpio");
    h.str(1125, "gzip");
    h.str(1126, "1");
    h.int32(5011, {8}); // file digests are SHA-256
    const std::string header = h.serialize();

    const std::string rawPayload = cpio(files, package.buildTime);
    const std::string payload = gzipStored(rawPayload);
    const std::string body = header + payload;

    Header s;
    s.str(273, hex(sha256(header)));
    s.int32(1000, {uint32_t(body.size())});
    s.bin(1004, md5(body));
    s.int32(1007, {uint32_t(rawPayload.size())});
    std::string signature = s.serialize();
    padTo(signature, 8);

    std::string lead;
    be32(lead, 0xEDABEEDBu);
    lead.push_back(3);
    lead.push_back(0);
    be16(lead, 0);      // binary package
    be16(lead, package.arch == "noarch" ? 0 : 12);
    std::string leadName = package.name + "-" + nvr;
    leadName.resize(66, '\0');
    lead += leadName;
    be16(lead, 1);      // linux
    be16(lead, 5);      // header-style signature
    lead.append(16, '\0');

    return lead + signature + body;
}

namespace {

bool writeBytes(QIODevice &out, const char *data, qint64 size)
{
    while (size > 0) {
        const qint64 written = out.write(data, size);
        if (written <= 0) return false;
        size -= written;
        data += written;
    }
    return true;
}

class StoredGzip {
public:
    explicit StoredGzip(QIODevice &out) : m_out(out)
    {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1) ? (value >> 1) ^ 0xedb88320u : value >> 1;
            m_table[i] = value;
        }
    }

    bool start()
    {
        const char header[] = {char(0x1f), char(0x8b), 8, 0, 0, 0, 0, 0, 0, 3};
        return writeBytes(m_out, header, sizeof(header));
    }

    bool write(const char *data, qint64 size)
    {
        if (size < 0 || uint64_t(size) > std::numeric_limits<uint32_t>::max() - m_size) return false;
        m_size += uint32_t(size);
        while (size > 0) {
            const uint16_t n = uint16_t(qMin<qint64>(size, 65535));
            const uint16_t inverse = uint16_t(~n);
            const char header[] = {0, char(n), char(n >> 8), char(inverse), char(inverse >> 8)};
            if (!writeBytes(m_out, header, sizeof(header)) || !writeBytes(m_out, data, n)) return false;
            for (unsigned int i = 0; i < n; ++i)
                m_crc = m_table[(m_crc ^ uint8_t(data[i])) & 0xff] ^ (m_crc >> 8);
            data += n;
            size -= n;
        }
        return true;
    }

    bool finish()
    {
        const char finalBlock[] = {1, 0, 0, char(0xff), char(0xff)};
        std::string trailer;
        le32(trailer, m_crc ^ 0xffffffffu);
        le32(trailer, m_size);
        return writeBytes(m_out, finalBlock, sizeof(finalBlock)) &&
            writeBytes(m_out, trailer.data(), qint64(trailer.size()));
    }

    uint32_t size() const { return m_size; }

private:
    QIODevice &m_out;
    uint32_t m_crc = 0xffffffffu;
    uint32_t m_size = 0;
    uint32_t m_table[256];
};

struct StreamEntry {
    StreamFile file;
    uint32_t size;
    QByteArray digest;
};

bool readEntry(const StreamFile &file, QCryptographicHash &hash, StoredGzip *gzip, uint32_t *size)
{
    uint64_t total = 0;
    auto consume = [&](const char *bytes, qint64 count) {
        if (count < 0 || uint64_t(count) > std::numeric_limits<uint32_t>::max() - total) return false;
        hash.addData(bytes, int(count));
        if (gzip && !gzip->write(bytes, count)) return false;
        total += uint64_t(count);
        return true;
    };
    if (file.sourcePath.empty()) {
        const std::string &value = file.content;
        size_t offset = 0;
        while (offset < value.size()) {
            const qint64 count = qint64(std::min<size_t>(65536, value.size() - offset));
            if (!consume(value.data() + offset, count)) return false;
            offset += size_t(count);
        }
    } else {
        QFile input(QString::fromUtf8(file.sourcePath.c_str()));
        const QFileInfo info(input);
        if (!file.content.empty() || !info.isFile() || info.isSymLink() || !input.open(QIODevice::ReadOnly))
            return false;
        char buffer[65536];
        for (;;) {
            const qint64 count = input.read(buffer, sizeof(buffer));
            if (count < 0) return false;
            if (count == 0) break;
            if (!consume(buffer, count)) return false;
        }
    }
    *size = uint32_t(total);
    return true;
}

bool writeCpioHeader(StoredGzip &gzip, const std::string &name, uint32_t mode,
                     uint32_t inode, uint32_t mtime, uint32_t size)
{
    std::string header("070701");
    const uint32_t fields[] = {inode, mode, 0, 0, 1, mtime, size, 0, 0, 0, 0,
                               uint32_t(name.size() + 1), 0};
    for (uint32_t field : fields) header += hex8(field);
    header += name;
    header.push_back(0);
    while (header.size() % 4) header.push_back(0);
    return gzip.write(header.data(), qint64(header.size()));
}

bool hashOrCopy(QIODevice &source, QIODevice *target, QCryptographicHash *hash)
{
    char buffer[65536];
    for (;;) {
        const qint64 count = source.read(buffer, sizeof(buffer));
        if (count < 0) return false;
        if (count == 0) return true;
        if (hash) hash->addData(buffer, int(count));
        if (target && !writeBytes(*target, buffer, count)) return false;
    }
}

}

bool writeStreaming(const Package &package, const std::vector<StreamFile> &input,
                    const std::string &outputPath, std::string *error)
{
    if (error) error->clear();
    auto fail = [error](const char *message) {
        if (error) *error = message;
        return false;
    };
    if (outputPath.empty() || package.name.empty() || package.arch.empty() || !package.files.empty())
        return fail("Invalid package metadata or output path.");
    std::vector<StreamFile> files = input;
    std::sort(files.begin(), files.end(), [](const StreamFile &a, const StreamFile &b) { return a.path < b.path; });
    std::vector<StreamEntry> entries;
    std::vector<std::string> dirs, names, digests, empty, users;
    std::vector<uint32_t> sizes, mtimes, flags, devices, inodes, indexes;
    std::vector<uint16_t> modes, rdevs;
    uint64_t total = 0;
    uint32_t inode = 1;
    std::string previous;
    for (const StreamFile &file : files) {
        const QString path = QString::fromUtf8(file.path.c_str());
        if (file.path.empty() || file.path[0] != '/' || path != QDir::cleanPath(path) ||
            file.path == previous || (file.mode & ~0777u))
            return fail("Invalid or duplicate package file path or mode.");
        previous = file.path;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        uint32_t size = 0;
        if (!readEntry(file, hash, nullptr, &size)) return fail("Cannot read a package source file.");
        total += size;
        if (total > std::numeric_limits<uint32_t>::max()) return fail("Package payload is too large.");
        const QByteArray digest = hash.result();
        entries.push_back(StreamEntry{file, size, digest});
        const size_t slash = file.path.rfind('/');
        const std::string dir = file.path.substr(0, slash + 1);
        auto it = std::find(dirs.begin(), dirs.end(), dir);
        if (it == dirs.end()) { dirs.push_back(dir); it = dirs.end() - 1; }
        indexes.push_back(uint32_t(it - dirs.begin()));
        names.push_back(file.path.substr(slash + 1));
        digests.push_back(digest.toHex().toStdString());
        sizes.push_back(size);
        modes.push_back(uint16_t(0100000u | file.mode));
        rdevs.push_back(0);
        mtimes.push_back(package.buildTime);
        flags.push_back(0);
        devices.push_back(1);
        inodes.push_back(inode++);
        empty.push_back("");
        users.push_back("root");
    }

    const QString output = QString::fromUtf8(outputPath.c_str());
    QTemporaryFile payload(QFileInfo(output).dir().filePath(QStringLiteral(".rpm-payload-XXXXXX")));
    if (!payload.open()) return fail("Cannot create the temporary RPM payload.");
    StoredGzip gzip(payload);
    if (!gzip.start()) return fail("Cannot start the RPM payload.");
    inode = 1;
    for (const StreamEntry &entry : entries) {
        if (!writeCpioHeader(gzip, "." + entry.file.path, 0100000u | entry.file.mode,
                             inode++, package.buildTime, entry.size))
            return fail("Cannot write the RPM payload header.");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        uint32_t size = 0;
        if (!readEntry(entry.file, hash, &gzip, &size) || size != entry.size || hash.result() != entry.digest)
            return fail("A package source changed or could not be read.");
        const char padding[] = {0, 0, 0};
        if (!gzip.write(padding, (4 - (size % 4)) % 4)) return fail("Cannot write RPM padding.");
    }
    if (!writeCpioHeader(gzip, "TRAILER!!!", 0, 0, package.buildTime, 0) || !gzip.finish() ||
        !payload.flush())
        return fail("Cannot finish the RPM payload.");

    const std::string nvr = package.version + "-" + package.release;
    Header header;
    header.str(1000, package.name); header.str(1001, package.version); header.str(1002, package.release);
    header.str(1004, package.summary, I18N); header.str(1005, package.description, I18N);
    header.int32(1006, {package.buildTime}); header.int32(1009, {uint32_t(total)});
    header.str(1014, package.license); header.str(1016, "Unspecified", I18N);
    header.str(1021, "linux"); header.str(1022, package.arch);
    if (!entries.empty()) {
        header.int32(1028, sizes); header.int16(1030, modes); header.int16(1033, rdevs);
        header.int32(1034, mtimes); header.strings(1035, digests); header.strings(1036, empty);
        header.int32(1037, flags); header.strings(1039, users); header.strings(1040, users);
    }
    header.strings(1047, {package.name}); header.str(1064, "4.14.3");
    if (!package.requires.empty()) {
        std::vector<std::string> requirementNames, requirementVersions;
        std::vector<uint32_t> requirementFlags;
        for (const Package::Requirement &requirement : package.requires) {
            if (requirement.name.empty()) return fail("Invalid package dependency.");
            requirementNames.push_back(requirement.name);
            requirementVersions.push_back(requirement.version);
            requirementFlags.push_back(requirement.flags);
        }
        header.int32(1048, requirementFlags);
        header.strings(1049, requirementNames);
        header.strings(1050, requirementVersions);
    }
    if (!entries.empty()) {
        header.int32(1095, devices); header.int32(1096, inodes); header.strings(1097, empty);
    }
    header.int32(1112, {8}); header.strings(1113, {nvr});
    if (!entries.empty()) {
        header.int32(1116, indexes); header.strings(1117, names); header.strings(1118, dirs);
    }
    header.str(1124, "cpio"); header.str(1125, "gzip"); header.str(1126, "1"); header.int32(5011, {8});
    const std::string mainHeader = header.serialize(63);
    if (uint64_t(mainHeader.size()) + uint64_t(payload.size()) > std::numeric_limits<uint32_t>::max())
        return fail("RPM body is too large.");
    QCryptographicHash md5Hash(QCryptographicHash::Md5);
    md5Hash.addData(mainHeader.data(), int(mainHeader.size()));
    if (!payload.seek(0) || !hashOrCopy(payload, nullptr, &md5Hash)) return fail("Cannot read RPM payload.");
    Header signatureHeader;
    signatureHeader.str(273, hex(sha256(mainHeader)));
    signatureHeader.int32(1000, {uint32_t(mainHeader.size() + size_t(payload.size()))});
    const QByteArray md5Value = md5Hash.result();
    signatureHeader.bin(1004, std::string(md5Value.constData(), size_t(md5Value.size())));
    signatureHeader.int32(1007, {gzip.size()});
    std::string signature = signatureHeader.serialize(62);
    padTo(signature, 8);
    std::string lead;
    be32(lead, 0xEDABEEDBu); lead.push_back(3); lead.push_back(0); be16(lead, 0);
    be16(lead, package.arch == "noarch" ? 0 : 12);
    std::string leadName = package.name + "-" + nvr;
    leadName.resize(66, '\0');
    lead += leadName;
    be16(lead, 1); be16(lead, 5); lead.append(16, '\0');
    QSaveFile outputFile(output);
    if (!outputFile.open(QIODevice::WriteOnly) ||
        !writeBytes(outputFile, lead.data(), qint64(lead.size())) ||
        !writeBytes(outputFile, signature.data(), qint64(signature.size())) ||
        !writeBytes(outputFile, mainHeader.data(), qint64(mainHeader.size())) ||
        !payload.seek(0) || !hashOrCopy(payload, &outputFile, nullptr) || !outputFile.commit())
        return fail("Cannot save the RPM file.");
    return true;
}

} // namespace RpmWriter
