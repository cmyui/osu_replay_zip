#pragma once

// osu! standard cursor axis models (encoder + decoder), replaying the game's float maths.
// Values only need to print the same 7 digits, so everything compares through dotnet::text_bounds.
// Frames a model can not reproduce are escaped.

#include <bit>
#include <climits>
#include "rc.h"
#include "orz_text.h"
#include "orz_decimal.h"
#include <cmath>
#include <algorithm>
#include <span>
#include <vector>

namespace dspace {

	// std::llround without the CRT call (v - trunc(v) is exact)
	FORCEINLINE i64 round_i64(double v) {

		if (!(std::abs(v) < 4e18))
			return std::llround(v);

		const i64 t{ (i64)v };
		const double f{ v - (double)t };

		return t + (f >= 0.5) - (f <= -0.5);
	}

	// std::nextafter towards +-infinity on the float's bits
	FORCEINLINE float next_up(float x) {

		if (x != x || x == INFINITY)
			return x;

		if (x == 0.f)
			return std::bit_cast<float>(1u);

		const u32 b{ std::bit_cast<u32>(x) };

		return std::bit_cast<float>(x > 0.f ? b + 1 : b - 1);
	}

	FORCEINLINE float next_down(float x) {
		return -next_up(-x);
	}

	enum class axis_mode : u8 {
		none = 0,

		// mostly can be used when raw input is turned off, maps to the windows (OS) cursor lattice
		// can map to any absolute device too though
		rational = 1,

		// any device that moves by a delta
		mouse = 2,

		// tablets, touch devices - etc
		absolute = 3,

		// fall back - value ordinals when nothing else maps
		text = 4,
	};

	enum class tablet_mapping : u8 { desktop, window };  // window is stables sAbsoluteToOsuWindow
	enum class float_unit : u8 { sse, x87 };

	// window pixels -> playfield
	// field = (pixels - off) * (1 / scale), shared by mouse and tablet
	struct field_map {
		float scale, off;
		bool wide; // pos - off stays unrounded (x87 register) before the multiply
		bool operator==(const field_map&) const = default;
	};

	// value = (a * k + b) / D
	struct rational_params {
		int D, a, b;
		bool operator==(const rational_params&) const = default;
	};

	// counts * sens per report, polling rate per second
	struct mouse_params {
		float sens, poll;
		bool operator==(const mouse_params&) const = default;
	};

	struct tablet_params {

		tablet_mapping mapping;

		float_unit fp;

		float span, origin, half, speed;  // screen or window size, screen origin - client origin, size / 2, speed

		bool operator==(const tablet_params&) const = default;
	};

	struct axis_params {

		axis_mode mode;

		field_map field;

		rational_params rational;

		mouse_params mouse;

		tablet_params tablet;

		bool operator==(const axis_params&) const = default;
	};

	// common window sizes
	constexpr int PM_RES[][2]{
		{ 1920, 1080 }, { 2560, 1440 }, { 1366, 768 }, { 1360, 768 }, { 1600, 900 }, { 1280, 720 },
		{ 1680, 1050 }, { 1920, 1200 }, { 1440, 900 }, { 1280, 800 }, { 1280, 1024 }, { 1024, 768 },
		{ 800, 600 }, { 3840, 2160 }, { 2560, 1080 }, { 3440, 1440 }, { 1536, 864 }, { 1280, 960 },
		{ 1400, 1050 }, { 1600, 1200 }, { 2560, 1600 }, { 1152, 864 }, { 1600, 1024 }, { 1920, 1440 },
	};

	FORCEINLINE auto playfield(int W, int H) {

		struct _playfield {
			float ratio, off_x, off_y;
		};

		const float WindowManager_Ratio{ (float)H / 480.f };
		const float GameField_Width{ 512 * WindowManager_Ratio * 1.f };
		const float GameField_Height{ 384 * WindowManager_Ratio * 1.f };
		const float GameField_Ratio{ GameField_Height / 384 };
		const float mode_offset{ -16 * WindowManager_Ratio };

		return _playfield{ GameField_Ratio, (float)(W - GameField_Width) / 2, (float)(H - GameField_Height) / 4 * 3 + mode_offset };
	}

	// most replays keep a consistent axis_model
	// if setup changes mid play (window moved, input device swapped, etc) it can though
	// a mouse segment starts with an escaped frame (re-seed)

	struct axis_segment {
		u32 start;
		axis_params p;
	};

	struct axis_model {

		std::vector<axis_segment> seg;

		axis_model() = default;
		explicit axis_model(const axis_params& p) : seg{ { 0u, p } } {}

		const axis_params& at(size_t frame) const {

			size_t k{};

			while (k + 1 < seg.size() && seg[k + 1].start <= frame)
				++k;

			return seg[k].p;
		}

