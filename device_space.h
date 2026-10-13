#pragma once

// osu! standard cursor axis models (encoder + decoder), replaying the game's float maths.
// Values only need to print the same 7 digits, so everything compares through dotnet::text_bounds.
// Frames a model can not reproduce are escaped.

#include <bit>
#include "rc.h"
#include "device_prior.h"
#include "orz_text.h"
#include "orz_decimal.h"
#include <cmath>
#include <algorithm>
#include <span>
#include <vector>

namespace dspace {

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
		return std::llround(((double)x * p.rational.D - p.rational.b) / p.rational.a);
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

	// window pixels -> playfield the game's way (GameField.DisplayToField: (pos - offset) * (1 / ratio) in float)
	inline float to_field(const axis_params& p, float pos) {

		const float inv{ 1.f / p.field.scale };
		const float d{ pos - p.field.off };

		return d * inv;
	}

	inline float absolute_field(const axis_params& p, i64 L) {

		return to_field(p, absolute_window_pos(p, L));
	}

	inline i64 absolute_L_guess(const axis_params& p, float x) {

		double pos{ (double)x * p.field.scale + p.field.off };

		if (p.tablet.mapping == tablet_mapping::window)
			return std::llround(((pos / p.tablet.span) * 65536.0 - 32768.0) / p.tablet.speed + 32768.0);

		if (p.tablet.speed != 1.f)
			pos = (pos - p.tablet.half) / p.tablet.speed + p.tablet.half;

		return std::llround((pos - p.tablet.origin) * 65536.0 / p.tablet.span);
	}

	constexpr i64 MOUSE_MAX_REPORTS{ 1ll << 16 };

	inline i64 mouse_reports(const axis_params& p, i64 dk, int dt) {

		const i64 n{ dk < 0 ? -dk : dk };

		const double want{ std::clamp((double)dt * p.mouse.poll / 1000.0, 1.0, (double)MOUSE_MAX_REPORTS) };

		return std::clamp<i64>(std::llround(want), 1, std::min(n, MOUSE_MAX_REPORTS));
	}

	inline float mouse_advance(const axis_params& p, float X, i64 dk, int dt) {

		if (dk == 0)
			return X;

		const i64 n{ dk < 0 ? -dk : dk }, sgn{ dk < 0 ? -1 : 1 };
		const i64 m{ mouse_reports(p, dk, dt) };

		for (i64 q{}; q < m; ++q) {

			const i64 c{ n / m + (q < n % m) };

			X = (float)((double)X + (double)(sgn * c) * (double)p.mouse.sens);

		}

		return X;
	}

	constexpr int MOUSE_SEED_RANGE{ 64 };
	constexpr int MOUSE_SLACK_RANGE{ 24 };

	inline float nudge(float Y, int j) {

		j = std::clamp(j, -MOUSE_SLACK_RANGE, MOUSE_SLACK_RANGE);

		for (int q{}; q < (j < 0 ? -j : j); ++q)
			Y = std::nextafter(Y, j < 0 ? -INFINITY : INFINITY);

		return Y;
	}

