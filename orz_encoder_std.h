#pragma once

#include "orz_frames.h"
#include "orz_device_std.h"

#include <algorithm>
#include <climits>
#include <array>
#include <unordered_map>
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

	struct _native_grid {
		bool on;
		u64 A; // 32:32 fixed p
		i64 C; // 32:32 fixed p
	};

	inline i64 grid_value(const _native_grid& g, i64 j) {

		return (i64)((u64)g.A * (u64)j + (u64)g.C) >> 32;
	}

	inline bool grid_index(const _native_grid& g, i64 L, i64& j) {

		const i64 num{ (L << 32) - g.C };

		j = num >= 0 ? (i64)(((u64)num + g.A - 1) / g.A) : -(i64)((u64)(-num) / g.A);

		return grid_value(g, j) == L;
	}

	inline _native_grid fit_grid(const std::vector<i64>& K) {

		std::vector<i64> d{ K };

		std::sort(d.begin(), d.end());
		d.erase(std::unique(d.begin(), d.end()), d.end());

		// a few escapes can widen the range well past 65535
		if (d.size() < 64 || d.front() <= -(1 << 24) || d.back() >= (1 << 24))
			return {};

		std::unordered_map<i64, int> gaps;

		for (size_t i{ 1 }; i < d.size(); ++i)
			++gaps[d[i] - d[i - 1]];

		std::vector<std::pair<int, i64>> common;

		for (const auto& [k, n] : gaps)
			common.push_back({ n, k });

		std::sort(common.begin(), common.end(), std::greater<>{});
		common.resize(std::min<size_t>(common.size(), 6));

		const auto covered = [&](double st) {

			i64 n{};

			for (const auto& [k, c] : gaps) {

				const double m{ std::round((double)k / st) };

				if (m >= 1 && std::abs((double)k - m * st) <= 0.5 + 0.01 * (double)k)
					n += c;

			}

			return n;
		};

		std::vector<std::pair<double, i64>> cand;
		std::vector<double> pairs;

		for (const auto& [n1, g1] : common) {

			const int lo_n{ gaps.count(g1 - 1) ? gaps[g1 - 1] : 0 }, hi_n{ gaps.count(g1 + 1) ? gaps[g1 + 1] : 0 };
			const i64 g2{ lo_n >= hi_n ? g1 - 1 : g1 + 1 };
			const int n2{ std::max(lo_n, hi_n) };
			const double pair{ (double)(g1 * n1 + g2 * n2) / (n1 + n2) };

			pairs.push_back(pair);

			for (int k{ 1 }; k <= 4; ++k)
				if (pair / k >= 1.5)
					cand.push_back({ pair / k, covered(pair / k) });

		}

		i64 most{};

		for (const auto& c : cand)
			most = std::max(most, c.second);

		double s{};

		for (const auto& [st, n] : cand)
			if (n * 10 >= most * 9)
				s = std::max(s, st);

		if (s < 1.5 || most * 2 < (i64)d.size() - 1)
			return {};

		const double guess{ s }, span{ (double)(d.back() - d.front()) };
		
		const auto sample = [&](size_t n) {

			std::vector<double> u;

			for (size_t i{}, stride{ std::max<size_t>(1, d.size() / n) }; i < d.size(); i += stride)
				u.push_back((double)(d[i] - d.front()) / span);

			return u;
		};

		const std::vector<double> few{ sample(256) }, many{ sample(2048) };

		double phase{};

		const auto sweep = [&](double reach_steps, double q_step, const std::vector<double>& u) {

			constexpr int BINS{ 256 };

			const double n0{ span / s };
			const int reach{ (int)(reach_steps / q_step) };

			int hist[BINS], best{ -1 };
			double best_s{ s };

			for (int q{ -reach }; q <= reach; ++q) {

				const double N{ n0 + q * q_step }, st{ span / N };

				// a wide reach can pass zero steps
				if (N <= 0 || st < 1.2)
					continue;

				std::fill(std::begin(hist), std::end(hist), 0);

				for (const double x : u) {

					const double f{ x * N };

					++hist[std::min(BINS - 1, (int)((f - std::floor(f)) * BINS))];

				}

				const int w{ std::max(1, (int)(BINS / st)) };

				int sum{};

				for (int b{}; b < w; ++b)
					sum += hist[b];

				for (int b{}; b < BINS; ++b) {

					if (sum > best) {

						best = sum;
						best_s = st;
						phase = (b + w * 0.5) / BINS * st;

					}

					sum += hist[(b + w) % BINS] - hist[b];

				}

			}

			s = best_s;
		};

		std::vector<i64> on, j;

		std::vector<size_t> up, dn;

		const auto hulls = [&]() {

			const auto cross = [&](size_t o, size_t a, size_t b) {

				return (j[a] - j[o]) * (on[b] - on[o]) - (on[a] - on[o]) * (j[b] - j[o]);
			};

			up.clear();
			dn.clear();

			for (size_t i{}; i < on.size(); ++i) {

				while (up.size() >= 2 && cross(up[up.size() - 2], up.back(), i) >= 0)
					up.pop_back();

				up.push_back(i);

				while (dn.size() >= 2 && cross(dn[dn.size() - 2], dn.back(), i) <= 0)
					dn.pop_back();

				dn.push_back(i);

			}

		};

		const auto window = [&](double st, double& lo) {

			double hi{ std::numeric_limits<double>::max() };

			lo = std::numeric_limits<double>::lowest();

			for (size_t i : up)
				lo = std::max(lo, (double)on[i] - (double)j[i] * st);

			for (size_t i : dn)
				hi = std::min(hi, (double)on[i] + 1. - (double)j[i] * st);

			return hi - lo;
		};

		const auto refine = [&](bool exact) {

			for (int it{}; it < 3; ++it) {

				std::vector<std::pair<double, size_t>> t(d.size());
				std::vector<i64> jd(d.size());

				for (size_t i{}; i < d.size(); ++i) {

					jd[i] = std::llround(((double)(d[i] - d.front()) - phase) / s);
					t[i] = { (double)(d[i] - d.front()) - (double)jd[i] * s, i };

				}

				size_t count{ d.size() }, at{};

				if (!exact) {

					std::sort(t.begin(), t.end());

					count = 0;

					for (size_t e{}, b{}; e < t.size(); ++e) {

						while (t[e].first - t[b].first >= 1.)
							++b;

						if (e - b + 1 > count) {

							count = e - b + 1;
							at = b;

						}

					}

					std::sort(t.begin() + at, t.begin() + at + count, [](const auto& x, const auto& y) { return x.second < y.second; });

				}

				on.clear();
				j.clear();

				const i64 low{ jd[t[at].second] };

				for (size_t k{ at }; k < at + count; ++k) {

					on.push_back(d[t[k].second]);
					j.push_back(jd[t[k].second] - low);

				}

				hulls();

				double a{ s * 0.999 }, b{ s * 1.001 }, lo;

				for (int k{}; k < 40; ++k) {

					const double m1{ a + (b - a) / 3 }, m2{ b - (b - a) / 3 };

					if (window(m1, lo) < window(m2, lo))
						a = m1;
					else
						b = m2;

				}

				s = (a + b) / 2;

				// clearly no exact grid near this step
				if (exact && window(s, lo) < -1.)
					break;

			}

			double lo;

			return window(s, lo) > 0 ? on.size() : 0;
		};

		double lo;

		const auto fit_from = [&](double st) -> size_t {

			s = st;
			phase = 0;

			for (int it{}; it < 20; ++it) {

				i64 n{};

				for (size_t i{ 1 }; i < d.size(); ++i)
					n += std::max<i64>(1, std::llround((double)(d[i] - d[i - 1]) / s));

				const double s1{ span / (double)n };

				if (s1 == s)
					break;

				s = s1;

			}

			if (const size_t n{ refine(true) }; n != 0)
				return n;

			// a span step far from st is some other step
			if (std::abs(st - s) > s * 0.1)
				return 0;

			sweep(span / s * std::max(0.01, std::abs(st - s) / s + 0.005), 1., few);
			sweep(1., 0.25, many);

			size_t count{ refine(false) };

			if (count == 0)
				return 0;

			const double s0{ s }, p0{ phase };

			sweep(std::min(span / s * 0.0025, 8.), 0.125, many);

			if (const size_t c{ refine(false) }; c > count && window(s, lo) > 0)
				count = c;
			else {

				s = s0;
				phase = p0;
				refine(false);

			}

			return count;
		};

		// estimated bits saved per value
		const auto gain = [&](size_t n) {

			const double f{ (double)n / (double)d.size() };
			
			return n * 10 < d.size() * 7 ? std::numeric_limits<double>::lowest() : f * std::log2(s) - (1. - f) * 3.;
		};

		if (const size_t count{ fit_from(guess) }; count * 10 < d.size() * 9) {

			double best{ gain(count) }, best_s{ s }, best_phase{ phase };
			std::vector<double> tried{ guess };

			for (const double pr : pairs)
				for (int k{ 1 }; k <= 6; ++k) {

					const double st{ pr / k };

					if (st < 1.2 || std::any_of(tried.begin(), tried.end(), [&](double t) { return std::abs(st - t) < t * 0.005; }))
						continue;

					tried.push_back(st);

					if (const double g{ gain(fit_from(st)) }; g > best) {

						best = g;
						best_s = s;
						best_phase = phase;

					}

				}

			if (best == std::numeric_limits<double>::lowest())
				return {};

			s = best_s;
			phase = best_phase;

			if (refine(true) == 0)
				refine(false);

		}

		const double w{ window(s, lo) };
		const u64 A{ (u64)std::llround(s * 4294967296.) };

		if (w <= 0 || A == 0)
			return {};

		return { true, A, (i64)std::floor((lo + w / 2) * 4294967296.) };
	}

	inline float grid_field(const dspace::axis_params& p, i64 L) {
		return p.mode == dspace::axis_mode::rational ? dspace::rational_value(p, L) : dspace::absolute_field(p, L);
	}

	inline dspace::axis_streams grid_streams(const dspace::axis_params& p, const dspace::axis_streams& st, std::span<const float> vals,
		bool exact_text, const _native_grid& g) {

		dspace::axis_streams o{};

		o.K.resize(st.K.size());
		o.slack = st.slack;
		o.decoded.resize(st.K.size());

		bool held{};
		float last{};
		i64 prev{};

		for (size_t i{}; i < st.K.size(); ++i) {

			const i64 L{ st.K[i] };

			i64 j;

			// the grid value below may be nearer
			if (!grid_index(g, L, j) && j > 0 && L - grid_value(g, j - 1) < grid_value(g, j) - L)
				--j;

			j = std::max<i64>(j, 0);
			o.K[i] = j;

			const i64 Ls{ grid_value(g, j) };
			const i64 dk{ Ls - prev };

			prev = Ls;

			const dotnet::text_bounds b{ vals[i], exact_text };

			if (held && dk == 0) {

				if (b.contains(last)) {

					o.decoded[i] = last;

					continue;
				}

			} else if (const float v{ grid_field(p, Ls) }; b.contains(v)) {

				held = false;
				last = v;
				o.decoded[i] = v;

				continue;
			}

			// mostly one native count off the grid value
			float x{ vals[i] };

			for (const i64 n : { Ls - 1, Ls + 1 })
				if (const float v{ grid_field(p, n) }; b.contains(v)) {

					x = v;

					break;
				}

			o.escapes.push_back({ (u32)i, x });
			o.escape_pred.push_back(grid_field(p, Ls));

			held = true;
			last = x;
			o.decoded[i] = x;

		}

		return o;
	}

	// the values one native count either side of grid value L
	inline void grid_near(const dspace::axis_params& p, i64 L, float (&near)[2]) {

		near[0] = grid_field(p, L - 1);
		near[1] = grid_field(p, L + 1);

	}

	// after the autopilot parameters in the head of the frames stream, per axis
	inline void write_grid(const _native_grid& g, bit_stream& o) {

		o.push_back(g.on);

		if (!g.on)
			return;

		for (int b{}; b < 48; ++b)
			o.push_back((g.A >> b) & 1);

		for (int b{}; b < 64; ++b)
			o.push_back(((u64)g.C >> b) & 1);

	}

	inline bool read_grid(const _bit_view in, size_t& i, _native_grid& g) {

		g = {};
		g.on = small_read<1>(in, i) != 0;

		if (!g.on)
			return true;

		u64 c{};

		for (int b{}; b < 48 && i < in.max_size(); ++b)
			g.A |= (u64)(in[i++] != 0) << b;

		for (int b{}; b < 64 && i < in.max_size(); ++b)
			c |= (u64)(in[i++] != 0) << b;

		g.C = (i64)c;

		return g.A != 0;
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
		bit_stream model_head{};

		frame_time::write_table(model_head, shared.table, shared.grid);
		dspace::write_info(m, model_head);
		write_autopilot(ap, model_head);

		_native_grid grid[2]{};
		dspace::axis_streams on_grid[2];

		for (int a{}; a < 2; ++a)
			if (m.axis[a].seg.size() == 1 && (m.axis[a].seg[0].p.mode == dspace::axis_mode::absolute ||
				m.axis[a].seg[0].p.mode == dspace::axis_mode::rational)) {

				grid[a] = fit_grid(p.st[a].K);

				if (grid[a].on)
					on_grid[a] = grid_streams(m.axis[a].seg[0].p, p.st[a], vals[a], state.exact_text, grid[a]);

			}

		const auto build = [&](bool use_grid) {

			bit_stream head{ model_head };

			for (int a{}; a < 2; ++a)
				write_grid(use_grid ? grid[a] : _native_grid{}, head);

			head.push_back(shared.start);

			if (shared.start)
				head.push_back(shared.start_time);

			frames::_frame_data fd{ shared.delta, shared.keys, shared.repeated };

			fd.moves = moves;

			std::vector<frames::_escape> values[2];

			for (int a{}; a < 2; ++a) {

				const bool on{ use_grid && grid[a].on };
				const dspace::axis_streams& st{ on ? on_grid[a] : p.st[a] };

				fd.K[a] = st.K;
				fd.slack[a] = st.slack;

				for (size_t j{}; j < st.escapes.size(); ++j) {

					const u32 i{ st.escapes[j].first };

					fd.escaped[a].push_back(i);
					values[a].push_back({ st.escapes[j].second, st.escape_pred[j] });

					if (on)
						grid_near(m.axis[a].seg[0].p, grid_value(grid[a], st.K[i]), values[a].back().near);

				}

			}

			const frames::_side side{ m.axis, shared.table, frame_time::_time_grid{ shared.grid }, shared.relax, ap, shared.start_time };

			add_to_u8(head.data, frames::encode(fd, side, shared.delta.size(), values, state.exact_text));

			return head.data;
		};

		bool use_grid{ grid[0].on || grid[1].on };

		for (int a{}; a < 2; ++a)
			use_grid &= !grid[a].on || on_grid[a].escapes.size() * 20 < on_grid[a].K.size();

		auto best{ build(use_grid) };

		if (!use_grid && (grid[0].on || grid[1].on))
			if (auto g{ build(true) }; g.size() < best.size()) {

				best = std::move(g);
				use_grid = true;

			}

		
		if (use_grid)
			for (int a{}; a < 2; ++a)
				if (grid[a].on)
					p.st[a].decoded = on_grid[a].decoded;

		p.streams.add_stream(orz_tag::frames, best.data(), best.size());

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

			if (px.mode != dspace::axis_mode::none || py.mode != dspace::axis_mode::none)
				refine_window(state, vals, dt, px, py, ws);

			// mouase axis are differing, try each other and keep the winners
			if (px.mode == dspace::axis_mode::mouse && py.mode == dspace::axis_mode::mouse &&
				(px.field.scale != py.field.scale || px.field.wide != py.field.wide || px.mouse.sens != py.mouse.sens || px.mouse.poll != py.mouse.poll)) {

				const dspace::axis_params from[2]{ py, px };
				dspace::axis_params* to[2]{ &px, &py };

				for (int a{}; a < 2; ++a) {

					size_t best{ axis_cost(ws[a], dt, a, state.exact_text) };

					for (const auto& [W, H] : PM_RES) {

						const auto [ratio, ox, oy] { dspace::playfield(W, H) };

						if (ratio != from[a].field.scale)
							continue;

						auto q{ from[a] };

						q.field.off = a ? oy : ox;

						auto st{ dspace::encode_axis(q, vals[a], dt, state.exact_text) };

						if (st.escapes.size() * 2 > vals[a].size())
							continue;

						if (const size_t c{ axis_cost(st, dt, a, state.exact_text) }; c < best) {

							best = c;
							*to[a] = q;
							ws[a] = std::move(st);

						}

					}

				}

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
	
	// the head of the frames stream: delta table, time grid, device model, autopilot and tablet grids; rest: what follows from the next byte
	inline bool read_head(std::span<const u8> data, std::vector<int>& table, u32& grid, dspace::_model_info& info,
		autopilot::_params& ap, _native_grid (&grid_of)[2], bool& start, bool& start_time, std::span<const u8>& rest) {

		const _bit_view head{ data };

		size_t bit{};

		table = frame_time::read_table(head, bit, grid);

		if (!dspace::read_info(head, bit, info) || !read_autopilot(head, bit, ap))
			return false;

		for (int a{}; a < 2; ++a)
			if (!read_grid(head, bit, grid_of[a]))
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

		_native_grid grid_of[2]{};

		if (!read_head(stream_data[orz_tag::frames], side.table, grid, info, side.ap, grid_of, start, start_time, rest))
			return R.empty();

		side.axis = info.axis;
		side.grid = frame_time::_time_grid{ grid };
		side.relax = (header->flags & ORZ_FLAG::relax) != 0;
		side.start_time = start_time;

		frames::_frame_data fd{};
		frames::_reader reader{ rest, fd, side, R.size() };

		const auto sample{ repeat::sample_of(fd.repeated) };

		// tablet grid indices back to values
		for (int a{}; a < 2; ++a)
			if (grid_of[a].on)
				for (auto& k : fd.K[a])
					k = grid_value(grid_of[a], k);

		const bool exact_text{ (header->flags & ORZ_FLAG::text_shortest) != 0 };

		std::vector<float> out;

		for (int a{}; a < 2; ++a) {

			size_t n{};
			
			const auto escape_value = [&](float pred) {

				frames::_escape v{ 0.f, pred };

				if (grid_of[a].on)
					grid_near(info.axis[a].seg[0].p, fd.K[a][fd.escaped[a][n++]], v.near);

				return reader.escape(v, a, exact_text);
			};

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
