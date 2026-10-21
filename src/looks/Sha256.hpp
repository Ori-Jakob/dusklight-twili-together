#pragma once

// SHA-256 (FIPS 180-4), for entries we read out of archives ourselves.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace twili::looks {

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);

}  // namespace twili::looks