		bool any_none() const {

			for (const auto& s : seg)
				if (s.p.mode == axis_mode::none)
					return true;

			return seg.empty();
		}

		bool all_none() const {

			for (const auto& s : seg)
				if (s.p.mode != axis_mode::none)
					return false;

			return true;
		}
	};

	struct _model_info {
		axis_model axis[2];
	};

	FORCEINLINE float rational_value(const axis_params& p, i64 k) {
		return (float)(((double)p.rational.a * (double)k + p.rational.b) / (double)p.rational.D);
	}

	FORCEINLINE i64 rational_k(const axis_params& p, float x) {
		return round_i64(((double)x * p.rational.D - p.rational.b) / p.rational.a);
	}

	// tablet/touch screen - all absolute postional devices
	inline float absolute_window_pos(const axis_params& p, i64 L) {

		// all taken from osu!stable

		if (p.tablet.mapping == tablet_mapping::window) {

			// ((float)((L - 32768) * speed) + 32768) / 65536 * W

			if (p.tablet.fp == float_unit::sse) {

				float a{ (float)(L - 32768) * p.tablet.speed };
				a = a + 32768.f;
				a = a / 65536.f;

				return a * p.tablet.span;
			}

			const float a{ (float)((double)(L - 32768) * p.tablet.speed) };

			return (float)(((double)a + 32768.0) / 65536.0 * p.tablet.span);
		}

		// ToScreenCoords: (float)L / 65536 * width + screen.X; then - ClientBounds.X; then (pos - W/2) * speed + W/2
		// origin = screen.X - client.X is applied as one sub
		if (p.tablet.fp == float_unit::sse) {

			float a{ (float)L / 65536.f };

			a = a * p.tablet.span;

			a = a - (-p.tablet.origin);

			if (p.tablet.speed != 1.f) {
				a = a - p.tablet.half;
				a = a * p.tablet.speed;
				a = a + p.tablet.half;
			}

			return a;
		}

		float a{ (float)((double)L / 65536.0 * p.tablet.span) };

		a = (float)((double)a + p.tablet.origin);

		if (p.tablet.speed != 1.f)
			a = (float)(((double)a - p.tablet.half) * p.tablet.speed + p.tablet.half);

		return a;
	}

	// GameField.Ratio as the x87 JIT returns it: Height / 384 unrounded, Height = 384 * (float)(H / 480)
	inline double field_ratio_x87(const axis_params& p) {

		const int H{ (int)std::lround((double)p.field.scale * 480.0) };
		const float height{ 384.f * ((float)H / 480.f) };

		return same_bits(height / 384.f, p.field.scale) ? (double)height / 384.0 : (double)p.field.scale;
	}

	inline float to_field(const axis_params& p, float pos) {

		if (p.field.wide && (p.mode == axis_mode::mouse || p.mode == axis_mode::absolute))
			return (float)(((double)pos - (double)p.field.off) * (1.0 / (double)p.field.scale));

		const float d{ pos - p.field.off };

		if (p.mode == axis_mode::mouse || (p.mode == axis_mode::absolute && p.tablet.fp == float_unit::x87))
			return (float)((double)d / field_ratio_x87(p));

		const float inv{ 1.f / p.field.scale };

		return d * inv;
	}

	inline float absolute_field(const axis_params& p, i64 L) {

		return to_field(p, absolute_window_pos(p, L));
	}

	inline i64 absolute_L_guess(const axis_params& p, float x) {

		double pos{ (double)x * p.field.scale + p.field.off };

		if (p.tablet.mapping == tablet_mapping::window)
			return round_i64(((pos / p.tablet.span) * 65536.0 - 32768.0) / p.tablet.speed + 32768.0);

		if (p.tablet.speed != 1.f)
			pos = (pos - p.tablet.half) / p.tablet.speed + p.tablet.half;

		return round_i64((pos - p.tablet.origin) * 65536.0 / p.tablet.span);
	}

	constexpr i64 MOUSE_MAX_REPORTS{ 1ll << 16 };

	inline i64 mouse_reports(const axis_params& p, i64 dk, int dt) {

		const i64 n{ dk < 0 ? -dk : dk };

		const double want{ std::clamp((double)dt * p.mouse.poll / 1000.0, 1.0, (double)MOUSE_MAX_REPORTS) };

		return std::clamp<i64>(round_i64(want), 1, std::min(n, MOUSE_MAX_REPORTS));
	}

	inline float window_size(const axis_params& p) {

		const int H{ (int)std::lround((double)p.field.scale * 480.0) };

		if (H <= 0 || H > 20000)
			return 0.f;

		const auto f{ playfield(0, H) };

		if (!same_bits(f.ratio, p.field.scale))
			return 0.f;

		if (same_bits(f.off_y, p.field.off))
			return (float)H;

		const int W{ (int)std::lround(2.0 * p.field.off + 512.0 * ((double)H / 480.0)) };

		if (W > 0 && W < 40000 && same_bits(playfield(W, H).off_x, p.field.off))
			return (float)W;

		return 0.f;
	}

