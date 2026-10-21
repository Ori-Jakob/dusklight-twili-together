#pragma once

// Bounded readers for disc files: Yaz0, RARC archives and a BMD's texture table. Every offset is
// checked, so a broken or hostile file gives nullopt instead of reading out of bounds.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace twili::looks {

bool isYaz0(std::span<const uint8_t> data);
// The decompressed stream, or nullopt when it is not Yaz0, is cut short or is over maxOut bytes.
std::optional<std::vector<uint8_t>> yaz0Decode(std::span<const uint8_t> data, size_t maxOut);

struct RarcFile {
    std::string name;
    uint32_t offset = 0;  // from the start of the archive
    uint32_t size = 0;
    uint8_t flags = 0;    // 0x04 compressed, 0x80 with Yaz0
};

// Every file of an uncompressed RARC (directories left out), or nullopt when it is malformed.
std::optional<std::vector<RarcFile>> rarcFiles(std::span<const uint8_t> arc);
// The first file named `name` (ASCII case-insensitive), decompressed when stored with Yaz0.
std::optional<std::vector<uint8_t>> rarcRead(std::span<const uint8_t> arc, const std::string& name,
    size_t maxOut);
// One file of rarcFiles(arc), decompressed when stored with Yaz0.
std::optional<std::vector<uint8_t>> rarcReadFile(std::span<const uint8_t> arc, const RarcFile& file,
    size_t maxOut);

struct RarcPath {
    std::string path;  // "root/dir/name", lowercase: where JKRArchive looks for an entry overlay
    RarcFile file;
};
// The files reachable from the root node, in rarcFiles order, or nullopt when it is malformed.
std::optional<std::vector<RarcPath>> rarcPaths(std::span<const uint8_t> arc);

struct Tex1Entry {
    std::string name;
    uint8_t format = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t mipCount = 1;
    bool palette = false;
    uint32_t imageOffset = 0;  // from the start of the BMD
    uint32_t imageBytes = 0;
};

// The TEX1 entries of a J3D model in table order, or nullopt when it has none or is malformed.
std::optional<std::vector<Tex1Entry>> bmdTextures(std::span<const uint8_t> bmd);

// Bytes of a GX image with its mip levels; 0 for a format it does not know.
uint32_t gxImageBytes(uint8_t format, uint16_t width, uint16_t height, uint8_t mipCount);
// A 64-bit content hash (not cryptographic): what a texture's bytes are compared by.
uint64_t hashBytes(const uint8_t* data, size_t size);
// XXH64 with seed 0: Aurora's content key of a texture's base mip.
uint64_t xxh64(const uint8_t* data, size_t size);

}  // namespace twili::looks
