#pragma once

// osu!standard frames as one interleaved stream. Per frame: keys, time delta, position repeat flag and, for a new
// position, both cursor moves (residuals of the move predictor) plus any mouse slack. Every value is a sequence of
// binary decisions coded with context mixing (cm.h), and one templated loop serves encoder, decoder and prior training.

#include "cm.h"
#include "frames_prior.h"

#include <algorithm>
#include <bit>
#include <span>
#include <vector>

namespace frames {

	// most common time deltas of non relax replays first; those code a delta as its place here (escape past the end)
	constexpr int GLOBAL_DELTAS[]{
		17, 16, 18, 15, 14, 13, 25, 4, 3, 6, 5, 8, 9, 7, 12, 10, 2, 11, 1, 19, 0, 24, 26, 20, 23, 21, 22, 27, 28, 29, 31, 30,
		33, 32, 34, 40, 35, -1, 36, 37, 39, 38, 41, 42, 67, 66, 50, 43, 44, 45, 49, 46, 48, 82, 47, 51, 81, 52, 53, 55, 54, 56
	};
	constexpr u32 GLOBAL_DELTA_COUNT{ (u32)(sizeof(GLOBAL_DELTAS) / sizeof(int)) };

	// relax replays code a delta as its place in their own table, the unary covers this many entries
	constexpr u32 RELAX_LISTED{ 64 };

	constexpr int KEY_BITS{ 5 }; // M1 M2 K1 K2 smoke (folded like the key stream), higher bits are escaped

	// valid moves are far below 2^40 and K far below 2^50, only damaged files reach these
	constexpr i64 MAX_MOVE{ 1ll << 40 }, MAX_K{ 1ll << 50 };

	struct _frame_data {
		std::vector<int> delta;
		std::vector<u32> keys;           // folded, K1 / K2 imply M1 / M2
		std::vector<u8> repeated;
		std::vector<i64> K[2];           // per new position
		std::vector<int> slack[2];       // per axis, in coding order
		std::vector<int> dt;             // per new position, ms since the previous one
	};

	struct _side {
		const dspace::axis_model* axis{};
		const std::vector<u32>* escapes[2]{}; // escaped positions per axis, ascending
		std::vector<int> table;               // relax: the replay's delta table
		frame_time::_time_grid grid{ 0 };
		bool relax{};
	};

	struct _models {

		cm::model keys_changed, keys_bits, keys_high, time, repeat, move, move_bits, slack;

		// tables follow the replay's length up to a cap (the frame count of a file to decode is untrusted),
		// and are larger when training priors
		void init(size_t frames, bool record_keys = false) {

			const int b{ (int)std::bit_width(frames) }, extra{ record_keys ? 4 : 0 };

			// single decision models: a counter per line; time / move / slack: a line of counters per context
			keys_changed.init(7, std::clamp(b + 4, 16, 22 + extra), 1, 64, 64, record_keys);
			keys_bits.init(6, std::clamp(b + 2, 14, 20 + extra), 1, 64, 64, record_keys);
			keys_high.init(1, 8, 1, 1, 1, record_keys);
			repeat.init(7, std::clamp(b + 4, 16, 22 + extra), 1, 64, 64, record_keys);
			time.init(7, std::clamp(b, 12, 16 + extra), 64, 64 * 3, 64 * 3, record_keys);

			init_moves(frames, 8, record_keys);

		}

		// move_inputs below 8 gives a cheaper move model (device model selection estimates)
		void init_moves(size_t positions, int move_inputs, bool record_keys = false) {

			const int b{ (int)std::bit_width(positions) }, extra{ record_keys ? 4 : 0 };

			move.init(move_inputs, std::clamp(b + 3, 12, 19 + extra), 16, 32 * 4, 32 * 32, record_keys);
			move_bits.init(4, std::clamp(b + 2, 12, 20 + extra), 1, 8 * 4, 8 * 32, record_keys);
			slack.init(3, std::clamp(b - 2, 10, 16), 16, 64, 64, record_keys);

		}

	};

