#pragma once

#include "orz_shared.h"

struct _stream_map {

	struct _elem {
		u8 tag;
		std::vector<u8> data;
	};

	std::vector<_elem> data;

	const std::vector<u8>& operator[](orz_tag tag) const {

		for (const auto& v : data) {

			if (v.tag != u8(tag))
				continue;

			return v.data;
		}

		static const std::vector<u8> empty{};

		return empty;
	}

};

namespace orz {

	inline bool lzma_decomp(const u8* data, size_t size, const _orz_data_block& block, std::vector<u8>& out) {

		if (block.raw_size > ORZ_MAX_STREAM)
			return false;

		u8 props[LZMA_PROPS_SIZE]{ block.props };

		memcpy(props + 1, &block.raw_size, 4);

		out.resize(block.raw_size);

		SizeT out_size{ block.raw_size }, input_size{ size };
		ELzmaStatus lzma_status{};

		if (LzmaDecode(out.data(), &out_size, data, &input_size, props,
			LZMA_PROPS_SIZE, LZMA_FINISH_END, &lzma_status, &lzma_allocFuncs) != SZ_OK)
			return false;

		out.resize(out_size);

		return true;
	}

}

namespace standard { bool decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R); }
namespace mania { void decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R); }
namespace taiko { void decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R); }
namespace fruits { u32 decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R); }

struct _decoded_orz {
	_stream_map streams;
	std::vector<_osr_frame> frames;
	u32 flags{};
};

static bool decode_frames(const std::vector<u8>& raw_bytes, const u32 options, _decoded_orz& out) {

	const auto container{ read_container(raw_bytes) };

	if (!container.valid)
		return false;

	const _orz_header* const header{ &container.header };

	_stream_map stream_data{};

	stream_data.data.resize(container.streams.size());

	for (size_t i{}; i < container.streams.size(); ++i) {

		const auto& s{ container.streams[i] };
		auto& o{ stream_data.data[i] };
		const u8* const data{ raw_bytes.data() + s.offset };

		o.tag = s.tag;

		if (!s.block.is_compressed)
			o.data.assign(data, data + s.block.size);
		else if (!orz::lzma_decomp(data, s.block.size, s.block, o.data))
			return false;

	}

	std::vector<_osr_frame> R(header->frame_count);

	{ // keys

		const std::vector<u8>& bs{ stream_data[orz_tag::key_bit_stream] };

		u32 flags{};

		if (bs.size() >= 4)
			memcpy(&flags, bs.data(), 4);

		size_t at{ 4 };

		const auto read_le = [&](size_t n) {

			u32 v{};

			for (size_t i{}; i < n; ++i, ++at)
				if (at < bs.size())
					v |= u32(bs[at]) << (8 * i);

			return v;
		};

		const auto read_key = [&](u32 flag_bits) {

			const i64 frames{ (i64)header->frame_count };

			i64 count{ -1 };
			size_t runs{};

			for (bool flag{}; count < frames && at < bs.size(); ++runs) {

				u32 v{ read_le(1) };

				if (v == 255 && (v = read_le(2)) == 65535)
					v = read_le(4);

				const i64 sustain{ 1 + (i64)v };

				if (flag)
					for (i64 i{ std::max(count, 0ll) }; i < std::min(count + sustain, frames); ++i)
						R[(size_t)i].keys |= flag_bits;

				count += sustain;
				flag = !flag;

			}

			at += runs & 1;

		};

		for (u32 key_check{ flags }; key_check; key_check &= key_check - 1)
			read_key(1u << std::countr_zero(key_check));

	}

	if (header->flags & ORZ_FLAG::mania_columns)
		mania::decode(header, stream_data, R);
	else if (header->flags & ORZ_FLAG::taiko_keys)
		taiko::decode(header, stream_data, R);
	else if (header->flags & ORZ_FLAG::fruits_catcher)
		fruits::decode(header, stream_data, R);
	else if (!standard::decode(header, stream_data, R))
		return false;

	// readd the closing seed frame
	if (const auto& seed{ stream_data[orz_tag::seed] }; seed.size() == 4)
		R.push_back({ -12345, 0.f, 0.f, *(u32*)seed.data() });

	if (header->flags & ORZ_FLAG::replay_verbatim) {

		const auto& data{ stream_data[orz_tag::osr_replay_data] };

		std::string text;

		// a score saved without a replay (no data) or data that never decompressed
		R = decompress_osr_string(data.data(), data.size(), text) ?
			orz_text::parse_frames(text.data(), text.size()) :
			std::vector<_osr_frame>{};

	}

	if (!(options & DECODE_SKIP_CRC)) {

		u32 crc{ crc32c(0, R.data(), R.size() * sizeof(_osr_frame)) };

		for (const orz_tag tag : CRC_PART_TAGS)
			if (const auto& part{ stream_data[tag] }; part.size())
				crc = crc32c_part(crc, part.data(), part.size());

		if (crc != header->crc)
			return false;

	}

	out = { std::move(stream_data), std::move(R), header->flags };

	return true;
}

