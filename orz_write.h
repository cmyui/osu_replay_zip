#pragma once

#include <algorithm>
#include <cmath>

void add_to_u8(std::vector<u8>& output, u32 input) {

	const auto i{ output.size() };

	output.resize(i + 4);

	memcpy(output.data() + i, &input, 4);
}

void add_to_u8(std::vector<u8>& output, const void* src, size_t size) {

	const auto i{ output.size() };

	output.resize(i + size);

	memcpy(output.data() + i, src, size);
}

template<typename T>
void add_to_u8(std::vector<u8>& output, const std::vector<T>& input) {

	constexpr auto SIZE{ sizeof(T) };

	const auto i{ output.size() };

	output.resize(i + input.size() * SIZE);

	memcpy(output.data() + i, input.data(), input.size() * SIZE);
}

// unsigned leb128, as .osr strings give their length
void add_uleb128(std::vector<u8>& output, u32 v) {

	for (; v > 0x7f; v >>= 7)
		output.push_back(u8(v | 0x80));

	output.push_back(u8(v));
}

std::pair<u32, u32>& unique_add(u32 v, std::vector<std::pair<u32, u32>>& b) {

	for (auto& t : b) {

		if (t.first == v) {

			++t.second;

			return t;
		}

	}

	return b.emplace_back(v, 1);
}

const auto pair_sort = [](const auto& x, const auto& y) {

	return x.second > y.second;
};

inline bit_stream position_escapes(const _osr& r, const std::vector<u32>& esc, float dummy_x, float dummy_y) {

	bit_stream s{};

	write_bucket((u32)esc.size(), s, 4, 8, 16);

	u32 prev{};

	for (const u32 i : esc) {

		write_bucket(i - prev, s, 4, 8, 16);

		prev = i;

		const bool dummy{ same_bits(r[i].x, dummy_x) && same_bits(r[i].y, dummy_y) };

		s.push_back(dummy);

		if (!dummy) {

			write_float(r[i].x, s);
			write_float(r[i].y, s);

		}

	}

	return s;
}

namespace comp {

	CLzmaEncProps base_prop(u32 lc, u32 lp) {

		CLzmaEncProps p;

		LzmaEncProps_Init(&p);

		p.level = 5;
		p.fb = 32;
		p.mc = 16;
		p.numHashBytes = 4;
		p.lc = lc;
		p.lp = lp;
		p.pb = 0;
		p.numThreads = 1;

		return p;
	}

	CLzmaEncProps pos_delta_prop() {
		return base_prop(0, 0);
	}

	CLzmaEncProps composite_key_prop() {
		return base_prop(0, 1);
	}

	bool compress(const bit_stream& b, CLzmaEncProps prop, std::vector<u8>& out, u8& props_byte) {

		out.resize(std::max<size_t>(size_t(b.data.size() * 1.5), 1024));

		size_t len{ out.size() };

		prop.dictSize = (u32)std::min<size_t>(b.data.size(), 1 << 24);

		u8 props[LZMA_PROPS_SIZE];
		SizeT propsSize = LZMA_PROPS_SIZE;

		if (LzmaEncode(out.data(), &len, b.data.data(), b.data.size(),
			&prop, props, &propsSize, 0,
			0,
			&lzma_allocFuncs, &lzma_allocFuncs
		) != SZ_OK)
			throw std::runtime_error("LZMA encode failed");

		out.resize(len);

		props_byte = props[0];

		return len < b.data.size();
	}

}

struct _key_data_constructor {

	u32 previous{};

	std::array<std::vector<u32>, 32> key_sustain;

	_key_data_constructor() {

		for (auto& k : key_sustain)
			k.push_back(0);

	}

	void tick(const u32 keys) {

		const u32 delta{ keys ^ previous };

		previous = keys;

		for (size_t i{}; i < key_sustain.size(); ++i) {

			if ((delta >> i) & 1)
				key_sustain[i].push_back(0);
			else
				++key_sustain[i].back();

		}

	}

};

struct _orz_construct {

	constexpr static size_t MAX_ELEMENTS{ 256 };

	orz_state* state;

	u32 flags{}, frame_count{}, data_count{};
	u32 frames_crc{};

