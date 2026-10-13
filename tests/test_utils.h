#pragma once
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <cstdint>

inline void require(bool ok, const char* message) {
	if (!ok)
		throw std::runtime_error(message);
}

inline std::vector<uint8_t> fixture_bytes(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	require(file.is_open(), "fixture could not be opened");
	return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}
