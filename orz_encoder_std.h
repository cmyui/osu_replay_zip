#pragma once

#include "orz_frames.h"
#include "orz_device_std.h"

#include <algorithm>
#include <climits>
#include <array>
#include <vector>

namespace standard {

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
		std::vector<u32> keys; // folded, K1 / K2 imply M1 / M2
		std::vector<int> table;
		u32 grid{};
		bool relax{};
		bool start{};
		bool start_time{};
	};

	inline bool starts_at_start(const _osr& r) {

		return r.size() > 2 && same_bits(r[0].x, POS_DEFAULT_X) && same_bits(r[0].y, POS_DEFAULT_Y) &&
			same_bits(r[1].x, POS_DEFAULT_X) && same_bits(r[1].y, POS_DEFAULT_Y);
	}

	static _shared_streams shared_streams(const _osr& r, const u32 flags, orz_state& state, bool start, bool start_time) {

		_shared_streams sh{ { &state }, { &state }, &r };

		sh.time.frame_count = (u32)r.size();
		sh.time.flags = flags;

		sh.delta = frame_time::deltas(r);
		sh.start = start;
		sh.start_time = start_time;

		// the first two deltas are not coded with start_time
		sh.table = frame_time::build_table(std::span{ sh.delta }.subspan(start_time ? 2 : 0));
		sh.grid = frame_time::pick_grid(sh.delta);

		sh.repeated = repeat::flags(r);
		sh.sample = repeat::sample_of(sh.repeated);

		sh.keys.resize(r.size());

		for (size_t i{}; i < r.size(); ++i)
			sh.keys[i] = r[i].keys ^ ((r[i].keys >> 2) & 3);

		sh.relax = (flags & ORZ_FLAG::relax) != 0;

		sh.osr.add_osr_parts();

		return sh;
	}

	
	// autopilot parameters, after the device model in the head of the frames stream
	inline void write_autopilot(const autopilot::_params& ap, bit_stream& o) {

		o.push_back(ap.on);

		if (!ap.on)
			return;

		small_write<2>(ap.rate, o);
		dspace::write_u(ap.width, o);
		dspace::write_u(ap.height, o);

	}

	inline bool read_autopilot(const _bit_view in, size_t& i, autopilot::_params& ap) {

		ap = {};
		ap.on = small_read<1>(in, i) != 0;

		if (!ap.on)
			return true;

		ap.rate = small_read<2>(in, i);
		ap.width = dspace::read_u(in, i);
		ap.height = dspace::read_u(in, i);

		return ap.rate < autopilot::RATE_COUNT && ap.width && ap.height && ap.width <= (1 << 15) && ap.height <= (1 << 15);
	}

	// window sizes tried for autopilot, the common ones first
	constexpr std::pair<u32, u32> AUTOPILOT_WINDOWS[]{
		{ 1920, 1080 }, { 2560, 1440 }, { 1366, 768 }, { 1280, 720 }, { 1600, 900 }, { 1680, 1050 }, { 1440, 900 }, { 1280, 1024 },
		{ 1024, 768 }, { 800, 600 }, { 1280, 960 }, { 1360, 768 }, { 1536, 864 }, { 2560, 1080 }, { 3440, 1440 }, { 1280, 800 },
		{ 1920, 1200 }, { 2560, 1600 }, { 1600, 1200 },
	};

	// the new positions with their times and 7 digit ordinals, for autopilot (none for shortest round-trip text)
	static std::vector<autopilot::sample> autopilot_samples(const _osr& r, const _shared_streams& shared, bool exact_text) {

		if (exact_text)
			return {};

		std::vector<autopilot::sample> s;

		i64 t{};

		for (size_t i{}; i < r.size(); ++i) {

			t += r[i].delta;

			if (shared.repeated[i]) {

				if (!s.empty())
					s.back().stays = true;

				continue;
			}

			autopilot::sample q{ t };

			q.v[0] = r[i].x;
			q.v[1] = r[i].y;

			if (!dotnet::text_ord(q.v[0], false, q.K[0]) || !dotnet::text_ord(q.v[1], false, q.K[1]))
				return {};

			s.push_back(q);

		}

		return s;
	}

	// the window, then the speed, under which fitted moves cover the most of a stretch past the start
	static autopilot::_params search_autopilot(std::span<const autopilot::sample> s) {

		const size_t skip{ std::min<size_t>(s.size(), 64) };
		const auto head{ s.subspan(skip, std::min<size_t>(s.size() - skip, 600)) };

		autopilot::_params best{};
		size_t most{};

		const auto attempt = [&](const autopilot::_params& p) {

			size_t c{};

			autopilot::fitter{ head, p }.fit(c);

			if (c > most) {

				most = c;
				best = p;

			}

		};

		for (const auto& [w, h] : AUTOPILOT_WINDOWS)
			attempt({ true, 0, w, h });

		for (u32 rate{ 1 }; rate < autopilot::RATE_COUNT && best.on; ++rate)
			attempt({ true, rate, best.width, best.height });

		if (most * 10 < head.size())
			best = {};

		return best;
	}

	struct _positions {

		_orz_construct streams;
		std::array<dspace::axis_streams, 2> st;

		size_t size() const {
			return streams.file_data.size();
		}

	};

	static _positions position_streams(const dspace::_model_info& m, const std::vector<float> vals[2], const std::vector<int>& dt,
		const _shared_streams& shared, orz_state& state, const autopilot::_params& ap = {}, const std::vector<autopilot::event>& moves = {}) {

		_positions p{ { &state } };

		for (int a{}; a < 2; ++a)
			p.st[a] = dspace::encode_axis_model(m.axis[a], vals[a], dt, state.exact_text);

		// frames: delta table and device model, from the next byte the context mixed frames and escape values
		bit_stream head{};

		frame_time::write_table(head, shared.table, shared.grid);
		dspace::write_info(m, head);
		write_autopilot(ap, head);

		head.push_back(shared.start);

		if (shared.start)
			head.push_back(shared.start_time);

		frames::_frame_data fd{ shared.delta, shared.keys, shared.repeated };

		fd.moves = moves;

		std::vector<std::pair<float, float>> values[2];

		for (int a{}; a < 2; ++a) {

			fd.K[a] = p.st[a].K;
			fd.slack[a] = p.st[a].slack;

			for (size_t j{}; j < p.st[a].escapes.size(); ++j) {

				fd.escaped[a].push_back(p.st[a].escapes[j].first);
				values[a].push_back({ p.st[a].escapes[j].second, p.st[a].escape_pred[j] });

			}

		}

		const frames::_side side{ m.axis, shared.table, frame_time::_time_grid{ shared.grid }, shared.relax, ap, shared.start_time };

		add_to_u8(head.data, frames::encode(fd, side, shared.delta.size(), values, state.exact_text));

		p.streams.add_stream(orz_tag::frames, head.data.data(), head.data.size());

		return p;
	}
		
	static void write_orz(const _positions& p, const _shared_streams& shared, orz_state& state) {

		_orz_construct result{ shared.time };

		const _osr& r{ *shared.frames };

		u32 crc{};

		for (size_t i{}; i < r.size(); ++i) {

			const u32 s{ shared.sample[i] };

			const bool at_start{ shared.start && i < 2 };

			crc = crc32c_frame(crc, { r[i].delta, at_start ? POS_DEFAULT_X : p.st[0].decoded[s], at_start ? POS_DEFAULT_Y : p.st[1].decoded[s], r[i].keys });

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

	
	// the replay's frames with autopilot moves in place of a device model
	static _positions autopilot_streams(std::span<const autopilot::sample> s, const autopilot::_params& ap, const std::vector<float> vals[2],
		const std::vector<int>& dt, const _shared_streams& shared, orz_state& state) {

		size_t covered{};

		const auto moves{ autopilot::fitter{ s, ap }.fit(covered) };

		return position_streams(all_text(), vals, dt, shared, state, ap, moves);
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

	void encode(const char* output_file, const _osr& original, const u32 flags, orz_state& state) {

		const bool start{ starts_at_start(original) };
		const bool start_time{ start && original[0].delta == 0 && original[1].delta == -1 };

		_osr moved;

		if (start) {

			moved = original;

			for (int k{}; k < 2; ++k) {

				moved[k].x = moved[2].x;
				moved[k].y = moved[2].y;

			}

		}

		const _osr& r{ start ? moved : original };

		const _shared_streams shared{ shared_streams(r, flags, state, start, start_time) };

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

		// autopilot: clearly autopilot replays skip the device models, others that have none try it against text
		const auto ap_samples{ autopilot_samples(r, shared, state.exact_text) };

		if (const auto ap{ autopilot::detect(ap_samples, AUTOPILOT_WINDOWS, std::size(AUTOPILOT_WINDOWS)) }; ap.on)
			best = autopilot_streams(ap_samples, ap, vals, dt, shared, state);
		else {

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

			const auto text_only = [&](int a) {

				return info.axis[a].seg.size() == 1 && info.axis[a].seg[0].p.mode == dspace::axis_mode::text;
			};

			if (text_only(0) && text_only(1)) {

				if (const auto search{ search_autopilot(ap_samples) }; search.on) {

					auto moves{ autopilot_streams(ap_samples, search, vals, dt, shared, state) };

					if (moves.size() < best.size())
						best = std::move(moves);

				}

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
	
	// the head of the frames stream: delta table, time grid and device model; rest: what follows from the next byte
	inline bool read_head(std::span<const u8> data, std::vector<int>& table, u32& grid, dspace::_model_info& info,
		autopilot::_params& ap, bool& start, bool& start_time, std::span<const u8>& rest) {

		const _bit_view head{ data };

		size_t bit{};

		table = frame_time::read_table(head, bit, grid);

		if (!dspace::read_info(head, bit, info) || !read_autopilot(head, bit, ap))
			return false;

		start = bit < head.max_size() && head[bit++];
		start_time = start && bit < head.max_size() && head[bit++];

		rest = data.subspan(std::min(data.size(), (bit + 7) / 8));

		return true;
	}

	bool decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R) {

		frames::_side side{};
		dspace::_model_info info{};
		u32 grid{};
		std::span<const u8> rest;

		bool start{}, start_time{};

		if (!read_head(stream_data[orz_tag::frames], side.table, grid, info, side.ap, start, start_time, rest))
			return R.empty();

		side.axis = info.axis;
		side.grid = frame_time::_time_grid{ grid };
		side.relax = (header->flags & ORZ_FLAG::relax) != 0;
		side.start_time = start_time;

		frames::_frame_data fd{};
		frames::_reader reader{ rest, fd, side, R.size() };

		const auto sample{ repeat::sample_of(fd.repeated) };

		const bool exact_text{ (header->flags & ORZ_FLAG::text_shortest) != 0 };

		std::vector<float> out;

		for (int a{}; a < 2; ++a) {

			const auto escape_value = [&](float pred) { return reader.escape(pred, a, exact_text); };

			if (!dspace::decode_axis(info.axis[a], fd.K[a], fd.slack[a], fd.escaped[a], fd.dt, exact_text, out, escape_value))
				return false;

			for (size_t i{}; i < R.size(); ++i)
				(&R[i].x)[a] = out[sample[i]];

		}

		for (size_t i{}; i < R.size(); ++i) {

			R[i].delta = fd.delta[i];
			R[i].keys = fd.keys[i] ^ ((fd.keys[i] >> 2) & 3);

		}

		for (size_t i{}; start && i < std::min<size_t>(2, R.size()); ++i) {

			R[i].x = POS_DEFAULT_X;
			R[i].y = POS_DEFAULT_Y;

		}

		return true;
	}

}