	FORCEINLINE int bit_length(i64 v) {

		const u64 m{ v < 0 ? 0 - u64(v) : u64(v) };

		return m == 0 ? 0 : (int)std::bit_width(m);
	}

	FORCEINLINE u64 log_bucket(i64 v) {
		return v <= 0 ? 0 : (u64)std::min(24, (int)std::bit_width((u64)v));
	}

	// a / b in eighths up to 4, 33 when there is no reference
	FORCEINLINE u64 ratio_bucket(i64 a, i64 b) {

		if (b <= 0)
			return 33;

		return (u64)std::clamp<i64>((a * 8 + b / 2) / b, 0, 32);
	}

	// raw bits (escapes), up to 64
	template<typename Coder>
	FORCEINLINE u64 raw(Coder& c, u64 v, int bits) {

		u64 out{};

		for (int b{ bits - 1 }; b >= 0; --b)
			out = (out << 1) | (u64)c.code((int)((v >> b) & 1), 32768);

		return out;
	}

	// Elias gamma of v >= 1 with raw bits
	template<typename Coder>
	FORCEINLINE u64 gamma(Coder& c, u64 v) {

		const int len{ v ? 63 - std::countl_zero(v) : 0 };

		int n{};

		while (n < 63 && c.code(len > n, 32768))
			++n;

		return (u64(1) << n) | raw(c, v, n);
	}

	struct _move_features {
		int mode, axis, order, bkp, first_miss, prev_miss, sign, run, scale, regime, first_res, prev_res, dt, since_move, gap;
	};

	// what the move features remember per axis
	struct _axis_history {

		i64 scale16{ 64 }, prev_res{}; // scale16: running mean of |residual| * 16
		int since_move{}, gap{};

		FORCEINLINE void fill(_move_features& f, int dt) const {

			f.scale = std::min(31, bit_length(scale16 >> 2));
			f.regime = dt <= 3 ? 0 : dt <= 11 ? 1 : 2;
			f.prev_res = (int)std::clamp<i64>(prev_res, -20, 20);
			f.dt = dt;
			f.since_move = std::min(since_move, 15);
			f.gap = std::min(gap, 15);

		}

		FORCEINLINE void push(i64 res, i64 d) {

			scale16 += ((std::min<i64>(res < 0 ? -res : res, 1 << 24) << 4) - scale16) >> 2;
			prev_res = res;

			if (d != 0) {

				gap = since_move;
				since_move = 0;

			} else
				++since_move;

		}

	};

	// fields of dspace::move_ctx
	FORCEINLINE _move_features move_features(u64 ctx) {

		_move_features f{};

		f.run = (int)(ctx % 3); ctx /= 3;
		f.sign = (int)(ctx % 3); ctx /= 3;
		f.prev_miss = (int)(ctx % 3); ctx /= 3;
		f.first_miss = (int)(ctx % 3); ctx /= 3;
		f.axis = (int)(ctx % 2); ctx /= 2;
		f.bkp = (int)(ctx % 32); ctx /= 32;
		f.mode = (int)ctx;

		return f;
	}