	inline float mouse_advance_m(const axis_params& p, float X, i64 dk, i64 m);

	inline float mouse_advance(const axis_params& p, float X, i64 dk, int dt) {

		return dk == 0 ? X : mouse_advance_m(p, X, dk, mouse_reports(p, dk, dt));
	}

	inline float mouse_advance_m(const axis_params& p, float X, i64 dk, i64 m) {

		if (dk == 0)
			return X;

		const i64 n{ dk < 0 ? -dk : dk }, sgn{ dk < 0 ? -1 : 1 };

		// while playing the cursor is clamped to the window after every report (RawMouseHandler)
		const float size{ window_size(p) };

		for (i64 q{}; q < m; ++q) {

			const i64 c{ n / m + (q < n % m) };

			X = (float)((double)X + (double)(sgn * c) * (double)p.mouse.sens);

			if (size > 0.f)
				X = std::clamp(X, 0.f, size - 1.f);

		}

		return X;
	}

	constexpr int MOUSE_SEED_RANGE{ 64 };
	constexpr int MOUSE_SLACK_RANGE{ 24 };

	inline float nudge(float Y, int j) {

		j = std::clamp(j, -MOUSE_SLACK_RANGE, MOUSE_SLACK_RANGE);

		for (int q{}; q < (j < 0 ? -j : j); ++q)
			Y = j < 0 ? next_down(Y) : next_up(Y);

		return Y;
	}

	// search order is 0, 1, -1, 2, -2, etc
	FORCEINLINE int slack_at(int i) {
		return (i & 1) ? (i + 1) / 2 : -(i / 2);
	}

	struct _mouse_candidates {

		const axis_params& p;
		float X;
		i64 dk, n, m;
		float list[20];
		int count{}, next_split{};
				
		_mouse_candidates(const axis_params& p_, float X_, i64 dk_, int dt, float first) : p{ p_ }, X{ X_ }, dk{ dk_ } {

			list[count++] = first;
			next_split = 1;

			n = dk < 0 ? -dk : dk;
			
			if (dk == 0) {

				m = 1;
				next_split = 99;

			} else
				m = mouse_reports(p, dk, dt);

		}
				
		bool get(int k, float& out) {

			if (k > 2 * MOUSE_SLACK_RANGE)
				return false;

			constexpr i64 OFFSETS[]{ 0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -6, 6, -7, 7, -8, 8 };

			while (count <= k && next_split < (int)std::size(OFFSETS)) {

				const i64 r{ m + OFFSETS[next_split++] };

				if (r < 1 || r > n || r > MOUSE_MAX_REPORTS)
					continue;

				const float v{ mouse_advance_m(p, X, dk, r) };

				bool seen{};

				for (int q{}; q < count; ++q)
					seen |= std::bit_cast<u32>(list[q]) == std::bit_cast<u32>(v);

				if (!seen)
					list[count++] = v;

			}

			if (k < count) {

				out = list[k];

				return true;
			}

			const int j{ k - count + 1 };

			if (j > 2 * MOUSE_SLACK_RANGE)
				return false;

			out = nudge(list[0], slack_at(j));

			return true;
		}

	};
	
	FORCEINLINE int slack_of_index(int k) {
		return (int)rc::unzz((u64)k);
	}

	FORCEINLINE int slack_index(int v) {
		return (int)rc::zz(v);
	}

	// first float within range ulps of Y (slack_at order) that fits, j = its offset; false leaves Y unchanged
	template<typename Fits>
	FORCEINLINE bool ulp_search(float& Y, int range, int& j, Fits fits) {

		if (fits(Y)) {

			j = 0;

			return true;
		}

		float up{ Y }, down{ Y };

		for (int s{ 1 }; s <= range; ++s) {

			up = next_up(up);

			if (fits(up)) {

				Y = up;
				j = s;

				return true;
			}

			down = next_down(down);

			if (fits(down)) {

				Y = down;
				j = -s;

				return true;
			}

		}

		return false;
	}

	inline float mouse_seed(const axis_params& p, float x, bool exact_text) {

		const dotnet::text_bounds b{ x, exact_text };

		float Y{ (float)((double)x * p.field.scale + p.field.off) };
		int j;

		ulp_search(Y, MOUSE_SEED_RANGE, j, [&](float v) { return b.contains(to_field(p, v)); });

		return Y;
	}

	FORCEINLINE i64 mouse_dk(const axis_params& p, float x, float X) {
		return round_i64(std::clamp(((double)x * p.field.scale + p.field.off - X) / p.mouse.sens, -1e7, 1e7));
	}

