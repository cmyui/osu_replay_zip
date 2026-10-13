#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

namespace lzma {

struct properties {
	uint32_t dictSize{ 1u << 24 };
	uint32_t lc{ 3 }, lp{}, pb{ 2 };
	uint32_t algo{ 1 }, fb{ 32 }, mc{};
	bool btMode{ true };
};

std::vector<uint8_t> encode(std::span<const uint8_t> input, const properties& p,
	std::array<uint8_t, 5>& encoded_properties);

// Decode a raw LZMA stream with a known output size; end markers are optional.
bool decode(std::span<const uint8_t> input, std::span<const uint8_t, 5> properties,
	std::vector<uint8_t>& output, std::size_t expected_size);

}