// an ORZ file held in memory into frames; false for anything that is not a valid file
bool orz_decode_frames(const std::vector<u8>& raw_bytes, _decoded_orz& out, const u32 options = 0) {

	try {
		return decode_frames(raw_bytes, options, out);
	} catch (const std::exception&) {
		return false;
	}

}

std::string replay_text(const std::vector<_osr_frame>& R, u32 flags, const _stream_map& stream_data) {

	if (flags & ORZ_FLAG::replay_verbatim) {

		const auto& data{ stream_data[orz_tag::osr_replay_data] };

		std::string text;

		decompress_osr_string(data.data(), data.size(), text);

		return text;
	}

	std::string text{ orz_text::frames_text(R, (flags & ORZ_FLAG::text_shortest) != 0) };

	if (flags & ORZ_FLAG::trailing_comma)
		text += ',';

	return text;
}

static bool decode_to_osr(const char* input_file, const char* output_file, const u32 options) {

	const auto raw_bytes{ read_file(input_file) };

	_decoded_orz decoded;

	if (!orz_decode_frames(raw_bytes, decoded, options))
		return false;

	const auto& [stream_data, R, flags] { decoded };

	const std::vector<u8> compressed_string{ (flags & ORZ_FLAG::replay_verbatim) ?
		stream_data[orz_tag::osr_replay_data] :
		compress_osr_string(replay_text(R, flags, stream_data), (options & DECODE_OSU_STABLE_LZMA) != 0)
	};

	std::vector<u8> final_file_output{};

	if (flags & ORZ_FLAG::has_osr_header) {

		// header chunk, life graph, chunk up to the replay data length (stored after a 0x01 marker byte)

		const auto& header{ stream_data[orz_tag::osr_header] };

		add_to_u8(final_file_output, header.data(), header.size());

		if (const auto& life_raw{ stream_data[orz_tag::life_bar] }; life_raw.size()) {

			const std::string text{ life::decode(life_raw, R) };

			final_file_output.push_back(0x0b);

			for (u32 len{ (u32)text.size() }; ; ) {

				const u8 c{ u8(len & 0x7f) };

				len >>= 7;

				final_file_output.push_back(len ? c | 0x80 : c);

				if (!len)
					break;

			}

			final_file_output.insert(final_file_output.end(), text.begin(), text.end());

		} else
			final_file_output.push_back(0x00);

		if (const auto& after_life{ stream_data[orz_tag::osr_after_life] }; after_life.size())
			final_file_output.insert(final_file_output.end(), after_life.begin() + 1, after_life.end());

		add_to_u8(final_file_output, compressed_string.size());

	}

	add_to_u8(final_file_output, compressed_string);

	// bytes after the replay data (online score id, extra data), verbatim
	if (const auto& tail{ stream_data[orz_tag::osr_tail] }; tail.size())
		final_file_output.insert(final_file_output.end(), tail.begin() + 1, tail.end());

	return write_file(output_file, final_file_output);
}

bool orz_to_osr(const char* input_file, const char* output_file, const u32 options = 0) {

	try {
		return decode_to_osr(input_file, output_file, options);
	} catch (const std::exception&) {
		return false;
	}

}

bool orz_round_trip_ok(const std::vector<u8>& orz, const std::string& want) {

	_decoded_orz d;

	return orz_decode_frames(orz, d) && replay_text(d.frames, d.flags, d.streams) == want;
}

#include "orz_time.h"
#include "orz_encoder_std.h"
#include "orz_encoder_mania.h"
#include "orz_encoder_taiko.h"
#include "orz_encoder_fruits.h"