	inline bool first_fit(_mouse_candidates& cand, const dotnet::text_bounds& b, int& k, float& Y) {

		for (; cand.dk != 0 || k < 1; ++k) {

			if (!cand.get(k, Y))
				return false;

			if (b.contains(to_field(cand.p, Y)))
				return true;

		}

		return false;
	}

	// rough bits of a candidate index in the slack stream
	FORCEINLINE double slack_cost(int k) {
		return k == 0 ? 0.3 : 2.0 + std::log2(1.0 + k);
	}

	inline double mouse_next_cost(const axis_params& p, float X, float x1, const dotnet::text_bounds& b1, int dt1) {

		const i64 dk{ mouse_dk(p, x1, X) };

		if (dk == 0)
			return b1.contains(to_field(p, X)) ? 0.0 : 40.0;

		_mouse_candidates cand{ p, X, dk, dt1, mouse_advance(p, X, dk, dt1) };

		int k{};
		float Y{};

		return first_fit(cand, b1, k, Y) ? slack_cost(k) : 40.0;
	}

	// quickly check surrounding cand's to see if there is a better fit
	inline void mouse_finalize(_mouse_candidates& cand, const dotnet::text_bounds& b, float x1, const dotnet::text_bounds& b1, int dt1,
		int& k, float& Y) {

		const double first_next{ mouse_next_cost(cand.p, Y, x1, b1, dt1) };

		if (first_next <= slack_cost(0))
			return;

		double best{ slack_cost(k) + first_next };

		float v{};

		for (int q{ k + 1 }, fits{ 1 }; fits < 6 && first_fit(cand, b, q, v); ++q, ++fits) {

			if (const double c{ slack_cost(q) + mouse_next_cost(cand.p, v, x1, b1, dt1) }; c < best) {

				best = c;
				Y = v;
				k = q;

			}

		}

	}

	struct axis_streams {

		std::vector<i64> K;
		std::vector<int> slack;
		std::vector<std::pair<u32, float>> escapes;
		std::vector<float> escape_pred;
		size_t repeats{};
		std::vector<float> decoded;

	};

	// escape value: 7 digit mantissa difference from the model's prediction, false (raw) if exponents differ
	FORCEINLINE bool escape_digits(float x, float pred, i64& diff) {

		if (!std::isfinite(pred))
			return false;

		const auto [mx, ex] { dotnet::digits7(x) };

		const auto [mp, ep] { dotnet::digits7(pred) };

		if (mx == 0 || mp == 0 || ex != ep)
			return false;

		diff = mx - mp;

		return true;
	}

	inline float escape_from_digits(float pred, i64 diff) {

		const auto [mp, ep] { dotnet::digits7(pred) };

		return orz_decimal::to_float(mp + diff, ep - 6);
	}

	// exact text: ulp distance from the prediction
	inline bool escape_ulps(float x, float pred, i64& diff) {

		if (!std::isfinite(pred) || !std::isfinite(x) || std::signbit(x) != std::signbit(pred))
			return false;

		diff = (i64)std::bit_cast<u32>(x) - (i64)std::bit_cast<u32>(pred);

		return diff > -(1ll << 30) && diff < (1ll << 30);
	}

