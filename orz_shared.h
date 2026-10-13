#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <nmmintrin.h>
#include <fstream>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_M_FP_FAST) || defined(_M_FP_CONTRACT) || defined(__FAST_MATH__)
#error "breaks without strict IEEE float maths (/fp:precise)"
#endif
#if defined(_M_IX86) && _M_IX86_FP < 2
#error "needs SSE2 floating point (/arch:SSE2)"
#endif
static_assert(FLT_EVAL_METHOD == 0, "float must not use extended precision (x87)");

#define FORCEINLINE __forceinline

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int64_t i64;

// MSVC generates terrible code for std::bit_cast<float>(u32) - have to use this
FORCEINLINE float float_from_bits(u32 bits) {
	return _mm_cvtss_f32(_mm_castsi128_ps(_mm_cvtsi32_si128((int)bits)));
}

// same float bits (tells -0 from 0, NaNs apart)
FORCEINLINE bool same_bits(float a, float b) {
	return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

constexpr u32 ORZ_VERSION{ 0 };

// limits for untrusted input, real replays are far below all of them and anything beyond is treated as corrupted
constexpr size_t ORZ_MAX_FRAMES{ 1u << 22 }; // (~19 hours at 60 fps)
constexpr size_t ORZ_MAX_STREAM{ 1u << 28 };
constexpr size_t ORZ_MAX_STREAMS{ 256 };

enum ORZ_FLAG {

	gamemode_std = 1 << 0,
	gamemode_taiko = 1 << 1,
	gamemode_fruits = 1 << 2,
	gamemode_mania = 1 << 3,

	has_osr_header = 1 << 4,

	trailing_comma = 1 << 5,

	replay_verbatim = 1 << 6,

	text_shortest = 1 << 7, // floats are 'shortest round-trip' (newer .NET, osu!lazer)
	mania_columns = 1 << 8, // osu!mania column / scroll speed streams

	taiko_keys = 1 << 9, // osu!taiko positions from the keys, rc time
	fruits_catcher = 1 << 10, // catcher positions against a movement prediction

	// osu! standard replay that has k1 without m1 set
	// mostly for osu!auto generated replays
	keys_unfolded = 1 << 11, 

};

enum ORZ_OPTION : u32 {

	ENCODE_CHECK_ROUND_TRIP = 1 << 0,

	DECODE_OSU_STABLE_LZMA = 1 << 1,

	DECODE_SKIP_CRC = 1 << 2,

};

enum class orz_tag : u8 {

	time_delta_table = 0,

	key_bit_stream,

	osr_header,
	osr_after_life,
	osr_tail,

	seed,

	life_bar,

	time_delta_rc,
	osr_replay_data,

	device_space_info,
	device_escape,
	device_escape_values,
	device_moves_rc,

	mania_time_rc,
	mania_scroll_speed,

	taiko_time_rc,
	taiko_escape,

	fruits_time_rc,
	fruits_x_rc,
	fruits_escape,

};

#include "lzma.h"

#ifndef ON_SCOPE_EXIT

template <typename F> struct on_scope_exit {
private: F func;
public:
	on_scope_exit(const on_scope_exit&) = delete;
	on_scope_exit& operator=(const on_scope_exit&) = delete;

	constexpr on_scope_exit(F&& f) : func(std::forward<F>(f)) {}
	constexpr ~on_scope_exit() { func(); }
};

#define PCAT0(x, y) PCAT1(x, y)
#define PCAT1(x, y) PCAT2(!, x ## y)
#define PCAT2(x, r) r
#define ON_SCOPE_EXIT(...) on_scope_exit PCAT0(__scope, __LINE__) {[&]{__VA_ARGS__}}

#endif

template<typename T>
bool write_file(const char* file_name, const T& data) {

	std::ofstream file(file_name, std::ios::binary);

	if (!file.is_open())
		return false;

	file.write((const char*)data.data(), data.size());

	return true;
}

std::vector<u8> read_file(const char* file_name) {

	std::ifstream file(file_name, std::ios::binary | std::ios::ate | std::ios::in);

	if (!file.is_open()) [[unlikely]]
		return {};

	const size_t file_size{ (size_t)file.tellg() };

	std::vector<u8> ret(file_size);

	if (file_size) [[likely]] {

		file.seekg(0, std::ios::beg);
		file.read((char*)ret.data(), file_size);

	}

	return ret;
}

struct bit_stream {

	std::vector<u8> data;
	u8 in_count{};

	void clear() {

		in_count = 0;
		data.clear();

	}

	size_t max_size() const {
		return data.size() * 8;
	}

	size_t size() const {

		size_t r{ in_count };

		if (data.size() > 0)
			r += (data.size() - 1) * 8;

		return r;
	}

	void push_back(const bool v) {

		if (in_count > 7 || data.size() == 0) {

			in_count = 0;
			data.push_back({});

		}

		data.back() |= u8(v) << (in_count++);

	}

	// bits past the end read as 0, so a short or damaged stream can never read outside data
	bool operator[](size_t i) const {

		const size_t bucket{ i >> 3 };

		return bucket < data.size() && ((data[bucket] >> (i & 7)) & 1);
	}

	void append(const bit_stream& v) {

		for (size_t i{}, size{ v.size() }; i < size; ++i)
			push_back(v[i]);

	}

};

struct _bit_view {

	std::span<const u8> data;

	_bit_view(std::span<const u8> bytes) : data{ bytes } {}
	_bit_view(const bit_stream& bits) : data{ bits.data } {}

	size_t max_size() const {
		return data.size() * 8;
	}

	bool operator[](size_t i) const {

		const size_t bucket{ i >> 3 };

		return bucket < data.size() && ((data[bucket] >> (i & 7)) & 1);
	}

};

// raw float, 32 bits
inline void write_float(float v, bit_stream& o) {

	const u32 b{ std::bit_cast<u32>(v) };

	for (int i{}; i < 32; ++i)
		o.push_back((b >> i) & 1);

}

inline float read_float(const _bit_view in, size_t& i) {

	u32 b{};

	for (int k{}; k < 32; ++k)
		b |= u32(in[i++]) << k;

	return float_from_bits(b);
}

template<size_t BIT_WRITE>
void small_write(u32 value, bit_stream& output) {

	if constexpr (BIT_WRITE == 1)
		return output.push_back(value);

	constexpr auto MAX_VALUE{ (1 << BIT_WRITE) - 1 };

	while (value >= MAX_VALUE) {

		for (size_t i{}; i < BIT_WRITE; ++i)
			output.push_back(1);

		const auto in{ 31 - std::countl_zero(value) };

		// only the bits under the most significant bit are stored
		for (size_t i{ BIT_WRITE - 1 }; i < in; ++i)
			output.push_back(1);

		output.push_back(0);

		for (size_t i{}; i < in; ++i)
			output.push_back((value >> i) & 1);

		return;
	}

	for (size_t i{}; i < BIT_WRITE; ++i)
		output.push_back((value >> i) & 1);

}

template<size_t BIT_WRITE>
u32 small_read(const _bit_view input, size_t& i) {

	if (i >= input.max_size())
		return 0;

	if constexpr (BIT_WRITE == 1)
		return input[i++];

	constexpr auto MAX_VALUE{ (1 << BIT_WRITE) - 1 };

	u32 value{}, c{};

	for (size_t size{ input.max_size() }; c < BIT_WRITE && (i + c) < size; ++c)
		value |= u32(input[i + c]) << c;

	i += c;
	c = 0;

	if (value != MAX_VALUE)
		return value;

	int bit_count{ BIT_WRITE - 1 };

	for (size_t size{ input.max_size() }; (i + c) < size; ++c)
		if (input[i + c] == 0)
			break;

	bit_count += c;

	if (bit_count > 31)
		bit_count = 31;

	i += c + 1;
	c = 0;
	value = 1 << bit_count;

	for (size_t size{ input.max_size() }; c < bit_count && (i + c) < size; ++c)
		value |= u32(input[i + c]) << c;

	i += bit_count;

	return value;
}

template<u32 OVERFLOW_BITS = 5, typename ...T >
void write_bucket(u32 value, bit_stream& output, const T... bits) {

	bool done{};

	const auto write = [&](const size_t b) {

		if (b == 0 || done)
			return;

		const u32 MAX_VALUE{ (1u << b) - 1 };

		if (value < MAX_VALUE) {

			for (size_t i{}; i < b; ++i)
				output.push_back((value >> i) & 1);

			done = true;

			return;
		}

		for (size_t i{}; i < b; ++i)
			output.push_back(1);

		value -= MAX_VALUE;

	};

	(write(bits), ...);

	if (done)
		return;

	value += 1;

	const auto in{ (size_t)31 - std::countl_zero(value) };

	for (size_t i{}; i < OVERFLOW_BITS; ++i)
		output.push_back((in >> i) & 1);

	for (size_t i{}; i < in; ++i)
		output.push_back((value >> i) & 1);

}

template<u32 OVERFLOW_BITS = 5, typename ...T >
u32 read_bucket(const _bit_view input, size_t& i, const T... bits) {

	u32 result{};

	const auto max_size{ input.max_size() };

	bool done{};

	const auto read = [&](const size_t b) {

		if (b == 0 || done)
			return;

		u32 v{};

		ON_SCOPE_EXIT(result += v; );

		for (size_t z{}; z < b && i < max_size; ++z)
			v |= u32(input[i++] != 0) << z;

		if (v != (1u << b) - 1)
			done = true;

	};

	(read(bits), ...);

	if (done)
		return result;

	u32 bit_count{};

	for (size_t z{}; z < OVERFLOW_BITS && i < max_size; ++z)
		bit_count |= u32(input[i++] != 0) << z;

	u32 v{ (u32)1 << bit_count };

	for (size_t z{}; z < bit_count && i < max_size; ++z)
		v |= u32(input[i++] != 0) << z;

	return result + v - 1;
}

constexpr u8 ORZ_MAGIC{ 'r' };

struct _orz_header {
	u32 version, flags, frame_count, data_count;
	u32 crc; // of the decoded replay
};

struct _orz_data_block {
	u32 size;
	bool is_compressed;
	u8 props;
	u32 raw_size;
};

struct _orz_container {

	struct _stream {
		u8 tag;
		_orz_data_block block;
		size_t offset;
	};

	bool valid{};
	bool newer{};
	_orz_header header{};
	std::vector<_stream> streams;
	size_t header_bytes{};
};

inline _orz_container read_container(const std::vector<u8>& file) {

	_orz_container c{};

	if (file.size() < 2 || file[0] != ORZ_MAGIC)
		return c;

	// the header is at most a few KB (ORZ_MAX_STREAMS streams of ~90 bits)
	const _bit_view bs{ std::span{ file }.subspan(1, std::min<size_t>(file.size() - 1, 4096)) };

	size_t i{};
	auto& h{ c.header };

	h.version = read_bucket(bs, i, 4, 8);

	// we are trying to read a orz file from the future. abort.
	if (h.version > ORZ_VERSION) {

		c.newer = true;

		return c;
	}

	h.flags = read_bucket(bs, i, 12);

	for (int k{}; k < 32; ++k)
		h.crc |= u32(small_read<1>(bs, i)) << k;

	h.frame_count = read_bucket(bs, i, 16);
	h.data_count = read_bucket(bs, i, 5);

	if (h.data_count > ORZ_MAX_STREAMS || h.frame_count > ORZ_MAX_FRAMES)
		return c;

	c.streams.resize(h.data_count);

	for (auto& s : c.streams) {

		const u32 tag{ read_bucket(bs, i, 5) };

		s.block.is_compressed = small_read<1>(bs, i) != 0;
		s.block.size = read_bucket(bs, i);

		if (s.block.is_compressed) {

			const u32 props{ read_bucket(bs, i, 8) };

			s.block.raw_size = read_bucket(bs, i);

			if (props > 255 || s.block.raw_size > ORZ_MAX_STREAM)
				return c;

			s.block.props = (u8)props;

		}

		if (tag > 255)
			return c;

		s.tag = (u8)tag;

	}

	c.header_bytes = 1 + (i + 7) / 8;

	if (c.header_bytes > file.size())
		return c;

	size_t offset{ c.header_bytes };

	for (auto& s : c.streams) {

		if (s.block.size > file.size() - offset)
			return c;

		s.offset = offset;
		offset += s.block.size;

	}

	c.valid = true;

	return c;
}

struct _osr_frame {
	int delta;
	float x, y;
	u32 keys;
};

using _osr = std::vector<_osr_frame>;

static_assert(sizeof(_osr_frame) == 16, "replay_crc hashes frames as 16 bytes");

inline u32 crc32c(u32 crc, const void* data, size_t size) {

	const u8* p{ (const u8*)data };

#if defined(_M_X64)

	u64 c{ crc };

	for (; size >= 8; size -= 8, p += 8) {

		u64 v;

		memcpy(&v, p, 8);

		c = _mm_crc32_u64(c, v);

	}

	u32 c32{ (u32)c };

#else

	u32 c32{ crc };

	for (; size >= 4; size -= 4, p += 4) {

		u32 v;

		memcpy(&v, p, 4);

		c32 = _mm_crc32_u32(c32, v);

	}

#endif

	for (; size; --size)
		c32 = _mm_crc32_u8(c32, *p++);

	return c32;
}

FORCEINLINE u32 crc32c_frame(u32 crc, const _osr_frame& f) {
	return crc32c(crc, &f, sizeof f);
}

constexpr orz_tag CRC_PART_TAGS[]{
	orz_tag::osr_header,
	orz_tag::life_bar,
	orz_tag::osr_after_life,
	orz_tag::osr_tail,
	orz_tag::osr_replay_data
};

FORCEINLINE u32 crc32c_part(u32 crc, const u8* data, size_t size) {

	const u32 n{ (u32)size };

	return crc32c(crc32c(crc, &n, 4), data, size);
}

inline bool decompress_osr_string(const u8* data, size_t size, std::string& out) {

	out.clear();

	if (size <= 5 + 8)
		return false;

	u32 text_size;

	memcpy(&text_size, data + 5, 4);

	if (text_size > ORZ_MAX_STREAM)
		return false;

	std::vector<u8> decoded;
	if (!lzma::decode({ data + 13, size - 13 }, std::span<const u8, 5>{ data, 5 }, decoded, text_size))
		return false;

	out.assign(decoded.begin(), decoded.end());
	return true;
}

inline std::vector<u8> compress_osr_string(std::string_view string, bool stable) {

	lzma::properties p{};

	if (stable) {

		p.dictSize = 1u << 21;
		p.fb = 255;

	} else {

		p.algo = 0;
		p.btMode = false;
		p.fb = 32;
		p.mc = 16;
		p.dictSize = (u32)std::clamp<size_t>(string.size(), 1 << 12, 1 << 24);

	}

	std::array<u8, 5> props;
	auto compressed{ lzma::encode({ (const u8*)string.data(), string.size() }, p, props) };
	std::vector<u8> output(13);
	std::copy(props.begin(), props.end(), output.begin());
	const u64 size{ string.size() };
	memcpy(output.data() + 5, &size, 8);
	output.insert(output.end(), compressed.begin(), compressed.end());
	return output;
}

#include "orz_state.h"

#include "device_space.h"
#include "orz_life.h"
