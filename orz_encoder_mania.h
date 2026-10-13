#pragma once

#include <bit>
#include <vector>

namespace mania {

	FORCEINLINE std::vector<u8> column_changes(const std::vector<u32>& col) {

		std::vector<u8> c(col.size());

		for (size_t i{ 1 }; i < col.size(); ++i)
			c[i] = col[i] != col[i - 1];

		return c;
	}

	void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state) {

		std::vector<u32> col(r.size());

		for (size_t i{}; i < r.size(); ++i) {

			const auto x{ r[i].x };

			if (!(x >= 0.f && x < 16777216.f) || x != std::floor(x)) {

				// if any are invalid, fall back to std - could maybe save verbatim?
				standard::encode(output_file, r, flags & ~u32(ORZ_FLAG::gamemode_mania), state);

				return;
			}

			col[i] = u32(x);

		}

		_orz_construct result{ &state };

		result.frame_count = r.size();
		result.flags = flags | ORZ_FLAG::mania_columns;

		frame_time::write_grid(result, orz_tag::mania_time_rc, frame_time::mania, frame_time::deltas(r), column_changes(col));

		{ // scroll speed

			bit_stream s{};
			std::vector<std::pair<u32, float>> changes;

			{

				u32 last_value{ 0u };

				for (size_t i{}; i < r.size(); ++i) {

					const auto v = std::bit_cast<u32>(r[i].y);

					if (v == last_value)
						continue;

					last_value = v;
					changes.push_back({ (u32)i, r[i].y });

				}

			}

			write_bucket((u32)changes.size(), s, 4, 8, 16);

			u32 prev{};

			for (const auto& [f, v] : changes) {

				write_bucket(f - prev, s, 4, 8, 16);

				prev = f;

				write_float(v, s);

			}

			result.add_compress_stream(orz_tag::mania_scroll_speed, s, comp::pos_delta_prop());

		}

		_key_data_constructor kd{};

		for (const u32 c : col)
			kd.tick(c);

		result.frames_crc = _orz_construct::crc_of(r, state.seed);

		result.write(output_file, kd);
	}

	void decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R) {

		const size_t n{ R.size() };

		std::vector<u32> col(n);

		for (size_t i{}; i < n; ++i) {

			col[i] = R[i].keys;
			R[i].keys = 0;

		}

		const auto delta{ frame_time::read_grid(stream_data, orz_tag::mania_time_rc, frame_time::mania, column_changes(col)) };

		{

			const _bit_view s{ stream_data[orz_tag::mania_scroll_speed] };

			size_t bit{};
			u32 frame{};
			float speed{};

			std::vector<std::pair<u32, float>> changes(std::min<size_t>(read_bucket(s, bit, 4, 8, 16), s.max_size() / 36));

			for (auto& [time, v] : changes) {

				time = frame += read_bucket(s, bit, 4, 8, 16);

				v = read_float(s, bit);

			}

			size_t c{};

			for (size_t i{}; i < n; ++i) {

				while (c < changes.size() && changes[c].first == i)
					speed = changes[c++].second;

				R[i].y = speed;

			}

		}

		for (size_t i{}; i < n; ++i) {

			R[i].delta = delta[i];
			R[i].x = (float)col[i];

		}

	}

}
