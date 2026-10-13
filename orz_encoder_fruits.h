#pragma once

#include <bit>
#include <charconv>
#include <vector>
#include "fruits_prior.h"

namespace fruits {

	constexpr float CATCHER_Y{ 9999.f };
	constexpr float DUMMY_X{ 256.f }, DUMMY_Y{ -500.f };
	constexpr size_t DUMMY_FRAMES{ 2 };
	constexpr double HYPER_MIN_RATIO{ 1.3 };

	constexpr u32 MOVE_INC{ 20 }, RES_INC{ 48 };
	constexpr int MOVE_EXACT{ 2 }, RES_EXACT{ 32 };

	static_assert(rc::safe_increment(MOVE_EXACT, MOVE_INC, 0) && rc::safe_increment(RES_EXACT, RES_INC, FRUITS_PRIOR_WEIGHT),
		"catch coder increment / prior weight can overflow the 16 bit frequencies");

	FORCEINLINE double walk_speed(u32 keys) {
		return (keys & 1) ? 1.0 : 0.5;
	}

	// dotnet::text_ord, negative positions are escaped
	FORCEINLINE bool text_ordinal(const float x, const bool exact_text, i64& ord) {
		return !std::signbit(x) && dotnet::text_ord(x, exact_text, ord);
	}

	inline bool escaped(const _osr_frame& f, size_t i, bool exact_text) {

		i64 ord;

		return i < DUMMY_FRAMES || !same_bits(f.y, CATCHER_Y) || !text_ordinal(f.x, exact_text, ord);
	}

	struct catcher {

		float x{ DUMMY_X };
		i64 x_ord{ -1 }; // -1 means none
		double hyper{ 1.0 };

		int dir{ 1 };

		u8 moved{};
		u8 exact_text{};

		FORCEINLINE u64 move_ctx(bool key_changed) const {

			return (u64)moved * 2 + key_changed;
		}

		FORCEINLINE u64 dir_ctx() const {

			return moved;
		}

		// speed multiplier carried into this frame
		FORCEINLINE double carried_mul(int d) const {

			return (moved && d == dir) ? hyper : 1.0;
		}

		FORCEINLINE float predict(int d, u32 keys, int delta) const {

			const double from{
				std::isfinite(x) ? (double)x : (double)DUMMY_X
			};

			return (float)std::clamp(from + d * walk_speed(keys) * carried_mul(d) * delta, 0.0, 512.0);
		}

		FORCEINLINE u64 res_ctx(int d, bool key_changed, float pred, int delta) const {

			const u64 decade{ u64(pred >= 10.f) + u64(pred >= 100.f) };

			const u64 dt{ delta < 8 ? 0u :
				(delta < 15 ? 1u :
				(delta <= 17 ? 2u : 3u))
			};

			return ((((u64)moved * 2 + key_changed) * 2 + (carried_mul(d) != 1.0)) * 3 + decade) * 4 + dt;
		}

		void move(float nx, i64 nx_ord, int d, u32 keys, int delta) {

			const double ratio{ std::abs((double)nx - (double)x) / walk_speed(keys) / std::max(delta, 1) };
			const bool inside{ nx > 0.f && nx < 512.f };

			hyper = (ratio > HYPER_MIN_RATIO && inside && delta >= 4 && std::isfinite(ratio)) ? ratio : 1.0;

			x = nx;
			x_ord = nx_ord;
			moved = true;
			dir = d;

		}

		// escaped frame
		void set(float nx) {

			x = nx;

			if (!text_ordinal(nx, exact_text, x_ord))
				x_ord = -1;

			moved = false;
			hyper = 1.0;

		}
	};

	static_assert(FRUITS_PRIOR_ROWS == 2 * 2 * 2 * 3 * 4, "fruits_prior.h does not match the residual context layout");

	inline rc::value_coder make_res_coder() {

		rc::value_coder c{ RES_EXACT, RES_INC };

		static_assert(FRUITS_PRIOR_ALPHABET == RES_EXACT + (65 - 6), "fruits_prior.h alphabet");

		c.prior = { FRUITS_PRIOR_ROW.data(), FRUITS_PRIOR_START, FRUITS_PRIOR_VALUES, FRUITS_PRIOR_ROWS };
		c.prior_weight = FRUITS_PRIOR_WEIGHT;

		c.enable_top_bit(FRUITS_PRIOR_ROWS);

		return c;
	}

	struct x_models {
		rc::value_coder moved{ MOVE_EXACT, MOVE_INC }, dir{ MOVE_EXACT, MOVE_INC }, res{ make_res_coder() };
	};

