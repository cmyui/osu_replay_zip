#pragma once

#include "orz_device_std.h"

#include <algorithm>
#include <climits>
#include <array>
#include <vector>

namespace standard {

	void encode_delta_time(const _osr& r, _orz_construct& orz) {

		if (r.size() < 3)
			return;

		const auto delta{ frame_time::deltas(r) };
		const auto table{ frame_time::build_table(delta) };
		const u32 grid{ frame_time::pick_grid(delta) };

		frame_time::write_table(orz, table, grid);

		const frame_time::_time_grid time_grid{ table, grid };

		frame_time::write_indices(orz, orz_tag::time_delta_rc, frame_time::osu, table, delta, [&](size_t i, i64 now) {

			return frame_time::osu_ctx(r, i, now, time_grid);
		});

	}
		
	namespace repeat {

		inline std::vector<u8> flags(const _osr& r) {

			std::vector<u8> repeated(r.size());

			for (size_t i{ 1 }; i < r.size(); ++i)
				repeated[i] = same_bits(r[i].x, r[i - 1].x) && same_bits(r[i].y, r[i - 1].y);

			return repeated;
		}

		inline std::vector<int> sample_dt(std::span<const int> delta, const std::vector<u8>& repeated) {

			std::vector<int> dt;

			i64 elapsed{};

			for (size_t i{}; i < delta.size(); ++i) {

				elapsed += delta[i];

				if (repeated[i])
					continue;

				
				dt.push_back((int)std::clamp<i64>(elapsed, INT_MIN, INT_MAX));

				elapsed = 0;

			}

			return dt;
		}

		inline std::vector<u32> sample_of(const std::vector<u8>& repeated) {

			std::vector<u32> s(repeated.size());

			for (size_t i{ 1 }; i < repeated.size(); ++i)
				s[i] = s[i - 1] + !repeated[i];

			return s;
		}

	}
		
	struct _shared_streams {
		_orz_construct time, osr;
		const _osr* frames;
		std::vector<int> delta;
		std::vector<u8> repeated;
		std::vector<u32> sample;
	};

	static _shared_streams shared_streams(const _osr& r, const u32 flags, orz_state& state) {

		_shared_streams sh{ { &state }, { &state }, &r };

		sh.time.frame_count = (u32)r.size();
		sh.time.flags = flags;

		encode_delta_time(r, sh.time);

		sh.delta = frame_time::deltas(r);
		sh.repeated = repeat::flags(r);
		sh.sample = repeat::sample_of(sh.repeated);

		_key_data_constructor kd{};

		for (const auto& f : r)
			kd.tick_std(f.keys);

		sh.osr.add_osr_parts(kd);

		return sh;
	}

	
	struct _positions {

		_orz_construct streams;
		std::array<dspace::axis_streams, 2> st;

		size_t size() const {
			return streams.file_data.size();
		}

	};

	static _positions position_streams(const dspace::_model_info& m, const std::vector<float> vals[2], const std::vector<int>& dt,
		const _shared_streams& shared, orz_state& state) {

		_positions p{ { &state } };

		for (int a{}; a < 2; ++a)
			p.st[a] = dspace::encode_axis_model(m.axis[a], vals[a], dt, state.exact_text);

		{

			const auto packed{ dspace::write_info(m) };

			p.streams.add_stream(orz_tag::device_space_info, packed.data(), packed.size());

		}

		const auto bytes{ dspace::write_moves_rc(m.axis, p.st.data(), shared.delta, shared.repeated) };

		p.streams.add_stream(orz_tag::device_moves_rc, bytes.data(), bytes.size());

		std::vector<u32> index;

		for (int a{}; a < 2; ++a)
			for (const auto& [f, v] : p.st[a].escapes)
				index.push_back(f * 2 + a);

		std::sort(index.begin(), index.end());

		if (index.size()) {

			bit_stream e{}, values{};

			dspace::write_escape_index(index, e);

			for (int a{}; a < 2; ++a)
				for (size_t j{}; j < p.st[a].escapes.size(); ++j)
					dspace::write_escape_value(p.st[a].escapes[j].second, p.st[a].escape_pred[j], values, state.exact_text);

			p.streams.add_compress_stream(orz_tag::device_escape, e, comp::pos_delta_prop());
			p.streams.add_compress_stream(orz_tag::device_escape_values, values, comp::pos_delta_prop());

		}

		return p;
	}
		
