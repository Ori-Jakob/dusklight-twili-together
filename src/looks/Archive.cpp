#include "looks/Archive.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace twili::looks {
namespace {

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}

// [offset, offset + size) lies inside a buffer of `total` bytes.
bool inside(size_t total, uint64_t offset, uint64_t size) {
    return offset <= total && size <= total - offset;
}

// A NUL-terminated name starting at `offset`, ending before `end`.
std::optional<std::string> cString(std::span<const uint8_t> data, size_t offset, size_t end) {
    end = std::min(end, data.size());
    if (offset >= end) {
        return std::nullopt;
    }
    const auto* first = data.data() + offset;
    const auto* nul = static_cast<const uint8_t*>(std::memchr(first, 0, end - offset));
    if (nul == nullptr) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(first), nul - first);
}

bool sameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        return lower(x) == lower(y);
    });
}

std::string lowercase(std::string text) {
    for (char& c : text) {
        c = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    }
    return text;
}

struct TileShape {
    uint8_t width, height, bytes;
};

std::optional<TileShape> tileShape(uint8_t format) {
    switch (format) {
    case 0x0:  // I4
    case 0x8:  // C4
    case 0xE:  // CMPR
        return TileShape{8, 8, 32};
    case 0x1:  // I8
    case 0x2:  // IA4
    case 0x9:  // C8
        return TileShape{8, 4, 32};
    case 0x3:  // IA8
    case 0x4:  // RGB565
    case 0x5:  // RGB5A3
    case 0xA:  // C14X2
        return TileShape{4, 4, 32};
    case 0x6:  // RGBA8
        return TileShape{4, 4, 64};
    default:
        return std::nullopt;
    }
}

constexpr uint32_t kRarcHeader = 0x20;
constexpr uint32_t kRarcNodeBytes = 0x10;
constexpr uint32_t kRarcEntryBytes = 0x14;
constexpr uint8_t kRarcDir = 0x02;
constexpr uint8_t kRarcCompressed = 0x04;
constexpr uint8_t kRarcYaz0 = 0x80;
constexpr uint32_t kResTimgBytes = 0x20;

}  // namespace

bool isYaz0(std::span<const uint8_t> data) {
    return data.size() >= 16 && std::memcmp(data.data(), "Yaz0", 4) == 0;
}

std::optional<std::vector<uint8_t>> yaz0Decode(std::span<const uint8_t> data, size_t maxOut) {
    if (!isYaz0(data)) {
        return std::nullopt;
    }
    const size_t outSize = be32(data.data() + 4);
    if (outSize > maxOut) {
        return std::nullopt;
    }
    std::vector<uint8_t> out(outSize);
    size_t src = 16, dst = 0;
    while (dst < outSize) {
        if (src >= data.size()) {
            return std::nullopt;
        }
        uint8_t code = data[src++];
        for (int bit = 0; bit < 8 && dst < outSize; bit++, code <<= 1) {
            if (code & 0x80) {
                if (src >= data.size()) {
                    return std::nullopt;
                }
                out[dst++] = data[src++];
                continue;
            }
            if (src + 1 >= data.size()) {
                return std::nullopt;
            }
            const uint8_t b1 = data[src], b2 = data[src + 1];
            src += 2;
            const size_t dist = (static_cast<size_t>(b1 & 0x0F) << 8 | b2) + 1;
            size_t count = b1 >> 4;
            if (count == 0) {
                if (src >= data.size()) {
                    return std::nullopt;
                }
                count = static_cast<size_t>(data[src++]) + 0x12;
            } else {
                count += 2;
            }
            if (dist > dst) {
                return std::nullopt;
            }
            for (; count > 0 && dst < outSize; count--, dst++) {
                out[dst] = out[dst - dist];
            }
        }
    }
    return out;
}

std::optional<std::vector<RarcFile>> rarcFiles(std::span<const uint8_t> arc) {
    if (arc.size() < kRarcHeader + 0x20 || std::memcmp(arc.data(), "RARC", 4) != 0) {
        return std::nullopt;
    }
    const uint8_t* info = arc.data() + kRarcHeader;
    const uint64_t dataStart = uint64_t(kRarcHeader) + be32(arc.data() + 0x0C);
    const uint64_t entryCount = be32(info + 0x08);
    const uint64_t entryStart = uint64_t(kRarcHeader) + be32(info + 0x0C);
    const uint64_t strSize = be32(info + 0x10);
    const uint64_t strStart = uint64_t(kRarcHeader) + be32(info + 0x14);
    if (!inside(arc.size(), entryStart, entryCount * kRarcEntryBytes) ||
        !inside(arc.size(), strStart, strSize) || dataStart > arc.size())
    {
        return std::nullopt;
    }
    std::vector<RarcFile> files;
    for (uint64_t i = 0; i < entryCount; i++) {
        const uint8_t* e = arc.data() + entryStart + i * kRarcEntryBytes;
        const uint32_t typeAndName = be32(e + 4);
        const uint8_t flags = static_cast<uint8_t>(typeAndName >> 24);
        if (flags & kRarcDir) {
            continue;
        }
        const auto name = cString(arc, strStart + (typeAndName & 0x00FFFFFF), strStart + strSize);
        const uint64_t offset = dataStart + be32(e + 8);
        const uint32_t size = be32(e + 12);
        if (!name || !inside(arc.size(), offset, size)) {
            return std::nullopt;
        }
        files.push_back({*name, static_cast<uint32_t>(offset), size, flags});
    }
    return files;
}