	inline std::vector<u8> encode_x(const _osr& r, const std::vector<u8>& changed, bool exact_text) {

		rc::rc_encoder e{};
		x_models m{};
		catcher c{};

		c.exact_text = exact_text;

		for (size_t i{}; i < r.size(); ++i) {

			if (escaped(r[i], i, exact_text)) {

				c.set(r[i].x);

				continue;
			}

			i64 x_ord{};

			text_ordinal(r[i].x, exact_text, x_ord); // escaped() checked it has one

			const bool moved{ x_ord != c.x_ord };

			m.moved.encode(e, c.move_ctx(changed[i]), moved);

			if (!moved) {

				c.moved = false;
				c.hyper = 1.0;

				continue;
			}

			const float x{ dotnet::from_text_ord(x_ord, exact_text) };
			const int d{ x > c.x ? 1 : -1 };

			m.dir.encode(e, c.dir_ctx(), d == c.dir);

			const float pred{ c.predict(d, r[i].keys, r[i].delta) };

			i64 pred_ord{};

			text_ordinal(pred, exact_text, pred_ord);

			const u64 ctx{ c.res_ctx(d, changed[i], pred, r[i].delta) };

			m.res.encode(e, ctx, rc::zz(x_ord - pred_ord), (int)ctx);

			c.move(x, x_ord, d, r[i].keys, r[i].delta);

		}

		return e.finish();
	}

	void encode(const char* output_file, const _osr& r, const u32 flags, orz_state& state) {

		_orz_construct result{ &state };

		result.frame_count = r.size();
		result.flags = flags | ORZ_FLAG::fruits_catcher;

		const auto changed{ frame_time::key_updates(r) };

		frame_time::write_grid(result, orz_tag::fruits_time_rc, frame_time::fruits, frame_time::deltas(r), changed);

		{

			std::vector<u32> esc{};

			for (size_t i{}; i < r.size(); ++i)
				if (escaped(r[i], i, state.exact_text))
					esc.push_back((u32)i);

			const bit_stream s{ position_escapes(r, esc, DUMMY_X, DUMMY_Y) };

			result.add_compress_stream(orz_tag::fruits_escape, s, comp::pos_delta_prop());

		}

		{

			const auto bytes{ encode_x(r, changed, state.exact_text) };

			result.add_stream(orz_tag::fruits_x_rc, bytes.data(), bytes.size());

		}

		_key_data_constructor kd{};

		for (const auto& f : r)
			kd.tick(f.keys);

		result.frames_crc = _orz_construct::crc_of(r, state.seed);

		result.write(output_file, kd);
	}

	u32 decode(const _orz_header* header, const _stream_map& stream_data, std::vector<_osr_frame>& R) {

		const size_t total_size{ R.size() };

		const bool exact_text{ (header->flags & ORZ_FLAG::text_shortest) != 0 };

		const auto key_update{ frame_time::key_updates(R) };

		const auto delta{ frame_time::read_grid(stream_data, orz_tag::fruits_time_rc, frame_time::fruits, key_update) };

		if (delta.size() < total_size)
			return 1;

		for (size_t i{}; i < total_size; ++i) {

			R[i].delta = delta[i];
			R[i].y = CATCHER_Y;

		}

		std::vector<bool> esc(total_size);

		{

			const _bit_view escape_stream{ stream_data[orz_tag::fruits_escape] };

			const size_t bits_total{ escape_stream.max_size() };

			size_t bit{};
			u64 frame{};

			const u32 count{ read_bucket(escape_stream, bit, 4, 8, 16) };

			if (count > total_size || count > bits_total / 2)
				return 1;

			for (size_t k{}; k < count; ++k) {

				frame += read_bucket(escape_stream, bit, 4, 8, 16);

				if (frame >= total_size || bit >= bits_total)
					return 1;

				esc[frame] = true;

				if (escape_stream[bit++]) {

					R[frame].x = DUMMY_X;
					R[frame].y = DUMMY_Y;

					continue;
				}

				if (bit + 64 > bits_total) // raw x and y cut short
					return 1;

				R[frame].x = read_float(escape_stream, bit);
				R[frame].y = read_float(escape_stream, bit);

			}

		}

		rc::rc_decoder d{ stream_data[orz_tag::fruits_x_rc] };

		x_models m{};
		catcher c{};

		c.exact_text = exact_text;

		for (size_t i{}; i < total_size; ++i) {

			if (esc[i]) {

				c.set(R[i].x);

				continue;
			}

			if (!m.moved.decode(d, c.move_ctx(key_update[i]))) {

				R[i].x = c.x;
				c.moved = false;
				c.hyper = 1.0;

				continue;
			}

			const int dir{ m.dir.decode(d, c.dir_ctx()) ? c.dir : -c.dir };

			const float pred{ c.predict(dir, R[i].keys, R[i].delta) };

			i64 pred_ord{};

			text_ordinal(pred, exact_text, pred_ord);

			const u64 ctx{ c.res_ctx(dir, key_update[i], pred, R[i].delta) };
			
			const i64 x_ord{ pred_ord + std::clamp(rc::unzz(m.res.decode(d, ctx, (int)ctx)), -(1ll << 40), 1ll << 40) };
			const float x{ dotnet::from_text_ord(x_ord, exact_text) };

			R[i].x = x;

			c.move(x, x_ord, dir, R[i].keys, R[i].delta);

		}

		return 0;
	}

}