	static void write_orz(const _positions& p, const _shared_streams& shared, orz_state& state) {

		_orz_construct result{ shared.time };

		const _osr& r{ *shared.frames };

		u32 crc{};

		for (size_t i{}; i < r.size(); ++i) {

			const u32 s{ shared.sample[i] };

			crc = crc32c_frame(crc, { r[i].delta, p.st[0].decoded[s], p.st[1].decoded[s], r[i].keys });

		}

		if (state.seed.present)
			crc = crc32c_frame(crc, { -12345, 0.f, 0.f, state.seed.value });

		result.frames_crc = crc;

		result.append(p.streams);
		result.append(shared.osr);
		result.finish("");

	}

	// axis_mode::none marks what detection could not fit
	static dspace::_model_info text_filled(dspace::_model_info m) {

		for (auto& ax : m.axis) {

			for (auto& sg : ax.seg) {

				if (sg.p.mode == dspace::axis_mode::none) {

					sg.p = {};
					sg.p.mode = dspace::axis_mode::text;

				}

			}

		}

		return m;
	}

	static dspace::_model_info all_text() {

		dspace::axis_params p{};

		p.mode = dspace::axis_mode::text;

		dspace::_model_info m{};

		m.axis[0] = dspace::axis_model{ p };
		m.axis[1] = dspace::axis_model{ p };

		return m;
	}

	
	static bool detect_device_model(orz_state& state, const std::vector<float> vals[2], const std::vector<int>& dt,
		const _shared_streams& shared, dspace::_model_info& info, _positions& best) {

		// whole replay, one model per axis
		dspace::_model_info whole{};

		dspace::axis_streams ws[2];

		{

			dspace::axis_params px, py;

			px = detect_axis_model(state, vals[0], dt, 0, nullptr, ws[0]);

			if (px.mode != dspace::axis_mode::none)
				py = detect_axis_model(state, vals[1], dt, 1, &px, ws[1]);
			else {

				// x found nothing: detect y on its own, then retry x with y's mapping
				py = detect_axis_model(state, vals[1], dt, 1, nullptr, ws[1]);

				if (py.mode == dspace::axis_mode::absolute)
					px = detect_axis_model(state, vals[0], dt, 0, &py, ws[0]);

			}

			whole.axis[0] = dspace::axis_model{ px };
			whole.axis[1] = dspace::axis_model{ py };

		}

		const bool whole_any{ !whole.axis[0].all_none() || !whole.axis[1].all_none() };
		const bool whole_ok{ !whole.axis[0].any_none() && !whole.axis[1].any_none() };

		bool try_segments{ !whole_ok };

		for (int a{}; a < 2 && whole_ok; ++a)
			try_segments |= (ws[a].escapes.size() + ws[a].repeats) * 100 > vals[a].size() * 3;

		bool found{};

		if (whole_any) {

			info = text_filled(whole);
			best = position_streams(info, vals, dt, shared, state);
			found = true;

		}

		if (try_segments) {

			dspace::_model_info seg{};

			const auto desktops = [&](int a) {

				return whole.axis[a].seg[0].p.mode != dspace::axis_mode::mouse;
			};

			seg.axis[0] = segment_axis(state, vals[0], dt, 0, nullptr, whole.axis[0].all_none(), desktops(0));
			seg.axis[1] = segment_axis(state, vals[1], dt, 1, &seg.axis[0], whole.axis[1].all_none(), desktops(1));

			if (!seg.axis[0].all_none() || !seg.axis[1].all_none()) {

				auto p{ position_streams(text_filled(seg), vals, dt, shared, state) };

				if (!found || p.size() < best.size()) {

					info = text_filled(seg);
					best = std::move(p);
					found = true;

				}

			}

		}

		return found;
	}