std::optional<std::vector<uint8_t>> rarcRead(std::span<const uint8_t> arc, const std::string& name,
    size_t maxOut) {
    const auto files = rarcFiles(arc);
    if (!files) {
        return std::nullopt;
    }
    for (const RarcFile& f : *files) {
        if (sameName(f.name, name)) {
            return rarcReadFile(arc, f, maxOut);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> rarcReadFile(std::span<const uint8_t> arc, const RarcFile& file,
    size_t maxOut) {
    if (!inside(arc.size(), file.offset, file.size)) {
        return std::nullopt;
    }
    const std::span<const uint8_t> bytes = arc.subspan(file.offset, file.size);
    if (file.flags & kRarcCompressed) {
        // Yay0 is not read: nothing the looks need is stored that way.
        return (file.flags & kRarcYaz0) ? yaz0Decode(bytes, maxOut) : std::nullopt;
    }
    if (bytes.size() > maxOut) {
        return std::nullopt;
    }
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

std::optional<std::vector<RarcPath>> rarcPaths(std::span<const uint8_t> arc) {
    const auto files = rarcFiles(arc);
    if (!files) {
        return std::nullopt;
    }
    const uint8_t* info = arc.data() + kRarcHeader;
    const uint64_t nodeCount = be32(info + 0x00);
    const uint64_t nodeStart = uint64_t(kRarcHeader) + be32(info + 0x04);
    const uint64_t entryCount = be32(info + 0x08);
    const uint64_t entryStart = uint64_t(kRarcHeader) + be32(info + 0x0C);
    const uint64_t strSize = be32(info + 0x10);
    const uint64_t strStart = uint64_t(kRarcHeader) + be32(info + 0x14);
    if (nodeCount == 0 || !inside(arc.size(), nodeStart, nodeCount * kRarcNodeBytes)) {
        return std::nullopt;
    }
    const auto nameAt = [&](uint32_t offset) {
        return cString(arc, strStart + offset, strStart + strSize);
    };
    // Entry index to path, walked from the root node as JKRArchive::buildIndexToPathMap does.
    std::vector<std::string> paths(entryCount);
    std::vector<bool> visited(nodeCount, false);
    const auto rootName = nameAt(be32(arc.data() + nodeStart + 4));
    if (!rootName) {
        return std::nullopt;
    }
    std::vector<std::pair<uint64_t, std::string>> stack{{0, *rootName + "/"}};
    while (!stack.empty()) {
        auto [node, prefix] = std::move(stack.back());
        stack.pop_back();
        if (node >= nodeCount || visited[node]) {
            continue;
        }
        visited[node] = true;
        const uint8_t* n = arc.data() + nodeStart + node * kRarcNodeBytes;
        const uint64_t first = be32(n + 0x0C);
        const uint64_t count = be16(n + 0x0A);
        if (first > entryCount || count > entryCount - first) {
            return std::nullopt;
        }
        for (uint64_t i = first; i < first + count; i++) {
            const uint8_t* e = arc.data() + entryStart + i * kRarcEntryBytes;
            const uint32_t typeAndName = be32(e + 4);
            const auto name = nameAt(typeAndName & 0x00FFFFFF);
            if (!name) {
                return std::nullopt;
            }
            if (*name == "." || *name == "..") {
                continue;
            }
            if ((typeAndName >> 24) & kRarcDir) {
                stack.emplace_back(be32(e + 8), prefix + *name + "/");
            } else {
                paths[i] = lowercase(prefix + *name);
            }
        }
    }
    // rarcFiles lists the file entries in table order.
    std::vector<RarcPath> out;
    size_t next = 0;
    for (uint64_t i = 0; i < entryCount; i++) {
        const uint8_t* e = arc.data() + entryStart + i * kRarcEntryBytes;
        if ((be32(e + 4) >> 24) & kRarcDir) {
            continue;
        }
        if (next < files->size() && !paths[i].empty()) {
            out.push_back({std::move(paths[i]), (*files)[next]});
        }
        next++;
    }
    return out;
}

std::optional<std::vector<Tex1Entry>> bmdTextures(std::span<const uint8_t> bmd) {
    if (bmd.size() < 0x20 || std::memcmp(bmd.data(), "J3D2", 4) != 0) {
        return std::nullopt;
    }
    const uint32_t sections = be32(bmd.data() + 0x0C);
    uint64_t at = 0x20;
    for (uint32_t s = 0; s < sections && s < 64; s++) {
        if (!inside(bmd.size(), at, 8)) {
            return std::nullopt;
        }
        const uint32_t size = be32(bmd.data() + at + 4);
        if (size < 8 || !inside(bmd.size(), at, size)) {
            return std::nullopt;
        }
        if (std::memcmp(bmd.data() + at, "TEX1", 4) != 0) {
            at += size;
            continue;
        }
        if (size < 0x14) {
            return std::nullopt;
        }
        const uint8_t* tex1 = bmd.data() + at;
        const uint16_t count = be16(tex1 + 8);
        const uint64_t headers = at + be32(tex1 + 0x0C);
        const uint64_t names = at + be32(tex1 + 0x10);
        if (!inside(bmd.size(), headers, uint64_t(count) * kResTimgBytes) ||
            !inside(bmd.size(), names, 4) ||
            !inside(bmd.size(), names + 4, uint64_t(be16(bmd.data() + names)) * 4) ||
            be16(bmd.data() + names) < count)
        {
            return std::nullopt;
        }
        std::vector<Tex1Entry> out;
        out.reserve(count);
        for (uint16_t i = 0; i < count; i++) {
            const uint8_t* entry = bmd.data() + names + 4 + i * 4;
            const auto name = cString(bmd, names + be16(entry + 2), bmd.size());
            const uint64_t header = headers + uint64_t(i) * kResTimgBytes;
            const uint8_t* timg = bmd.data() + header;
            Tex1Entry t;
            t.name = name.value_or(std::string{});
            t.format = timg[0];
            t.width = be16(timg + 2);
            t.height = be16(timg + 4);
            t.palette = timg[8] != 0;
            t.mipCount = std::max<uint8_t>(timg[0x18], 1);
            t.imageBytes = gxImageBytes(t.format, t.width, t.height, t.mipCount);
            const uint64_t image = header + be32(timg + 0x1C);
            if (!name || t.imageBytes == 0 || !inside(bmd.size(), image, t.imageBytes)) {
                return std::nullopt;
            }
            t.imageOffset = static_cast<uint32_t>(image);
            out.push_back(std::move(t));
        }
        return out;
    }
    return std::nullopt;
}

uint32_t gxImageBytes(uint8_t format, uint16_t width, uint16_t height, uint8_t mipCount) {
    const auto tile = tileShape(format);
    if (!tile || width == 0 || height == 0 || width > 4096 || height > 4096) {
        return 0;
    }
    uint32_t total = 0;
    uint32_t w = width, h = height;
    for (uint8_t level = 0; level < std::max<uint8_t>(mipCount, 1); level++) {
        total += ((w + tile->width - 1) / tile->width) * ((h + tile->height - 1) / tile->height) *
                 tile->bytes;
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
    }
    return total;
}

uint64_t hashBytes(const uint8_t* data, size_t size) {
    uint64_t h = 0xCBF29CE484222325ull ^ size;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, 8);
        h = (h ^ word) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < size; i++) {
        h = (h ^ data[i]) * 0x100000001B3ull;
    }
    return h;
}

namespace {

constexpr uint64_t kP1 = 0x9E3779B185EBCA87ull, kP2 = 0xC2B2AE3D27D4EB4Full, kP3 = 0x165667B19E3779F9ull,
                   kP4 = 0x85EBCA77C2B2AE63ull, kP5 = 0x27D4EB2F165667C5ull;

uint64_t rotl(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = v << 8 | p[i];
    }
    return v;
}

uint64_t xxRound(uint64_t acc, uint64_t input) {
    return rotl(acc + input * kP2, 31) * kP1;
}

uint64_t xxMerge(uint64_t acc, uint64_t val) {
    return (acc ^ xxRound(0, val)) * kP1 + kP4;
}

}  // namespace

uint64_t xxh64(const uint8_t* p, size_t len) {
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = kP1 + kP2, v2 = kP2, v3 = 0, v4 = 0 - kP1;
        for (; p + 32 <= end; p += 32) {
            v1 = xxRound(v1, le64(p));
            v2 = xxRound(v2, le64(p + 8));
            v3 = xxRound(v3, le64(p + 16));
            v4 = xxRound(v4, le64(p + 24));
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = xxMerge(xxMerge(xxMerge(xxMerge(h, v1), v2), v3), v4);
    } else {
        h = kP5;
    }
    h += len;
    for (; p + 8 <= end; p += 8) {
        h = rotl(h ^ xxRound(0, le64(p)), 27) * kP1 + kP4;
    }
    if (p + 4 <= end) {
        const uint64_t k = static_cast<uint64_t>(p[0]) | static_cast<uint64_t>(p[1]) << 8 |
                           static_cast<uint64_t>(p[2]) << 16 | static_cast<uint64_t>(p[3]) << 24;
        h = rotl(h ^ k * kP1, 23) * kP2 + kP3;
        p += 4;
    }
    for (; p < end; p++) {
        h = rotl(h ^ *p * kP5, 11) * kP1;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

}  // namespace twili::looks