	orz_tag tag_table[MAX_ELEMENTS];
	_orz_data_block data_table[MAX_ELEMENTS];

	std::vector<u8> file_data;

	void add_compress_stream(orz_tag tag, const bit_stream& data, const CLzmaEncProps& props) {

		if (data_count == 255 || data.size() == 0) [[unlikely]]
			return;

		ON_SCOPE_EXIT( ++data_count; );

		tag_table[data_count] = tag;

		auto& dt{ data_table[data_count] };

		std::vector<u8> c;
		u8 lzma_props{};

		if (!comp::compress(data, props, c, lzma_props)) {

			dt.is_compressed = false;
			dt.size = (u32)data.data.size();

			add_to_u8(file_data, data.data);

		} else {

			dt.is_compressed = true;
			dt.size = (u32)c.size();
			dt.props = lzma_props;
			dt.raw_size = (u32)data.data.size();

			add_to_u8(file_data, c);

		}

	}

	void add_stream(orz_tag tag, const void* src, size_t size) {

		if (data_count == 255 || size == 0) [[unlikely]]
			return;

		ON_SCOPE_EXIT( ++data_count; );

		tag_table[data_count] = tag;

		auto& dt{ data_table[data_count] };

		dt.is_compressed = false;
		dt.size = (u32)size;

		add_to_u8(file_data, src, size);

	}

	void append(const _orz_construct& o) {

		for (size_t i{}; i < o.data_count && data_count < 255; ++i) {

			tag_table[data_count] = o.tag_table[i];
			data_table[data_count] = o.data_table[i];

			++data_count;

		}

		add_to_u8(file_data, o.file_data);

	}

	void write(const char* output_file, const _key_data_constructor& kd) {

		add_osr_parts();
		add_key_stream(kd);
		finish(output_file);

	}

	void add_osr_parts() {

		const auto& st{ *state };

		{ // header, bytes after the life bar, bytes after the replay data: each a leb128 size, then the bytes

			std::vector<u8> parts;

			for (const auto* bytes : { &st.osr_header, &st.after_life, &st.tail }) {

				add_uleb128(parts, (u32)bytes->size());

				parts.insert(parts.end(), bytes->begin(), bytes->end());

			}

			if (parts.size() > 3)
				add_stream(orz_tag::osr_parts, parts.data(), parts.size());

		}

		if (st.replay_data.present)
			add_stream(orz_tag::osr_replay_data, st.replay_data.bytes.data(), st.replay_data.bytes.size());

		if (st.life.present) {

			const auto bytes{ life::encode(st.life.text, st.input) };

			add_stream(orz_tag::life_bar, bytes.data(), bytes.size());

		}

	}

	void add_key_stream(const _key_data_constructor& kd) {

		bit_stream key_data{};

		u32 active_flags{};

		for (size_t i{}; i < kd.key_sustain.size(); ++i)
			active_flags |= (kd.key_sustain[i].size() > 1) << i;

		add_to_u8(key_data.data, active_flags);

		auto& o{ key_data.data };

		for (const auto& ks : kd.key_sustain) {

			if (ks.size() <= 1)
				continue;

			for (const u32 v : ks) {

				if (v < 255) {

					o.push_back((u8)v);

					continue;
				}

				o.push_back(255);

				if (v < 65535) {

					o.push_back(u8(v));
					o.push_back(u8(v >> 8));

					continue;
				}

				o.push_back(255);
				o.push_back(255);

				add_to_u8(o, v);

			}

			if (ks.size() & 1)
				o.push_back(0);

		}

		add_compress_stream(orz_tag::key_bit_stream, key_data, comp::composite_key_prop());

	}

	static u32 crc_of(std::span<const _osr_frame> frames, const _seed_frame& seed) {

		u32 crc{ crc32c(0, frames.data(), frames.size() * sizeof(_osr_frame)) };

		if (seed.present)
			crc = crc32c_frame(crc, { -12345, 0.f, 0.f, seed.value });

		return crc;
	}