	// bounds: optional precomputed text_bounds of vals
	// max_escapes: give up past this many (streams incomplete)
	// decoded: optional, gets what decode_axis will return per frame
	inline axis_streams encode_axis(const axis_params& p, std::span<const float> vals, std::span<const int> dt,
		bool exact_text, std::span<const dotnet::text_bounds> bounds = {}, size_t max_escapes = SIZE_MAX, float* decoded = nullptr) {

		axis_streams o{};
		o.K.resize(vals.size());

		const auto out = [&](size_t i, float v) { if (decoded) decoded[i] = v; };

		// held: after an escape, frames with an unchanged K repeat the value instead of escaping again

		bool held{};
		float held_value{};

		const auto escape = [&](size_t i, float pred) {

			o.escapes.push_back({ (u32)i, vals[i] });
			o.escape_pred.push_back(pred);

			held = true;
			held_value = vals[i];

			out(i, vals[i]);

		};

		// true if it had to escape
		const auto still = [&](size_t i, const dotnet::text_bounds& b, float pred) {

			if (!b.contains(held_value)) {

				escape(i, pred);

				return true;
			}

			++o.repeats;

			out(i, held_value);

			return false;
		};

		if (p.mode == axis_mode::none) {

			for (size_t i{}; i < vals.size(); ++i)
				escape(i, NAN);

			return o;
		}

		const auto bound = [&](const size_t i) {

			return bounds.empty() ? dotnet::text_bounds{ vals[i], exact_text } : bounds[i];
		};

		if (p.mode == axis_mode::text) {

			for (size_t i{}; i < vals.size(); ++i) {

				if (i64 ord; dotnet::text_ord(vals[i], exact_text, ord)) {

					if (const float v{ dotnet::from_text_ord(ord, exact_text) }; bound(i).contains(v)) {

						o.K[i] = ord;
						out(i, v);

						continue;
					}

				}

				o.K[i] = i ? o.K[i - 1] : 0; // escaped, no move
				escape(i, NAN);

			}

			return o;
		}

		if (p.mode == axis_mode::absolute) {

			const bool rising{ p.tablet.span > 0.f && p.tablet.speed > 0.f && p.field.scale > 0.f };

			for (size_t i{}; i < vals.size(); ++i) {

				if (o.escapes.size() > max_escapes)
					return o;

				const dotnet::text_bounds b{ bound(i) };

				const i64 L0{ absolute_L_guess(p, vals[i]) };
				const float f0{ absolute_field(p, L0) };

				i64 L{ L0 };
				float fl{ f0 };

				bool found{ b.contains(f0) };

				if (!found) {

					if (rising) {

						const int dir{ b.side(f0) > 0 ? -1 : 1 };

						for (int j{ 1 }; j <= 2; ++j) {

							const float fj{ absolute_field(p, L0 + dir * j) };
							const int sd{ b.side(fj) };

							if (sd == 0) {

								L = L0 + dir * j;
								fl = fj;
								found = true;

								break;
							}

							if (sd == dir)
								break;

						}

					} else {

						for (int s{ 1 }; s < 5 && !found; ++s) {

							const float fs{ absolute_field(p, L0 + slack_at(s)) };

							if (b.contains(fs)) {

								L = L0 + slack_at(s);

								fl = fs;
								found = true;

							}

						}

					}

				}

				o.K[i] = L;

				if (held && i && L == o.K[i - 1]) {

					still(i, b, fl);

					continue;
				}

				if (found) {

					held = false;
					out(i, fl);

				} else
					escape(i, f0);

			}

			return o;
		}

		if (p.mode == axis_mode::rational) {

			for (size_t i{}; i < vals.size(); ++i) {

				if (o.escapes.size() > max_escapes)
					return o;

				const dotnet::text_bounds b{ bound(i) };
				const i64 k{ rational_k(p, vals[i]) };
				const float v{ rational_value(p, k) };

				o.K[i] = k;

				if (held && i && k == o.K[i - 1]) {

					still(i, b, v);

					continue;
				}

				if (b.contains(v)) {

					held = false;
					out(i, v);

				} else
					escape(i, v);

			}

			return o;
		}

		// mouse
		float X{};
		i64 K{};

		for (size_t i{}; i < vals.size(); ++i) {

			if (o.escapes.size() > max_escapes)
				return o;

			const float x{ vals[i] };
			const dotnet::text_bounds b{ bound(i) };

			bool escaped{ i == 0 };

			const i64 dk{ escaped ? 0 : mouse_dk(p, x, X) };

			float pred{ NAN };

			if (!escaped && held && dk == 0) {

				o.K[i] = K;

				if (still(i, b, to_field(p, X))) // what decode_axis predicts for an escape with dk = 0
					X = mouse_seed(p, x, exact_text);

				continue;
			}

			if (!escaped) {

				const float Y0{ mouse_advance(p, X, dk, dt[i]) };
				pred = to_field(p, Y0);

				_mouse_candidates cand{ p, X, dk, dt[i], Y0 };

				float Y{};
				int k{};

				if (first_fit(cand, b, k, Y)) {

					if (decoded && dk != 0 && i + 1 < vals.size())
						mouse_finalize(cand, b, vals[i + 1], bound(i + 1), dt[i + 1], k, Y);

					if (dk != 0)
						o.slack.push_back(slack_of_index(k));

					X = Y;

				} else
					escaped = true;

			}

			if (escaped) {

				escape(i, pred);
				X = mouse_seed(p, x, exact_text);

			} else {

				held = false;
				out(i, to_field(p, X));

			}

			o.K[i] = K += dk;

		}

		return o;
	}

	// each segment encoded on its own, joined in frame order
	inline axis_streams encode_axis_model(const axis_model& m, std::span<const float> vals, std::span<const int> dt, bool exact_text) {

		axis_streams o{};
		o.K.resize(vals.size());
		o.decoded.resize(vals.size());

		for (size_t k{}; k < m.seg.size(); ++k) {

			const size_t from{ m.seg[k].start };
			const size_t to{ k + 1 < m.seg.size() ? m.seg[k + 1].start : vals.size() };

			if (from >= to)
				continue;

			const auto part{
				encode_axis(m.seg[k].p, vals.subspan(from, to - from), dt.subspan(from, to - from),
					exact_text, {}, SIZE_MAX, o.decoded.data() + from)
			};

			for (size_t i{}; i < part.K.size(); ++i)
				o.K[from + i] = part.K[i];

			o.slack.insert(o.slack.end(), part.slack.begin(), part.slack.end());
			o.repeats += part.repeats;

			for (size_t j{}; j < part.escapes.size(); ++j) {

				o.escapes.push_back({ (u32)(part.escapes[j].first + from), part.escapes[j].second });
				o.escape_pred.push_back(part.escape_pred[j]);

			}

		}

		return o;
	}