	// search order is 0, 1, -1, 2, -2, etc
	FORCEINLINE int slack_at(int i) {
		return (i & 1) ? (i + 1) / 2 : -(i / 2);
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

			up = std::nextafter(up, INFINITY);

			if (fits(up)) {

				Y = up;
				j = s;

				return true;
			}

			down = std::nextafter(down, -INFINITY);

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

	inline void write_escape_value(float x, float pred, bit_stream& out, bool exact_text) {

		if (i64 diff; exact_text ? escape_ulps(x, pred, diff) : escape_digits(x, pred, diff)) {

			write_bucket(u32(rc::zz(diff) + 1), out, 2, 4, 8, 16);

			return;
		}

		write_bucket(0, out, 2, 4, 8, 16);
		write_float(x, out);

	}

	inline float read_escape_value(float pred, const _bit_view in, size_t& i, bool exact_text) {

		const u32 code{ read_bucket(in, i, 2, 4, 8, 16) };

		if (code && exact_text)
			return float_from_bits(u32((i64)std::bit_cast<u32>(pred) + rc::unzz(code - 1)));

		if (code)
			return escape_from_digits(pred, rc::unzz(code - 1));

		return read_float(in, i);
	}

	// escape frame list (index = frame * 2 + axis), values are stored separately
	FORCEINLINE void write_escape_index(const std::vector<u32>& index, bit_stream& out) {

		write_bucket((u32)index.size(), out, 8, 16);

		u32 prev{};
		for (const u32 v : index) {

			write_bucket(v - prev, out, 4, 8, 16);
			prev = v;

		}

	}

	FORCEINLINE std::vector<u32> read_escape_index(const _bit_view in) {

		size_t i{};

		std::vector<u32> index(std::min<size_t>(read_bucket(in, i, 8, 16), in.max_size() / 4));

		u32 prev{};
		for (auto& v : index)
			v = prev += read_bucket(in, i, 4, 8, 16);

		return index;
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

			const i64 dk{ escaped ? 0 :
				std::llround(std::clamp(((double)x * p.field.scale + p.field.off - X) / p.mouse.sens, -1e7, 1e7))
			};

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

				// moves may be off by a few ulps (slack)
				float Y{ Y0 };
				int j;

				if (ulp_search(Y, dk == 0 ? 0 : MOUSE_SLACK_RANGE, j, [&](float v) { return b.contains(to_field(p, v)); })) {

					if (dk != 0)
						o.slack.push_back(j);

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

	// values / cursor: escape value stream (null: use escapes[].second); false if damaged
	inline bool decode_axis(const axis_model& m, const std::vector<i64>& K,
		const std::vector<int>& slack, const std::vector<std::pair<u32, float>>& escapes, std::span<const int> dt,
		bool exact_text, std::vector<float>& out, const _bit_view* values = nullptr, size_t* cursor = nullptr) {

		size_t ev{};

		const auto escape_value = [&](float pred) {

			return values ? read_escape_value(pred, *values, *cursor, exact_text) : escapes[ev++].second;
		};

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

			const bool escaped{ e < escapes.size() && escapes[e].first == i };

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

			X = dk == 0 ? Y0 : nudge(Y0, s < slack.size() ? slack[s++] : 0);

			out[i] = to_field(p, X);

		}

		return true;
	}

	// model parameters: small integers + the playfield floats

	inline void write_u(u32 v, bit_stream& o) { write_bucket(v, o, 4, 8, 16); }
	inline u32 read_u(const _bit_view in, size_t& i) { return read_bucket(in, i, 4, 8, 16); }

	inline u32 hundredths(float v) { return (u32)std::lround((double)v * 100.0); }
	inline float from_hundredths(u32 v) { return (float)((double)v / 100.0); }

	inline void write_params(const axis_params& p, bit_stream& o) {

		write_bucket((u32)p.mode, o, 2);

		if (p.mode == axis_mode::rational) {

			write_u((u32)p.rational.D, o);
			write_u((u32)p.rational.a, o);
			write_u((u32)rc::zz(p.rational.b), o);

		}

		if (p.mode == axis_mode::mouse) {

			write_u(hundredths(p.mouse.sens), o);
			write_u((u32)p.mouse.poll, o);
			write_float(p.field.scale, o);
			write_float(p.field.off, o);

		}

		if (p.mode == axis_mode::absolute) {

			o.push_back(p.tablet.mapping == tablet_mapping::window);
			o.push_back(p.tablet.fp == float_unit::x87);

			write_u((u32)p.tablet.span, o);
			write_u((u32)rc::zz((int)p.tablet.origin), o);
			write_u((u32)p.tablet.half, o);
			write_u(hundredths(p.tablet.speed), o);
			write_float(p.field.scale, o);
			write_float(p.field.off, o);

		}

	}

	inline axis_params read_params(const _bit_view in, size_t& i) {

		axis_params p{};

		p.mode = (axis_mode)read_bucket(in, i, 2);

		if (p.mode == axis_mode::rational) {

			p.rational.D = (int)read_u(in, i);
			p.rational.a = (int)read_u(in, i);
			p.rational.b = (int)rc::unzz(read_u(in, i));

		}

		if (p.mode == axis_mode::mouse) {

			p.mouse.sens = from_hundredths(read_u(in, i));
			p.mouse.poll = (float)read_u(in, i);
			p.field.scale = read_float(in, i);
			p.field.off = read_float(in, i);

		}

		if (p.mode == axis_mode::absolute) {

			p.tablet.mapping = in[i++] ? tablet_mapping::window : tablet_mapping::desktop;
			p.tablet.fp = in[i++] ? float_unit::x87 : float_unit::sse;
			p.tablet.span = (float)read_u(in, i);
			p.tablet.origin = (float)rc::unzz(read_u(in, i));
			p.tablet.half = (float)read_u(in, i);
			p.tablet.speed = from_hundredths(read_u(in, i));
			p.field.scale = read_float(in, i);
			p.field.off = read_float(in, i);

		}

		return p;
	}

	inline std::vector<u8> write_info(const _model_info& m) {

		bit_stream o{};

		for (const auto& ax : m.axis) {

			write_u((u32)ax.seg.size(), o);

			u32 prev{};

			for (const auto& sg : ax.seg) {

				write_u(sg.start - prev, o);
				prev = sg.start;
				write_params(sg.p, o);

			}

		}

		return o.data;
	}

	inline bool read_info(std::span<const u8> data, _model_info& out) {

		if (data.empty())
			return false;

		const _bit_view in{ data };

		size_t i{};
		_model_info m{};

		for (auto& ax : m.axis) {

			const u32 n{ read_u(in, i) };

			if (n == 0 || n > 100000)
				return false;

			ax.seg.resize(n);

			u32 prev{};

			for (auto& sg : ax.seg) {

				sg.start = prev += read_u(in, i);
				sg.p = read_params(in, i);

				if (sg.p.mode > axis_mode::text ||
					(sg.p.mode == axis_mode::mouse && !(sg.p.mouse.poll >= 0.f && sg.p.mouse.poll <= 8000.f)))
						return false; // no detector produces these
			}

			if (ax.seg[0].start != 0)
				return false;

		}

		out = std::move(m);

		return true;
	}

	// moves are coded as d - pred, then mouse slack

	constexpr u32 MOVE_INC{ 144 }, SLACK_INC{ 40 };

	static_assert(rc::safe_increment(16, MOVE_INC, DEVICE_PRIOR_WEIGHT) && rc::safe_increment(16, SLACK_INC, 0),
		"device move / slack coder increment can overflow the 16 bit frequencies");

	// 0, 1-2, more
	inline int miss_class(i64 res) {

		const i64 m{ res < 0 ? -res : res };

		return m == 0 ? 0 : m <= 2 ? 1 : 2;
	}

	// mode, |pred| bucket, axis, first axis miss, previous miss, pred sign, run (0 / same way / reversed)
	inline u64 move_ctx(const axis_params& p, i64 pred, int a, int first_miss, int prev_miss, int run) {

		const u64 base{ ((u64)p.mode * 32 + (u64)rc::bk(pred)) * 2 + (u64)a };
		const u64 sign{ pred < 0 ? 0u : pred == 0 ? 1u : 2u };

		return (((base * 3 + (u64)first_miss) * 3 + (u64)prev_miss) * 3 + sign) * 3 + (u64)run;
	}

	// top bit context, per mode and axis
	inline int move_bit_ctx(const axis_params& p, int a) {
		return (int)p.mode * 2 + a;
	}

	inline rc::value_coder make_move_coder() {

		rc::value_coder c{ 16, MOVE_INC };

		c.prior = { DEVICE_PRIOR_ROW.data(), DEVICE_PRIOR_START, DEVICE_PRIOR_VALUES, DEVICE_PRIOR_ROWS };
		c.prior_weight = DEVICE_PRIOR_WEIGHT;

		c.enable_top_bit(5 * 2);

		return c;
	}

	struct move_predictor {

		static constexpr size_t WINDOW_FRAMES{ 8 };
		static constexpr i64 WINDOW_MS{ 8 }, ACCEL_MS{ 12 }, SCALE_MIN{ 4 };

		std::span<const int> dt;
		i64 hist[2][WINDOW_FRAMES]{};

		static i64 ms(int v) {
			return std::clamp(v, 1, 40);
		}

		// n / m rounded half away from zero, m > 0
		static i64 round_div(i64 n, i64 m) {
			return n >= 0 ? (n + m / 2) / m : -((-n + m / 2) / m);
		}

		i64 window(const int a, const size_t i) const {

			if (i < 2)
				return 0;

			i64 moved{}, span{};

			for (size_t j{}; j < WINDOW_FRAMES && j + 2 <= i; ++j) {

				moved += hist[a][j];
				span += ms(dt[i - 1 - j]);

				if (span >= WINDOW_MS)
					break;
			}

			return round_div(moved * ms(dt[i]), std::max(span, 1ll));
		}

		i64 pred(int a, size_t i) const {

			i64 p{ window(a, i) };

			if (i >= 3 && ms(dt[i]) >= ACCEL_MS && ms(dt[i - 1]) >= ACCEL_MS && ms(dt[i - 2]) >= ACCEL_MS)
				p += (hist[a][0] - hist[a][1]) * 3 / 8;

			return p;
		}

		int first(size_t i) const {
			return std::abs(window(1, i)) > std::abs(window(0, i)) ? 1 : 0;
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

		i64 miss[2]{};

		u64 ctx(const axis_params& p, i64 pred, int a, int first_miss) const {

			const i64 h0{ hist[a][0] }, h1{ hist[a][1] };

			const int run{ (h0 == 0 || h1 == 0) ? 0 :
				((h0 > 0) == (h1 > 0) ? 1 : 2)
			};

			return move_ctx(p, pred, a, first_miss, miss_class(miss[a]), run);
		}

		void push(int a, i64 d, i64 pred) {

			for (size_t j{ WINDOW_FRAMES - 1 }; j > 0; --j)
				hist[a][j] = hist[a][j - 1];

			hist[a][0] = d;
			miss[a] = d - pred;

		}

	};

	// shared by encoder and decoder: larger predicted axis first, the second scaled by the first's outcome
	template<typename Move, typename Escaped, typename Slack>
	void move_loop(const axis_model p[2], std::span<const int> dt, size_t n, Move move, Escaped escaped, Slack slack) {

		move_predictor mp{ dt };

		for (size_t i{}; i < n; ++i) {

			const int first{ mp.first(i) };
			i64 first_d{}, first_pred{};

			for (int q{}; q < 2; ++q) {

				const int a{ q ? 1 - first : first };

				i64 pr{ mp.pred(a, i) };

				if (q)
					pr = move_predictor::scaled(pr, first_d, first_pred);

				const axis_params& params{ p[a].at(i) };
				const int miss{ q ? miss_class(first_d - first_pred) : 0 };
				const i64 d{ move(a, i, mp.ctx(params, pr, a, miss), move_bit_ctx(params, a), pr) };

				if (!q) {

					first_d = d;
					first_pred = pr;

				}

				mp.push(a, d, pr);

				if (!escaped(a, i) && params.mode == axis_mode::mouse && d != 0)
					slack(a);

			}

		}

	}

	// steps past escapes[at] if it is frame i
	template<typename E>
	FORCEINLINE bool next_escape(const E& escapes, size_t& at, size_t i) {
		if (at < escapes.size() && escapes[at].first == i) {

			++at;

			return true;
		}

		return false;
	}

	inline std::vector<u8> write_moves_rc(const axis_model p[2], const axis_streams st[2], std::span<const int> dt) {

		rc::rc_encoder e{};
		rc::value_coder moves{ make_move_coder() }, slack{ 16, SLACK_INC };
		size_t esc[2]{}, sl[2]{};

		move_loop(p, dt, st[0].K.size(),
			[&](int a, size_t i, u64 ctx, int bit_ctx, i64 pr) {

				const i64 d{ st[a].K[i] - (i ? st[a].K[i - 1] : 0) };

				moves.encode(e, ctx, rc::zz(d - pr), bit_ctx);

				return d;
			},
			[&](int a, size_t i) {

				return next_escape(st[a].escapes, esc[a], i);
			},
			[&](int a) {

				slack.encode(e, a, rc::zz(st[a].slack[sl[a]++]));

			});

		return e.finish();
	}

	// estimated coded size of one axis, for model selection
	inline size_t moves_rc_bytes(const axis_params& p, const axis_streams& st, std::span<const int> dt, int a) {

		rc::rc_encoder e{};
		rc::value_coder moves{ make_move_coder() }, slack{ 16, SLACK_INC };
		move_predictor mp{ dt };

		size_t esc{}, sl{};

		for (size_t i{}; i < st.K.size(); ++i) {

			const i64 d{ st.K[i] - (i ? st.K[i - 1] : 0) };
			const i64 pr{ mp.pred(a, i) };

			moves.encode(e, mp.ctx(p, pr, a, 0), rc::zz(d - pr), move_bit_ctx(p, a));
			mp.push(a, d, pr);

			if (!next_escape(st.escapes, esc, i) && p.mode == axis_mode::mouse && d != 0)
				slack.encode(e, a, rc::zz(st.slack[sl++]));

		}

		return e.finish().size();
	}

	constexpr i64 MAX_MOVE{ 1ll << 40 }, MAX_K{ 1ll << 50 };

	inline void read_moves_rc(const std::vector<u8>& data, const axis_model p[2], const std::vector<std::pair<u32, float>> escapes[2],
		std::span<const int> dt, size_t n, std::vector<i64> K[2], std::vector<int> slack_out[2]) {

		rc::rc_decoder dec{ data };
		rc::value_coder moves{ make_move_coder() }, slack{ 16, SLACK_INC };
		size_t esc[2]{};

		for (int a{}; a < 2; ++a) {

			K[a].assign(n, 0);
			slack_out[a].clear();

		}

		// valid moves are far below 2^40 and K far below 2^50
		move_loop(p, dt, n,
			[&](int a, size_t i, u64 ctx, int bit_ctx, i64 pr) {

				const i64 res{ std::clamp(rc::unzz(moves.decode(dec, ctx, bit_ctx)), -MAX_MOVE, MAX_MOVE) };
				const i64 d{ std::clamp(res + pr, -MAX_MOVE, MAX_MOVE) };

				K[a][i] = std::clamp((i ? K[a][i - 1] : 0) + d, -MAX_K, MAX_K);

				return d;
			},
			[&](int a, size_t i) {

				return next_escape(escapes[a], esc[a], i);
			},
			[&](int a) {

				slack_out[a].push_back((int)std::clamp<i64>(rc::unzz(slack.decode(dec, a)), -MOUSE_SLACK_RANGE, MOUSE_SLACK_RANGE));

			});

	}
}