	// move residual: zero, sign, bit length in unary, two modelled bits below the leading one, then raw bits
	template<typename Coder>
	i64 move_residual(Coder& c, _models& M, i64 r, const _move_features& f) {

		const u64 ctx[8]{
			(u64)f.scale * 4 + f.regime,
			(((u64)f.bkp * 3 + f.regime) * 2 + f.order) * 5 + f.mode,
			(((((((u64)f.mode * 64 + f.bkp) * 2 + f.axis) * 3 + f.first_miss) * 3 + f.prev_miss) * 3 + f.sign) * 3 + f.run) * 32 + f.scale,
			(((u64)f.scale * 20 + std::min(f.first_res, 19)) * 2 + f.order) * 3 + f.regime,
			((u64)(f.prev_res + 40) * 32 + f.scale) * 3 + f.regime,
			((u64)std::min(f.dt, 40) * 32 + f.bkp) * 2 + f.order,
			(((u64)f.since_move * 16 + f.gap) * 8 + std::min(f.bkp, 7)) * 3 + f.regime,
			((u64)f.since_move * 16 + f.gap) * 41 + (u64)(f.prev_res + 20),
		};

		const size_t apc{ (size_t)std::min(f.scale, 31) };

		// slots: 0 zero, 1-3 sign (by the prediction's sign), 4-15 bit length steps, then a second line for longer ones
		M.move.bind(ctx);

		const auto decide = [&](int y, u32 step) {

			const u32 slot{ step & 15 };

			return M.move.code(c, y, slot, (size_t)step * 4 + (size_t)f.regime, (size_t)step * 32 + apc);
		};

		if (decide(r == 0, 0))
			return 0;

		const int negative{ decide(r < 0, 1 + (u32)f.sign) };

		const u64 v{ r < 0 ? 0 - u64(r) : u64(r) };
		const int len{ v ? 63 - std::countl_zero(v) : 0 };

		int n{};

		while (n < 12 && decide(len > n, 4 + (u32)n))
			++n;

		if (n == 12) {

			M.move.bind(ctx, 1);

			while (n < 62 && decide(len > n, 16 + (u32)std::min(n - 12, 15)))
				++n;

		}

		u64 out{ 1 };

		for (int b{ n - 1 }, k{}; b >= 0; --b, ++k) {

			const int bit{ (int)((v >> b) & 1) };

			if (k < 2) {

				const u64 id{ (u64)k * 4 + (out & ((u64(1) << k) - 1)) };
				const u64 cx[4]{ ((u64)n * 16 + id) * 64, ((ctx[1] * 64 + (u64)n) * 64 + id) * 2, ((ctx[0] * 64 + (u64)n) * 64 + id) * 2 + 1, (u64)n * 64 + id + 7 };

				out = (out << 1) | (u64)M.move_bits.code(c, bit, cx, (size_t)id * 4 + (size_t)f.regime, (size_t)id * 32 + (size_t)std::min(n, 31));

			} else
				out = (out << 1) | (u64)c.code(bit, 32768);

		}

		return negative ? -(i64)out : (i64)out;
	}

	// mouse slack in ulps (|slack| <= MOUSE_SLACK_RANGE), unary on its zigzag
	template<typename Coder>
	int slack_value(Coder& c, cm::model& m, int s, int axis, i64 d) {

		const u64 z{ rc::zz(s) };
		const u64 ctx[3]{ (u64)axis * 64 + (u64)bit_length(d), (u64)axis, 0 };

		m.bind(ctx);

		u64 v{};

		while (v < 2 * (u64)dspace::MOUSE_SLACK_RANGE) {

			const u32 slot{ (u32)std::min<u64>(v, 15) };

			if (!m.code(c, z > v, slot, slot, slot))
				break;

			++v;

		}

		return (int)rc::unzz(v);
	}