	// mouse reports a decode may simulate, guards damaged files
	constexpr i64 MOUSE_DECODE_BUDGET{ 1ll << 27 };

	// escapes: escaped positions, ascending; escape_value(pred): the next escaped value. false if damaged
	template<typename Value>
	inline bool decode_axis(const axis_model& m, const std::vector<i64>& K, const std::vector<int>& slack,
		const std::vector<u32>& escapes, std::span<const int> dt, bool exact_text, std::vector<float>& out, Value&& escape_value) {

		out.assign(K.size(), 0.f);

		size_t e{}, s{};
		float X{};
		i64 prevK{};
		i64 budget{ MOUSE_DECODE_BUDGET };
		bool held{};

		size_t sg{};

		for (size_t i{}; i < K.size(); ++i) {

			while (sg + 1 < m.seg.size() && m.seg[sg + 1].start <= i)
				++sg;

			const axis_params& p{ m.seg[sg].p };
			const bool segment_start{ m.seg[sg].start == i };

			const bool escaped{ e < escapes.size() && escapes[e] == i };

			if (escaped)
				++e;

			const i64 dk{ K[i] - prevK };
			prevK = K[i];

			if (p.mode == axis_mode::mouse && dk != 0 && (budget -= mouse_reports(p, dk, dt[i])) < 0)
				return false;

			if (p.mode == axis_mode::none) {

				out[i] = escape_value(NAN);

				continue;
			}

			if (p.mode == axis_mode::text) {

				out[i] = escaped ? escape_value(NAN) : dotnet::from_text_ord(K[i], exact_text);

				continue;
			}

			// held value, see encode_axis
			if (segment_start)
				held = false;

			if (held && dk == 0 && !escaped) {

				out[i] = out[i - 1];

				continue;
			}

			held = escaped;

			if (p.mode == axis_mode::rational || p.mode == axis_mode::absolute) {

				const float v{ p.mode == axis_mode::rational ? rational_value(p, K[i]) : absolute_field(p, K[i]) };

				out[i] = escaped ? escape_value(v) : v;

				continue;
			}

			if (escaped) {

				out[i] = escape_value(segment_start ? NAN : to_field(p, mouse_advance(p, X, dk, dt[i])));

				X = mouse_seed(p, out[i], exact_text);

				continue;
			}

			const float Y0{ mouse_advance(p, X, dk, dt[i]) };

			if (dk != 0) {

				_mouse_candidates cand{ p, X, dk, dt[i], Y0 };

				if (!cand.get(slack_index(s < slack.size() ? slack[s++] : 0), X))
					return false;

			} else
				X = Y0;

			out[i] = to_field(p, X);

		}

		return true;
	}

	// model parameters: small integers + the playfield floats

	inline void write_u(u32 v, bit_stream& o) { write_bucket(v, o, 4, 8, 16); }
	inline u32 read_u(const _bit_view in, size_t& i) { return read_bucket(in, i, 4, 8, 16); }

	inline u32 hundredths(float v) { return (u32)std::lround((double)v * 100.0); }
	inline float from_hundredths(u32 v) { return (float)((double)v / 100.0); }

	inline bool on_hundredths(float v) {
		return v > 0.f && v < 1e6f && same_bits(from_hundredths(hundredths(v)), v);
	}

	inline void write_setting(float v, bit_stream& o) {

		const bool grid{ on_hundredths(v) };

		o.push_back(!grid);

		if (grid)
			write_u(hundredths(v), o);
		else
			write_float(v, o);

	}

	inline float read_setting(const _bit_view in, size_t& i) {

		if (!in[i++])
			return from_hundredths(read_u(in, i));

		return read_float(in, i);
	}

	constexpr int WINDOW_BITS{ 5 };

	static_assert(std::size(PM_RES) <= (1u << WINDOW_BITS));

	inline int window_of(const field_map& f, int axis) {

		for (int r{}; r < (int)std::size(PM_RES); ++r) {

			const auto [ratio, ox, oy] { playfield(PM_RES[r][0], PM_RES[r][1]) };

			if (same_bits(ratio, f.scale) && same_bits(axis ? oy : ox, f.off))
				return r;

		}

		return -1;
	}