	void finish(const char* output_file) {

		// the CRC continues over the stored .osr parts, in CRC_PART_TAGS order
		u32 crc{ frames_crc };

		for (const orz_tag tag : CRC_PART_TAGS) {

			size_t offset{};

			for (size_t i{}; i < data_count; offset += data_table[i++].size) {

				if (tag_table[i] == tag) {

					crc = crc32c_part(crc, file_data.data() + offset, data_table[i].size);

					break;
				}

			}

		}

		bit_stream h{};

		write_bucket(ORZ_VERSION, h, 4, 8);
		write_bucket(flags, h, 12);

		for (int k{}; k < 32; ++k)
			h.push_back((crc >> k) & 1);

		h.push_back(state->seed.present);

		for (int k{}; state->seed.present && k < 32; ++k)
			h.push_back((state->seed.value >> k) & 1);

		write_bucket(frame_count, h, 16);
		write_bucket(data_count, h, 3);

		for (size_t i{}; i < data_count; ++i) {

			const auto& dt{ data_table[i] };

			write_bucket((u32)tag_table[i], h, 4);
			h.push_back(dt.is_compressed);
			write_bucket(dt.size, h);

			if (dt.is_compressed) {

				write_bucket(dt.props, h, 8);
				write_bucket(dt.raw_size, h);

			}

		}

		std::vector<u8> final_file{};

		final_file.reserve(1 + h.data.size() + file_data.size());

		final_file.push_back(ORZ_MAGIC);

		add_to_u8(final_file, h.data);
		add_to_u8(final_file, file_data);

		if (strlen(output_file))
			write_file(output_file, final_file);

		state->output = std::move(final_file);

	}

};

namespace standard { void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state); }
namespace mania { void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state); }
namespace taiko { void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state); }
namespace fruits { void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state); }

void encode_replay(const char* output_file, _osr& r, const u32 flags, orz_state& state, bool relax) {

	if (flags & ORZ_FLAG::gamemode_mania)
		return mania::encode(output_file, r, flags, state);

	if (flags & ORZ_FLAG::gamemode_taiko)
		return taiko::encode(output_file, r, flags, state);

	// CTB replays with relax map the catcher to the x of the cursor, encode the replay through the standard encoder to catch this
	if ((flags & ORZ_FLAG::gamemode_fruits) && !relax)
		return fruits::encode(output_file, r, flags, state);

	standard::encode(output_file, r, relax ? flags | ORZ_FLAG::relax : flags, state);
}

bool orz_round_trip_ok(const std::vector<u8>& orz, const std::string& want);

