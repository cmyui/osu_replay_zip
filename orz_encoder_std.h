#pragma once

#include "orz_device_std.h"

#include <algorithm>
#include <array>
#include <vector>

namespace standard {

	void encode_delta_time(const _osr& r, _orz_construct& orz) {

		if (r.size() < 3)
			return;

		const auto delta{ frame_time::deltas(r) };
		const auto table{ frame_time::write_table(orz, delta) };
		const u64 rate{ frame_time::rate_class(table[0]) };

		frame_time::write_indices(orz, orz_tag::time_delta_rc, frame_time::osu, table, delta, [&](size_t i, i64 now) {

			return frame_time::osu_ctx(r, i, now, rate);
		});

	}

	struct device_shared {
		_orz_construct time, osr;
		const _osr* frames;
	};

	static device_shared shared_streams(const _osr& r, const u32 flags, orz_state& state) {

		// k1+k2 normally always come with m1+m2 set - so we fold them
		bool foldable{ true };

		for (const auto& f : r)
			foldable &= !((f.keys & 4) && !(f.keys & 1)) && !((f.keys & 8) && !(f.keys & 2));

		device_shared sh{ { &state }, { &state }, &r };

		sh.time.frame_count = (u32)r.size();
		sh.time.flags = flags | (foldable ? 0u : u32(ORZ_FLAG::keys_unfolded));

		encode_delta_time(r, sh.time);

		_key_data_constructor kd{};

		for (const auto& f : r) {

			if (foldable)
				kd.tick_std(f.keys);
			else
				kd.tick(f.keys);

		}

		sh.osr.add_osr_parts(kd);

		return sh;
	}

	static std::vector<u8> build_device_file(const dspace::_model_info& info, const std::array<dspace::axis_streams, 2>& st,
		const std::vector<int>& dt, const device_shared& shared, orz_state& state) {

		_orz_construct result{ shared.time };

		{ // CRC

			const _osr& r{ *shared.frames };

			u32 crc{};

			for (size_t i{}; i < r.size(); ++i)
				crc = crc32c_frame(crc, { r[i].delta, st[0].decoded[i], st[1].decoded[i], r[i].keys });

			if (state.seed.present)
				crc = crc32c_frame(crc, { -12345, 0.f, 0.f, state.seed.value });

			result.frames_crc = crc;

		}

		{

			const auto packed{ dspace::write_info(info) };

			result.add_stream(orz_tag::device_space_info, packed.data(), packed.size());

		}

		const auto bytes{ dspace::write_moves_rc(info.axis, st.data(), dt) };

		result.add_stream(orz_tag::device_moves_rc, bytes.data(), bytes.size());
		
		std::vector<u32> index;

		for (int a{}; a < 2; ++a)
			for (const auto& [f, v] : st[a].escapes)
				index.push_back(f * 2 + a);

		std::sort(index.begin(), index.end());

		if (index.size()) {

			bit_stream e{}, values{};

			dspace::write_escape_index(index, e);

			for (int a{}; a < 2; ++a)
				for (size_t j{}; j < st[a].escapes.size(); ++j)
					dspace::write_escape_value(st[a].escapes[j].second, st[a].escape_pred[j], values, state.exact_text);

			result.add_compress_stream(orz_tag::device_escape, e, comp::pos_delta_prop());
			result.add_compress_stream(orz_tag::device_escape_values, values, comp::pos_delta_prop());

		}

		result.append(shared.osr);
		result.finish("");

		return std::move(state.output);
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

	static std::vector<u8> build_model_file(const dspace::_model_info& m, const std::vector<float> vals[2], const std::vector<int>& dt,
		const device_shared& shared, orz_state& state, std::array<dspace::axis_streams, 2>& st) {

		for (int a{}; a < 2; ++a)
			st[a] = dspace::encode_axis_model(m.axis[a], vals[a], dt, state.exact_text);

		return build_device_file(m, st, dt, shared, state);
	}

	static bool detect_device_model(orz_state& state, const std::vector<float> vals[2], const std::vector<int>& dt,
		const device_shared& shared, dspace::_model_info& info, std::vector<u8>& bytes, std::array<dspace::axis_streams, 2>& st) {

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

		info = {};
		bytes.clear();

		if (whole_any) {

			info = whole;
			bytes = build_model_file(text_filled(whole), vals, dt, shared, state, st);

		}

		if (try_segments) {

			dspace::_model_info seg{};

			const auto desktops = [&](int a) {

				return whole.axis[a].seg[0].p.mode != dspace::axis_mode::mouse;
			};

			seg.axis[0] = segment_axis(state, vals[0], dt, 0, nullptr, whole.axis[0].all_none(), desktops(0));
			seg.axis[1] = segment_axis(state, vals[1], dt, 1, &seg.axis[0], whole.axis[1].all_none(), desktops(1));

			if (!seg.axis[0].all_none() || !seg.axis[1].all_none()) {

				std::array<dspace::axis_streams, 2> seg_st;

				auto seg_bytes{ build_model_file(text_filled(seg), vals, dt, shared, state, seg_st) };

				if (bytes.empty() || seg_bytes.size() < bytes.size()) {

					info = seg;
					bytes = std::move(seg_bytes);
					st = std::move(seg_st);

				}

			}

		}

		return !bytes.empty();
	}

	void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state) {

		const std::vector<int> dt{ frame_time::deltas(r) };

		std::vector<float> vals[2];

		for (size_t i{}; i < r.size(); ++i) {

			vals[0].push_back(r[i].x);
			vals[1].push_back(r[i].y);

		}

		const device_shared shared{ shared_streams(r, flags, state) };

		dspace::_model_info detected{};
		bool has_model{};

		std::vector<u8> model_bytes;
		std::array<dspace::axis_streams, 2> ms;

		if (!state.debug.disable_device && r.size() >= 64)
			has_model = detect_device_model(state, vals, dt, shared, detected, model_bytes, ms);

		dspace::_model_info info{ all_text() };
		std::vector<u8> bytes;
		std::array<dspace::axis_streams, 2> st;

		if (has_model) {

			info = text_filled(detected);

			if (model_bytes.empty())
				model_bytes = build_model_file(info, vals, dt, shared, state, ms);

			bytes = std::move(model_bytes);
			st = std::move(ms);

		}

		// both axis' as text, only tried when the model misses 5% or more of an axis's frames
		const auto misses = [&](int a) {

			return (st[a].escapes.size() + st[a].repeats) * 20 >= r.size();
		};

		if (!has_model || misses(0) || misses(1)) {

			std::array<dspace::axis_streams, 2> text_st;

			auto text_bytes{ build_model_file(all_text(), vals, dt, shared, state, text_st) };

			if (!has_model || text_bytes.size() <= bytes.size()) {

				info = all_text();
				st = std::move(text_st);
				bytes = std::move(text_bytes);

			}

		}

		state.output = std::move(bytes);

		++state.stats.files;

		state.stats.frames += r.size();

		for (int a{}; a < 2; ++a) {

			for (const auto& sg : info.axis[a].seg)
				++state.stats.axis[(size_t)sg.p.mode];

			state.stats.escapes += st[a].escapes.size();

		}

		if (info.axis[0].seg.size() > 1 || info.axis[1].seg.size() > 1)
			++state.stats.segmented;

		if (strlen(output_file))
			write_file(output_file, state.output);

	}
	