	inline void write_field(const field_map& f, int axis, bit_stream& o) {

		const int r{ window_of(f, axis) };

		o.push_back(r >= 0);

		if (r < 0) {

			write_float(f.scale, o);
			write_float(f.off, o);

			return;
		}

		for (int b{}; b < WINDOW_BITS; ++b)
			o.push_back((r >> b) & 1);

	}

	inline bool read_field(const _bit_view in, size_t& i, int axis, field_map& f) {

		if (!in[i++]) {

			f.scale = read_float(in, i);
			f.off = read_float(in, i);

			return true;
		}

		int r{};

		for (int b{}; b < WINDOW_BITS; ++b)
			r |= (int)in[i++] << b;

		if (r >= (int)std::size(PM_RES))
			return false;

		const auto [ratio, ox, oy] { playfield(PM_RES[r][0], PM_RES[r][1]) };

		f.scale = ratio;
		f.off = axis ? oy : ox;

		return true;
	}
	
	constexpr u32 POLL_RATES[]{ 125, 250, 500, 1000, 2000, 4000, 8000 };
	
	inline void write_poll(float poll, bit_stream& o) {

		u32 k{};

		while (k < std::size(POLL_RATES) && (float)POLL_RATES[k] != poll)
			++k;

		for (int b{}; b < 3; ++b)
			o.push_back((k >> b) & 1);

		if (k == std::size(POLL_RATES))
			for (int b{}; b < 16; ++b)
				o.push_back(((u32)poll >> b) & 1);

	}

	inline float read_poll(const _bit_view in, size_t& i) {

		u32 k{};

		for (int b{}; b < 3; ++b)
			k |= (u32)in[i++] << b;

		if (k < std::size(POLL_RATES))
			return (float)POLL_RATES[k];

		u32 v{};

		for (int b{}; b < 16; ++b)
			v |= (u32)in[i++] << b;

		return (float)v;
	}

	static_assert(std::size(POLL_RATES) == 7);

	inline void write_params(const axis_params& p, int axis, bit_stream& o) {

		write_bucket((u32)p.mode, o, 2);

		if (p.mode == axis_mode::rational) {

			write_u((u32)p.rational.D, o);
			write_u((u32)p.rational.a, o);
			write_u((u32)rc::zz(p.rational.b), o);

		}

		if (p.mode == axis_mode::mouse) {

			write_setting(p.mouse.sens, o);
			write_poll(p.mouse.poll, o);
			o.push_back(p.field.wide);
			write_field(p.field, axis, o);

		}

		if (p.mode == axis_mode::absolute) {

			o.push_back(p.tablet.mapping == tablet_mapping::window);
			o.push_back(p.tablet.fp == float_unit::x87);
			o.push_back(p.field.wide);

			write_u((u32)p.tablet.span, o);
			write_u((u32)rc::zz((int)p.tablet.origin), o);
			write_u((u32)p.tablet.half, o);
			write_setting(p.tablet.speed, o);
			write_field(p.field, axis, o);

		}

	}

	inline axis_params read_params(const _bit_view in, size_t& i, int axis, bool& ok) {

		axis_params p{};

		p.mode = (axis_mode)read_bucket(in, i, 2);

		if (p.mode == axis_mode::rational) {

			p.rational.D = (int)read_u(in, i);
			p.rational.a = (int)read_u(in, i);
			p.rational.b = (int)rc::unzz(read_u(in, i));

		}

		if (p.mode == axis_mode::mouse) {

			p.mouse.sens = read_setting(in, i);
			p.mouse.poll = read_poll(in, i);
			p.field.wide = in[i++] != 0;
			ok &= read_field(in, i, axis, p.field);

		}

		if (p.mode == axis_mode::absolute) {

			p.tablet.mapping = in[i++] ? tablet_mapping::window : tablet_mapping::desktop;
			p.tablet.fp = in[i++] ? float_unit::x87 : float_unit::sse;
			p.field.wide = in[i++] != 0;
			p.tablet.span = (float)read_u(in, i);
			p.tablet.origin = (float)rc::unzz(read_u(in, i));
			p.tablet.half = (float)read_u(in, i);
			p.tablet.speed = read_setting(in, i);
			ok &= read_field(in, i, axis, p.field);

		}

		return p;
	}

	inline bool y_from_x(const axis_params& x, axis_params& y) {

		y = x;

		if (x.mode == axis_mode::rational)
			return true;

		if (x.mode != axis_mode::mouse)
			return false;

		const int r{ window_of(x.field, 0) };

		if (r < 0)
			return false;

		y.field.off = playfield(PM_RES[r][0], PM_RES[r][1]).off_y;

		return true;
	}