static void encode_osr(const char* in_file, const char* out_file, u32 flags, orz_state& state) {

	{

		orz_state fresh{};

		fresh.options = state.options;
		fresh.debug = state.debug;

		state = std::move(fresh);

	}

	const auto raw_file{ read_file(in_file) };

	if (raw_file.size() < 128)
		return;

	const size_t file_size{ raw_file.size() };

	size_t pos{};

	const auto skip = [&](size_t n) {

		if (n > file_size - pos)
			return false;

		pos += n;

		return true;
	};

	const auto skip_string = [&]() {

		if (pos >= file_size)
			return false;

		if (raw_file[pos++] != 0x0b)
			return true;

		u32 len{};

		for (int k{}; ; ++k) {

			if (pos >= file_size || k == 5)
				return false;

			const u8 c{ raw_file[pos++] };

			len |= u32(c & 0x7f) << (7 * k);

			if (!(c & 0x80))
				break;

		}

		return skip(len);
	};

	size_t lzma_offset{}, lzma_size{ file_size };

	bool relax{ (state.options & ENCODE_RELAX) != 0 };

	auto& header_bytes{ state.osr_header };

	if (flags & ORZ_FLAG::has_osr_header) {

		if (raw_file[0] > 3) // if the osr isnt for one of the main 4 game modes, abort
			return;

		flags |= 1u << raw_file[0];

		const bool ok {
			skip(1) && skip(4) && // gamemode, game version
			skip_string() && skip_string() && skip_string() // beatmap hash, player name, replay hash
		};

		if (!ok)
			return;

		#pragma pack(push, 1)

		struct _osr_header_values {

			i16 hit_300, hit_100, hit_50, geki, katsu, misses;

			i32 score;
			u16 combo;
			u8 perfect;

			u32 mods;

		};

		#pragma pack(pop)

		const auto* header_values{ (const _osr_header_values*)(raw_file.data() + pos) };

		if (!skip(sizeof(_osr_header_values)))
			return;

		relax |= (header_values->mods & OSR_MOD_RELAX) != 0;

		const size_t life_start{ pos };

		if (!skip_string())
			return;

		const size_t life_end{ pos };

		if (life_end - life_start >= 2 && raw_file[life_start] == 0x0b) {

			size_t q{ life_start + 1 };

			while (q < life_end && (raw_file[q] & 0x80))
				++q;

			if (q < life_end)
				++q;

			state.life = { true, std::string((const char*)raw_file.data() + q, (const char*)raw_file.data() + life_end) };

		}

		if (!skip(8)) // time stamp
			return;

		header_bytes.assign(raw_file.begin(), raw_file.begin() + life_start);

		state.after_life.assign(raw_file.begin() + life_end, raw_file.begin() + pos);

		if (!state.life.present && life_end > life_start && raw_file[life_start] != 0)
			state.life = { true, std::string() };

		if (pos + 4 > file_size)
			return;

		u32 compressed{};

		memcpy(&compressed, raw_file.data() + pos, 4);

		pos += 4;

		lzma_offset = pos;
		lzma_size = file_size - pos;

		if (compressed <= lzma_size) {

			state.tail.assign(raw_file.begin() + pos + compressed, raw_file.end());
			lzma_size = compressed;

		}

	}

	const u8* const lzma_start{ raw_file.data() + lzma_offset };

	const auto write_verbatim = [&]() {

		state.seed = {};
		state.exact_text = false;
		state.replay_data = { true, std::vector<u8>(lzma_start, lzma_start + lzma_size) };

		_orz_construct result{ &state };

		result.flags = flags | ORZ_FLAG::replay_verbatim;
		result.frames_crc = _orz_construct::crc_of(state.input, {});
		result.write(out_file, _key_data_constructor{});

	};

	state.input_size = lzma_size;

	if (!decompress_osr_string(lzma_start, lzma_size, state.text)) {

		write_verbatim();

		return;
	}

	_osr replay_stream{ orz_text::parse_frames(state.text.data(), state.text.size()) };

	state.input = replay_stream;

	if (replay_stream.size() > ORZ_MAX_FRAMES)
		replay_stream.clear(); // format can not store this many frames

	// only trust the frames if the text writer gives back this exact text, else the replay data is stored as is
	u32 text_flags{};

	state.exact_text = false;

	{

		std::string_view body{ state.text };

		if (body.size() && body.back() == ',') {

			text_flags |= ORZ_FLAG::trailing_comma;
			body.remove_suffix(1);

		}

		if (replay_stream.empty() || orz_text::frames_text(replay_stream, state.exact_text) != body) {

			state.exact_text = true;

			if (orz_text::frames_text(replay_stream, state.exact_text) == body)
				text_flags |= ORZ_FLAG::text_shortest;
			else {

				state.exact_text = false;
				text_flags |= ORZ_FLAG::replay_verbatim;

			}

		}

	}

	{

		const float x_limit{ (flags & ORZ_FLAG::gamemode_mania) ? 16777216.f : 131072.f };

		for (const auto& f : replay_stream)
			if (!(std::abs(f.x) <= x_limit) || !(std::abs(f.y) <= 131072.f)) // catches NaN too
				text_flags |= ORZ_FLAG::replay_verbatim;

	}

	if (text_flags & ORZ_FLAG::replay_verbatim) {

		write_verbatim();

		return;
	}

	state.seed = {};

	if (replay_stream.size()) {

		const auto& b{ replay_stream.back() };

		if (b.delta == -12345 && b.x == 0.f && b.y == 0.f) {

			state.seed = { true, b.keys };
			replay_stream.pop_back();

		}

	}

	encode_replay(out_file, replay_stream, flags | text_flags, state, relax);

	if ((state.options & ENCODE_CHECK_ROUND_TRIP) && !orz_round_trip_ok(state.output, state.text))
		write_verbatim();

}

void osr_to_orz(const char* in_file, const char* out_file, const u32 flags, orz_state& state) {

	try {
		encode_osr(in_file, out_file, flags, state);
	} catch (const std::exception&) {
		state.output.clear(); // nothing is written for input the encoder can not handle
	}

}