	// encoding reads fd, decoding (encoding false) fills it; side must match on both ends
	template<typename Coder>
	void frame_loop(Coder& c, _models& M, _frame_data& fd, const _side& side, size_t frames, bool encoding) {

		if (!encoding) {

			fd.delta.assign(frames, 0);
			fd.keys.assign(frames, 0);
			fd.repeated.assign(frames, 0);

			for (int a{}; a < 2; ++a) {

				fd.K[a].clear();
				fd.slack[a].clear();

			}

			fd.dt.clear();

		}

		const auto clamp_delta = [](int d) { return (u64)std::clamp(d, -2, 40) + 2; };

		// keys
		i64 now{}, last_change{ -100000 }, last_press{ -100000 }, press_interval{}, prev_hold{};
		i64 press_start[KEY_BITS]{}, key_last_press[KEY_BITS], key_interval[KEY_BITS]{}, key_prev_hold[KEY_BITS]{};
		int frames_since_change{}, last_key{ -1 }, prev_key{ -1 };

		for (auto& v : key_last_press)
			v = -100000;

		// time
		int zero_run{}, prev_zero_run{}, one_run{}, since_new{};

		// repeat flag
		int repeat_run{};
		i64 repeat_since{}, last_move{};

		// moves
		dspace::move_predictor mp{};
		_axis_history history[2]{};
		size_t i{}, escape_at[2]{}, slack_at[2]{};
		i64 elapsed{}, first_res{};

		for (size_t f{}; f < frames; ++f) {

			const u32 pk{ f ? fd.keys[f - 1] : 0u };
			const u64 d1{ f >= 1 ? clamp_delta(fd.delta[f - 1]) : 50 };

			{ // keys

				const u64 phase{ side.grid.bin(now) };
				const bool down{ (pk & ((1u << KEY_BITS) - 1)) != 0 };

				i64 held{};

				if (down) {

					i64 earliest{ now };

					for (int b{}; b < KEY_BITS; ++b)
						if ((pk >> b) & 1)
							earliest = std::min(earliest, press_start[b]);

					held = now - earliest;

				}

				const u64 rhythm{ ratio_bucket(now - last_press, press_interval) }, hold{ down ? ratio_bucket(held, prev_hold) : 34 };

				// the released key closest to its usual press interval
				u64 due{ 33 };

				for (int b{}, best{ 1000 }; b < KEY_BITS; ++b) {

					if (key_interval[b] <= 0 || ((pk >> b) & 1))
						continue;

					const u64 q{ ratio_bucket(now - key_last_press[b], key_interval[b]) };

					if (std::abs((int)q - 8) < best) {

						best = std::abs((int)q - 8);
						due = q;

					}

				}

				const u64 s{ std::min<u64>(pk, 63) };
				const u64 ctx[7]{
					(s * 40 + due) * 2,
					s * 32 + log_bucket(now - last_change),
					s * 40 + rhythm,
					s * 40 + hold,
					((s * 64 + d1) * 32 + phase) * 2,
					(s * 32 + log_bucket(press_interval)) * 32 + log_bucket(now - last_press),
					(s * 32 + (u64)std::min(frames_since_change, 31)) * 32 + log_bucket(now - last_change),
				};

				const u32 want{ encoding ? fd.keys[f] : 0u };

				u32 k{ pk };

				if (M.keys_changed.code(c, want != pk, ctx, s, s)) {

					const u32 flip{ want ^ pk };

					u32 flipped{};

					for (int b{}; b < KEY_BITS; ++b) {

						const u64 is_down{ (pk >> b) & 1 };
						const u64 key_rhythm{ ratio_bucket(now - key_last_press[b], key_interval[b]) };
						const u64 key_hold{ is_down ? ratio_bucket(now - press_start[b], key_prev_hold[b]) : 34 };
						const u64 bctx[6]{
							(((u64)b * 40 + key_rhythm) * 40 + key_hold) * 2 + is_down,
							(u64)b * 64 + log_bucket(now - key_last_press[b]) * 2 + is_down,
							(s * 64 + (flipped & 63)) * 32 + (u64)b,
							(((u64)b * 33 + (u64)(last_key + 1)) * 33 + (u64)(prev_key + 1)) * 2 + is_down,
							((u64)b * 40 + rhythm) * 2 + is_down,
							((u64)b * 40 + hold) * 2 + is_down,
						};

						const size_t set{ (size_t)b * 2 + (size_t)is_down };

						flipped |= (u32)M.keys_bits.code(c, (int)((flip >> b) & 1), bctx, set, set) << b;

					}

					// a change outside the buttons above (never seen in osu!standard, kept for exactness)
					const u64 hctx[1]{ flipped != 0 };
					const u32 high{ flipped == 0 || M.keys_high.code(c, (flip >> KEY_BITS) != 0, hctx, 0, 0) ?
						(u32)raw(c, flip >> KEY_BITS, 32 - KEY_BITS) : 0u };

					k = pk ^ (flipped | (high << KEY_BITS));

				}

				fd.keys[f] = k;

			}

			const u32 k{ fd.keys[f] };
			const bool changed{ k != pk };

			{ // time

				const u64 d2{ f >= 2 ? clamp_delta(fd.delta[f - 2]) : 50 }, d3{ f >= 3 ? clamp_delta(fd.delta[f - 3]) : 50 },
					d4{ f >= 4 ? clamp_delta(fd.delta[f - 4]) : 50 };
				const u64 kc{ changed }, kc1{ f ? (u64)(fd.keys[f - 1] != (f > 1 ? fd.keys[f - 2] : 0u)) : 0 };
				const u64 r1{ f >= 1 ? (u64)fd.repeated[f - 1] : 2 }, r2{ f >= 2 ? (u64)fd.repeated[f - 2] : 2 },
					r3{ f >= 3 ? (u64)fd.repeated[f - 3] : 2 };

				i64 span{};
				int count{};

				for (size_t j{ f }; j > 0 && count < 16; --j, ++count)
					span += std::max(0, fd.delta[j - 1]);

				const u64 rate{ (u64)std::min<i64>(span * 4 / std::max(count, 1), 127) };
				const u64 ctx[7]{
					(d1 * 64 + d2) * 3 + kc,
					((d1 * 64 + d2) * 64 + d3) * 64 + d4,
					(((u64)std::min(zero_run, 15) * 16 + (u64)std::min(prev_zero_run, 15)) * 16 + (u64)std::min(one_run, 15)) * 3 + kc,
					(((u64)side.grid.bin(now) * 64 + d1) * 3 + kc) * 2 + kc1,
					((d1 * 3 + r1) * 3 + r2) * 3 + r3,
					(rate * 64 + d1) * 3 + kc,
					((u64)std::min(since_new, 31) * 64 + d1) * 3 + r1,
				};

				const size_t regime{ d1 <= 5 ? 0u : d1 <= 13 ? 1u : 2u };

				M.time.bind(ctx);

				// unary over a list of deltas, one decision per entry
				const auto place = [&](u32 entries, u32 want, bool last_implied) {

					u32 at{};

					for (; at < entries && !(last_implied && at + 1 == entries); ++at) {

						const size_t set{ (size_t)std::min<u32>(at, 63) * 3 + regime };

						if (M.time.code(c, want == at, std::min<u32>(at, 63), set, set))
							break;

					}

					return at;
				};

				int d{};

				if (side.relax) {

					// the first RELAX_LISTED entries in unary, later ones (rare) as an escaped index
					const u32 n{ (u32)side.table.size() }, listed{ std::min(n, RELAX_LISTED) };
					const u32 want{ encoding ? (u32)(std::find(side.table.begin(), side.table.end(), fd.delta[f]) - side.table.begin()) : 0u };

					u32 at{ place(listed, want, listed == n) };

					if (at == listed && listed < n)
						at = listed + (u32)(gamma(c, encoding ? want - listed + 1 : 0) - 1);

					d = n ? side.table[std::min(at, n - 1)] : 0;

				} else {

					const u32 want{ encoding ? (u32)(std::find(GLOBAL_DELTAS, GLOBAL_DELTAS + GLOBAL_DELTA_COUNT, fd.delta[f]) - GLOBAL_DELTAS) : 0u };
					const u32 at{ place(GLOBAL_DELTA_COUNT, want, false) };

					d = at < GLOBAL_DELTA_COUNT ? GLOBAL_DELTAS[at] : (int)rc::unzz(gamma(c, encoding ? rc::zz(fd.delta[f]) + 1 : 0) - 1);

				}

				fd.delta[f] = d;

			}

			const int d_f{ fd.delta[f] };

			elapsed += d_f;

			bool repeated{};

			if (f) { // repeat flag

				repeat_since += d_f;

				const auto bucket = [](i64 v, i64 hi) { return (u64)std::clamp<i64>(v, -2, hi) + 2; };

				const u64 dd{ bucket(d_f, 30) }, s{ bucket(repeat_since, 40) };
				const u64 p1{ fd.repeated[f - 1] }, p2{ f >= 2 ? (u64)fd.repeated[f - 2] : 2 }, p3{ f >= 3 ? (u64)fd.repeated[f - 3] : 2 };
				const u64 moved{ (u64)std::min<i64>(last_move, 40) };
				const u64 ctx[7]{
					((s * 40 + dd) * 4 + (u64)std::min(repeat_run, 3)) * 2 + (u64)changed,
					((dd * 3 + p1) * 3 + p2) * 2 + (u64)changed,
					s * 40 + dd,
					((p1 * 3 + p2) * 3 + p3) * 40 + dd,
					(s * 41 + moved) * 16 + (u64)std::min(repeat_run, 15),
					(s * 40 + dd) * 41 + moved,
					(u64)std::min(repeat_run, 31) * 40 + dd,
				};

				const size_t set{ (size_t)std::min<u64>(dd, 20) * 3 + (size_t)std::min<u64>(p1, 2) };

				repeated = M.repeat.code(c, encoding ? fd.repeated[f] : 0, ctx, set, set) != 0;

				fd.repeated[f] = repeated;

				if (repeated)
					++repeat_run;
				else {

					repeat_run = 0;
					repeat_since = 0;

				}

			}

			if (!repeated) { // moves, as dspace::move_loop

				const int dt{ (int)std::clamp<i64>(elapsed, INT_MIN, INT_MAX) };

				if (!encoding)
					fd.dt.push_back(dt);

				int pdt{ dt };

				if (i && elapsed - d_f > dspace::move_predictor::REST_MS) {

					mp.rest((int)std::clamp<i64>(elapsed - d_f, INT_MIN, INT_MAX));
					pdt = d_f;

				}

				elapsed = 0;

				const int first{ mp.first(pdt) };

				i64 first_d{}, first_pred{};

				for (int q{}; q < 2; ++q) {

					const int a{ q ? 1 - first : first };

					i64 pr{ mp.pred(a, pdt) };

					if (q)
						pr = dspace::move_predictor::scaled(pr, first_d, first_pred);

					const dspace::axis_params& params{ side.axis[a].at(i) };

					_move_features mf{ move_features(mp.ctx(params, pr, a, q ? dspace::miss_class(first_d - first_pred) : 0)) };

					history[a].fill(mf, dt);

					mf.order = q;
					mf.first_res = q ? bit_length(first_res) * 2 + (first_res < 0) : 0;

					const i64 prev_K{ i ? fd.K[a][i - 1] : 0 };
					const i64 res{ std::clamp(move_residual(c, M, encoding ? fd.K[a][i] - prev_K - pr : 0, mf), -MAX_MOVE, MAX_MOVE) };
					const i64 d{ std::clamp(res + pr, -MAX_MOVE, MAX_MOVE) };

					if (!encoding)
						fd.K[a].push_back(std::clamp(prev_K + d, -MAX_K, MAX_K));

					history[a].push(res, d);

					if (!q) {

						first_d = d;
						first_pred = pr;
						first_res = res;

					}

					mp.push(a, d, pr);

					const auto& esc{ *side.escapes[a] };
					const bool escaped{ escape_at[a] < esc.size() && esc[escape_at[a]] == i };

					if (escaped)
						++escape_at[a];

					if (!escaped && params.mode == dspace::axis_mode::mouse && d != 0) {

						const int s{ slack_value(c, M.slack, encoding ? fd.slack[a][slack_at[a]] : 0, a, d) };

						if (!encoding)
							fd.slack[a].push_back(s);

						++slack_at[a];

					}

				}

				mp.advance(pdt);

				last_move = std::abs(fd.K[0][i] - (i ? fd.K[0][i - 1] : 0)) + std::abs(fd.K[1][i] - (i ? fd.K[1][i - 1] : 0));

				++i;

			}

			// key timing state
			const i64 t{ now + d_f };

			if (changed) {

				const u32 pressed{ k & ~pk }, released{ pk & ~k };

				for (int b{}; b < KEY_BITS; ++b) {

					if ((released >> b) & 1) {

						prev_hold = t - press_start[b];
						key_prev_hold[b] = prev_hold;

					}

					if ((pressed >> b) & 1) {

						key_interval[b] = t - key_last_press[b];
						key_last_press[b] = t;

					}

				}

				if (pressed & ((1u << KEY_BITS) - 1)) {

					press_interval = t - last_press;
					last_press = t;

					for (int b{}; b < KEY_BITS; ++b) {

						if ((pressed >> b) & 1) {

							press_start[b] = t;
							prev_key = last_key;
							last_key = b;

						}

					}

				}

				last_change = t;
				frames_since_change = 0;

			} else
				++frames_since_change;

			if (d_f == 0)
				++zero_run;
			else {

				prev_zero_run = zero_run;
				zero_run = 0;

			}

			one_run = d_f == 1 ? one_run + 1 : 0;
			since_new = repeated ? since_new + std::max(0, d_f) : 0;
			now = t;

		}

	}