	inline void write_info(const _model_info& m, bit_stream& o) {

		for (int a{}; a < 2; ++a) {

			const auto& ax{ m.axis[a] };

			write_u((u32)ax.seg.size(), o);

			if (a == 1 && ax.seg.size() == 1 && m.axis[0].seg.size() == 1) {

				axis_params y;

				const bool same{ y_from_x(m.axis[0].seg[0].p, y) && y == ax.seg[0].p };

				o.push_back(same);

				if (same)
					continue;

			}

			u32 prev{};

			bool writing_segment{};

			for (const auto& sg : ax.seg) {

				if (writing_segment)
					write_u(sg.start - prev, o);
				else
					writing_segment = true;

				prev = sg.start;
				write_params(sg.p, a, o);

			}

		}

	}

	// from bit i of in, which ends past it
	inline bool read_info(const _bit_view in, size_t& i, _model_info& out) {

		_model_info m{};

		for (int a{}; a < 2; ++a) {

			auto& ax{ m.axis[a] };

			const u32 n{ read_u(in, i) };

			if (n == 0 || n > 100000)
				return false;

			ax.seg.resize(n);

			if (a == 1 && n == 1 && m.axis[0].seg.size() == 1 && in[i++]) {

				if (!y_from_x(m.axis[0].seg[0].p, ax.seg[0].p))
					return false;

				continue;
			}

			u32 prev{};

			bool writing_segment{};

			for (auto& sg : ax.seg) {

				if (writing_segment)
					sg.start = prev += read_u(in, i);
				else {

					sg.start = 0;

					writing_segment = true;

				}

				bool ok{ true };

				sg.p = read_params(in, i, a, ok);

				if (!ok || sg.p.mode > axis_mode::text ||
					(sg.p.mode == axis_mode::mouse && !(sg.p.mouse.poll >= 0.f && sg.p.mouse.poll <= 8000.f)))
						return false; // no detector produces these
			}

			if (ax.seg[0].start != 0)
				return false;

		}

		out = std::move(m);

		return true;
	}

	struct move_predictor {

		static constexpr size_t WINDOW_FRAMES{ 8 };
		static constexpr i64 WINDOW_MS{ 8 }, ACCEL_MS{ 12 }, SCALE_MIN{ 4 }, REST_MS{ 14 };

		i64 hist[2][WINDOW_FRAMES]{};
		int hist_dt[WINDOW_FRAMES]{};
		size_t n{};

		static i64 ms(int v) {
			return std::clamp(v, 1, 40);
		}

		// n / m rounded half away from zero, m > 0
		static i64 round_div(i64 n, i64 m) {
			return n >= 0 ? (n + m / 2) / m : -((-n + m / 2) / m);
		}

		i64 window(const int a, const int dt) const {

			if (n < 2)
				return 0;

			i64 moved{}, span{};

			for (size_t j{}; j < WINDOW_FRAMES && j + 2 <= n; ++j) {

				moved += hist[a][j];
				span += ms(hist_dt[j]);

				if (span >= WINDOW_MS)
					break;
			}

			return round_div(moved * ms(dt), std::max(span, 1ll));
		}

		i64 pred(int a, int dt) const {

			i64 p{ window(a, dt) };

			if (n >= 3 && ms(dt) >= ACCEL_MS && ms(hist_dt[0]) >= ACCEL_MS && ms(hist_dt[1]) >= ACCEL_MS)
				p += (hist[a][0] - hist[a][1]) / 4;

			return p;
		}

		int first(int dt) const {
			return std::abs(window(1, dt)) > std::abs(window(0, dt)) ? 1 : 0;
		}

		static i64 scaled(i64 pred, i64 first_d, i64 first_pred) {

			const i64 A{ std::abs(first_d) }, B{ std::abs(first_pred) };

			if (B < SCALE_MIN || first_d == 0 || (first_d > 0) != (first_pred > 0))
				return pred;

			if (2 * A < B)
				return round_div(pred, 2);

			if (A > 2 * B)
				return pred * 2;

			if (std::abs(pred) >= (1ll << 31) || A >= (1ll << 31))   // only for invalid files - keeps pred * A in range
				return pred;

			return round_div(pred * A, B);
		}

		void push(int a, i64 d) {

			for (size_t j{ WINDOW_FRAMES - 1 }; j > 0; --j)
				hist[a][j] = hist[a][j - 1];

			hist[a][0] = d;

		}

		void advance(int dt) {

			for (size_t j{ WINDOW_FRAMES - 1 }; j > 0; --j)
				hist_dt[j] = hist_dt[j - 1];

			hist_dt[0] = dt;
			++n;

		}

		void rest(int gap) {

			for (int a{}; a < 2; ++a)
				for (size_t j{ WINDOW_FRAMES - 1 }; j > 0; --j)
					hist[a][j] = hist[a][j - 1];

			hist[0][0] = hist[1][0] = 0;

			advance(gap);

		}

	};

}