	bool decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R) {

		if (const auto& trc{ stream_data[orz_tag::time_delta_rc] }; trc.size()) {

			const auto table{ frame_time::read_table(stream_data) };
			const u64 rate{ frame_time::rate_class(table.empty() ? 16 : table[0]) };

			frame_time::read_indices(trc, frame_time::osu, table, R.size(),
				[&](size_t i, i64 now) {

					return frame_time::osu_ctx(R, i, now, rate);
				},
				[&](size_t i, int d) {

					R[i].delta = d;

				});

		}

		dspace::_model_info info{};

		if (!dspace::read_info(stream_data[orz_tag::device_space_info], info))
			return R.empty();

		const std::vector<int> dt{ frame_time::deltas(R) };

		std::vector<std::pair<u32, float>> escapes[2];

		for (const u32 index : dspace::read_escape_index(_bit_view{ stream_data[orz_tag::device_escape] }))
			escapes[index % 2].push_back({ index / 2, 0.f });

		const _bit_view escape_values{ stream_data[orz_tag::device_escape_values] };

		size_t escape_cursor{};

		std::vector<i64> K[2];
		std::vector<int> slack[2];

		dspace::read_moves_rc(stream_data[orz_tag::device_moves_rc], info.axis, escapes, dt, R.size(), K, slack);

		const bool exact_text{ (header->flags & ORZ_FLAG::text_shortest) != 0 };

		std::vector<float> out;

		for (int a{}; a < 2; ++a) {

			if (!dspace::decode_axis(info.axis[a], K[a], slack[a], escapes[a], dt, exact_text, out, &escape_values, &escape_cursor))
				return false;

			for (size_t i{}; i < R.size(); ++i)
				(&R[i].x)[a] = out[i];

		}

		return true;
	}

}