	// counters start from priors trained on real replays (frames_prior.h), relax and other replays apart.
	// Relax frame times are coded against the replay's own delta table, so their time model starts flat.
	inline void seed_priors(_models& M, bool relax) {

		cm::model* models[FRAMES_PRIOR_MODELS]{ &M.keys_changed, &M.keys_bits, &M.keys_high, &M.time, &M.repeat, &M.move, &M.move_bits, &M.slack };

		const u32 v{ relax ? 1u : 0u };

		for (u32 m{}; m < FRAMES_PRIOR_MODELS; ++m) {

			if (relax && models[m] == &M.time)
				continue;

			for (u32 j{ FRAMES_PRIOR_START[v][m] }; j < FRAMES_PRIOR_START[v][m + 1]; ++j) {

				const u32 value{ FRAMES_PRIOR_VALUES[j] };

				auto& k{ models[m]->table.line(FRAMES_PRIOR_KEYS[j])[value >> 24] };

				k.p = (u16)(value & 0xFFFF);
				k.n = (u16)std::min<u32>((value >> 16) & 0xFF, FRAMES_PRIOR_CONFIDENCE);

			}

		}

	}

	inline std::vector<u8> encode(_frame_data& fd, const _side& side, size_t frames) {

		_models M{};

		M.init(frames);
		seed_priors(M, side.relax);

		cm::bit_encoder e{};

		frame_loop(e, M, fd, side, frames, true);

		return e.finish();
	}