	void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state) {

		const _shared_streams shared{ shared_streams(r, flags, state) };

		const std::vector<int> dt{ repeat::sample_dt(shared.delta, shared.repeated) };

		std::vector<float> vals[2];

		for (size_t i{}; i < r.size(); ++i) {

			if (shared.repeated[i])
				continue;

			vals[0].push_back(r[i].x);
			vals[1].push_back(r[i].y);

		}

		dspace::_model_info info{ all_text() };
		_positions best{ { &state } };

		const bool has_model{ !state.debug.disable_device && dt.size() >= 64 && detect_device_model(state, vals, dt, shared, info, best) };

		// both axis' as text, only tried when the model misses 5% or more of an axis's frames
		const auto misses = [&](int a) {

			return (best.st[a].escapes.size() + best.st[a].repeats) * 20 >= dt.size();
		};

		if (!has_model || misses(0) || misses(1)) {

			auto text{ position_streams(all_text(), vals, dt, shared, state) };

			if (!has_model || text.size() <= best.size()) {

				info = all_text();
				best = std::move(text);

			}

		}

		write_orz(best, shared, state);

		++state.stats.files;

		state.stats.frames += r.size();

		for (int a{}; a < 2; ++a) {

			for (const auto& sg : info.axis[a].seg)
				++state.stats.axis[(size_t)sg.p.mode];

			state.stats.escapes += best.st[a].escapes.size();

		}

		if (info.axis[0].seg.size() > 1 || info.axis[1].seg.size() > 1)
			++state.stats.segmented;

		if (strlen(output_file))
			write_file(output_file, state.output);

	}
	
	bool decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R) {

		if (const auto& trc{ stream_data[orz_tag::time_delta_rc] }; trc.size()) {

			u32 grid{};

			const auto table{ frame_time::read_table(stream_data, grid) };
			const frame_time::_time_grid time_grid{ table, grid };

			frame_time::read_indices(trc, frame_time::osu, table, R.size(),
				[&](size_t i, i64 now) {

					return frame_time::osu_ctx(R, i, now, time_grid);
				},
				[&](size_t i, int d) {

					R[i].delta = d;

				});

		}

		dspace::_model_info info{};

		if (!dspace::read_info(stream_data[orz_tag::device_space_info], info))
			return R.empty();

		std::vector<std::pair<u32, float>> escapes[2];

		for (const u32 index : dspace::read_escape_index(_bit_view{ stream_data[orz_tag::device_escape] }))
			escapes[index % 2].push_back({ index / 2, 0.f });

		const _bit_view escape_values{ stream_data[orz_tag::device_escape_values] };

		size_t escape_cursor{};

		std::vector<i64> K[2];
		std::vector<int> slack[2];

		std::vector<int> dt;
		std::vector<u8> repeated;

		dspace::read_moves_rc(stream_data[orz_tag::device_moves_rc], info.axis, escapes, frame_time::deltas(R), K, slack, dt, repeated);

		const auto sample{ repeat::sample_of(repeated) };

		const bool exact_text{ (header->flags & ORZ_FLAG::text_shortest) != 0 };

		std::vector<float> out;

		for (int a{}; a < 2; ++a) {

			if (!dspace::decode_axis(info.axis[a], K[a], slack[a], escapes[a], dt, exact_text, out, &escape_values, &escape_cursor))
				return false;

			for (size_t i{}; i < R.size(); ++i)
				(&R[i].x)[a] = out[sample[i]];

		}

		return true;
	}

}
