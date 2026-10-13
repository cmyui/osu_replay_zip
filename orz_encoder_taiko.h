#pragma once

#include <bit>
#include <vector>

namespace taiko {

	FORCEINLINE float mouse_x(u32 keys) {

		return (keys & 3) ? 0.f :
			((keys & 12) ? 640.f : 320.f);
	}

	constexpr float MOUSE_Y{ 9999.f };
	constexpr float DUMMY_X{ 256.f }, DUMMY_Y{ -500.f };

	void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state) {

		_orz_construct result{ &state };

		result.frame_count = r.size();
		result.flags = flags | ORZ_FLAG::taiko_keys;

		frame_time::write_grid(result, orz_tag::taiko_time_rc, frame_time::taiko, frame_time::deltas(r), frame_time::key_updates(r));

		{

			std::vector<u32> esc{};

			// seems to be from pausing? .x can rarely be 0 with no key so we need to account for that
			for (size_t i{}; i < r.size(); ++i)
				if (!same_bits(r[i].x, mouse_x(r[i].keys)) || !same_bits(r[i].y, MOUSE_Y))
					esc.push_back((u32)i);

			const bit_stream s{ position_escapes(r, esc, DUMMY_X, DUMMY_Y) };

			result.add_compress_stream(orz_tag::taiko_escape, s, comp::pos_delta_prop());

		}

		_key_data_constructor kd{};

		for (const auto& f : r)
			kd.tick(f.keys);

		result.frames_crc = _orz_construct::crc_of(r, state.seed);

		result.write(output_file, kd);

	}

	void decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R) {

		const size_t n{ R.size() };

		const auto delta{ frame_time::read_grid(stream_data, orz_tag::taiko_time_rc, frame_time::taiko, frame_time::key_updates(R)) };

		for (size_t i{}; i < n; ++i)
			R[i].delta = delta[i];

		for (auto& f : R) {

			f.x = mouse_x(f.keys);
			f.y = MOUSE_Y;

		}

		const _bit_view s{ stream_data[orz_tag::taiko_escape] };

		size_t bit{};
		u32 frame{};

		
		for (size_t e{ std::min<size_t>({ read_bucket(s, bit, 4, 8, 16), s.max_size() / 5, n }) }; e--; ) {

			frame += read_bucket(s, bit, 4, 8, 16);

			if (frame >= n)
				break;

			if (s[bit++]) {

				R[frame].x = DUMMY_X;
				R[frame].y = DUMMY_Y;

				continue;
			}

			R[frame].x = read_float(s, bit);
			R[frame].y = read_float(s, bit);

		}

	}

}