	inline void decode(std::span<const u8> data, _frame_data& fd, const _side& side, size_t frames) {

		_models M{};

		M.init(frames);
		seed_priors(M, side.relax);

		cm::bit_decoder d{ data };

		frame_loop(d, M, fd, side, frames, false);

	}

	// coded size of one axis's moves and slack on their own, for comparing device models
	inline size_t axis_bytes(const dspace::axis_params& p, const dspace::axis_streams& st, std::span<const int> dt, int a) {

		_models M{};

		M.init_moves(st.K.size(), 3);

		cm::bit_encoder e{};
		dspace::move_predictor mp{};

		_axis_history history{};
		size_t escape_at{}, slack_at{};

		for (size_t i{}; i < st.K.size(); ++i) {

			const i64 d{ st.K[i] - (i ? st.K[i - 1] : 0) };
			const i64 pr{ mp.pred(a, dt[i]) };

			_move_features mf{ move_features(mp.ctx(p, pr, a, 0)) };

			history.fill(mf, dt[i]);
			history.push(move_residual(e, M, d - pr, mf), d);

			mp.push(a, d, pr);
			mp.advance(dt[i]);

			const bool escaped{ escape_at < st.escapes.size() && st.escapes[escape_at].first == i };

			if (escaped)
				++escape_at;

			if (!escaped && p.mode == dspace::axis_mode::mouse && d != 0 && slack_at < st.slack.size())
				slack_value(e, M.slack, st.slack[slack_at++], a, d);

		}

		return e.finish().size();
	}

}
