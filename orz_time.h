#pragma once

#include "time_prior.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace frame_time {

	constexpr int TIME_EXACT{ 64 };
	constexpr u32 TIME_INC{ 256 };

	static_assert([] {
		for (const u32 w : TIME_PRIOR_WEIGHT)
			if (!rc::safe_increment(TIME_EXACT, TIME_INC, w))
				return false;
		return true;
	}(), "time coder increment / prior weight can overflow the 16 bit frequencies");

	enum kind : u32 { osu, mania, taiko, fruits };

	FORCEINLINE rc::value_coder make_coder(kind k) {

		rc::value_coder c{ TIME_EXACT, TIME_INC };

		c.prior = { TIME_PRIOR_ROW.data() + TIME_PRIOR_FIRST_ROW[k], TIME_PRIOR_START, TIME_PRIOR_VALUES, TIME_PRIOR_ROWS[k] };
		c.prior_weight = TIME_PRIOR_WEIGHT[k];

		return c;
	}
	
	struct _grid_period {
		i64 num, den;
	};

	constexpr _grid_period GRID_PERIODS[]{ { 50, 3 }, { 25, 1 }, { 25, 2 }, { 100, 3 }, { 20, 1 }, { 0, 1 } };
	constexpr u32 GRID_PERIOD_COUNT{ 6 }, GRID_BINS{ 25 };

	FORCEINLINE u64 grid_phase(i64 now, const _grid_period& p) {

		if (p.num == 0)
			return 0;

		const i64 phase{ now * p.den };

		switch (p.num) {

			case 50: return ((phase % 50 + 50) % 50) * GRID_BINS / 50;
			case 25: return ((phase % 25 + 25) % 25) * GRID_BINS / 25;
			case 100: return ((phase % 100 + 100) % 100) * GRID_BINS / 100;
			case 20: return ((phase % 20 + 20) % 20) * GRID_BINS / 20;
			default: return (u64)((((phase % p.num + p.num) % p.num) * GRID_BINS) / p.num);

		}
	}

	struct _time_grid {

		_grid_period period{ GRID_PERIODS[0] };

		explicit _time_grid(u32 grid) : period{ GRID_PERIODS[grid < GRID_PERIOD_COUNT ? grid : 0] } {}

		u64 bin(i64 now) const {
			return grid_phase(now, period);
		}

	};

	FORCEINLINE u64 grid_ctx(bool changed, i64 now, size_t i, const _time_grid& time_grid) {
		return i < 2 ? 0 : (u64)changed * 25 + time_grid.bin(now);
	}

	FORCEINLINE std::vector<u8> key_updates(const std::vector<_osr_frame>& r) {

		std::vector<u8> c(r.size());

		for (size_t i{ 1 }; i < r.size(); ++i)
			c[i] = r[i].keys != r[i - 1].keys;

		return c;
	}

	FORCEINLINE std::vector<int> deltas(const std::vector<_osr_frame>& r) {

		std::vector<int> delta(r.size());

		for (size_t i{}; i < r.size(); ++i)
			delta[i] = r[i].delta;

		return delta;
	}
	
	inline std::vector<int> build_table(std::span<const int> delta) {

		std::vector<std::pair<u32, u32>> counts{};

		counts.reserve(64);

		for (const int d : delta)
			unique_add(std::bit_cast<u32>(d), counts);

		std::sort(counts.begin(), counts.end(), pair_sort);

		std::vector<int> table(counts.size());

		for (size_t k{}; k < counts.size(); ++k)
			table[k] = std::bit_cast<int>(counts[k].first);

		return table;
	}

	inline u32 pick_grid(std::span<const int> delta) {

		constexpr int SYM{ 48 };

		u32 counts[GRID_PERIOD_COUNT][GRID_BINS][SYM]{};

		i64 now{};

		for (const int d : delta) {

			const int s{ std::clamp(d, 0, SYM - 1) };

			for (u32 g{}; g < GRID_PERIOD_COUNT; ++g)
				++counts[g][grid_phase(now, GRID_PERIODS[g])][s];

			now += d;

		}

		u32 best{};
		double best_bits{ std::numeric_limits<double>::max() };

		for (u32 g{}; g < GRID_PERIOD_COUNT; ++g) {

			double bits{};

			for (const auto& row : counts[g]) {

				u32 total{};

				for (const u32 c : row)
					total += c;

				for (const u32 c : row)
					if (c)
						bits += c * std::log2((double)total / c) + 0.5 * std::log2((double)total);

			}

			if (bits < best_bits) {

				best_bits = bits;
				best = g;

			}

		}

		return best;
	}

	inline void write_table(_orz_construct& orz, const std::vector<int>& table, u32 grid) {

		bit_stream t{};

		small_write<5>((u32)table.size(), t);

		for (const int v : table)
			small_write<5>((u32)std::abs(v), t);

		for (const int v : table)
			t.push_back(v < 0);

		for (int b{}; b < 3; ++b)
			t.push_back((grid >> b) & 1);

		orz.add_stream(orz_tag::time_delta_table, t.data.data(), t.data.size());

	}

	inline std::vector<int> read_table(const _stream_map& stream_data, u32& grid) {

		const _bit_view t{ stream_data[orz_tag::time_delta_table] };

		size_t bit{};

		std::vector<int> table(std::min<size_t>(small_read<5>(t, bit), t.max_size() / 6));

		for (auto& v : table)
			v = (int)small_read<5>(t, bit);

		for (auto& v : table)
			if (small_read<1>(t, bit))
				v = (int)(0u - (u32)v); // negate without overflow

		grid = 0;

		for (int b{}; b < 3; ++b)
			grid |= (u32)small_read<1>(t, bit) << b;

		return table;
	}
	
	template<typename Ctx>
	void write_indices(_orz_construct& orz, orz_tag tag, kind k, const std::vector<int>& table, std::span<const int> delta, Ctx ctx) {

		rc::rc_encoder e{};
		rc::value_coder coder{ make_coder(k) };

		i64 now{};

		for (size_t i{}; i < delta.size(); ++i) {

			const u32 index{ (u32)(std::find(table.begin(), table.end(), delta[i]) - table.begin()) };

			coder.encode(e, ctx(i, now), index);

			now += delta[i];

		}

		const auto& bytes{ e.finish() };

		orz.add_stream(tag, bytes.data(), bytes.size());
	}
	
	template<typename Ctx, typename Set>
	void read_indices(std::span<const u8> data, kind k, const std::vector<int>& table, size_t n, Ctx ctx, Set set) {

		rc::rc_decoder d{ data };
		rc::value_coder coder{ make_coder(k) };

		i64 now{};

		for (size_t i{}; i < n; ++i) {

			const u64 index{ coder.decode(d, ctx(i, now)) };
			const int delta{ index < table.size() ? table[index] : 0 };

			set(i, delta);

			now += delta;

		}

	}
	
	inline void write_grid(_orz_construct& orz, orz_tag tag, kind k, const std::vector<int>& delta, const std::vector<u8>& changed) {

		const auto table{ build_table(delta) };
		const u32 grid{ pick_grid(delta) };

		write_table(orz, table, grid);

		const _time_grid time_grid{ grid };

		write_indices(orz, tag, k, table, delta, [&](size_t i, i64 now) {

			return grid_ctx(changed[i], now, i, time_grid);
		});

	}

	inline std::vector<int> read_grid(const _stream_map& stream_data, orz_tag tag, kind k, const std::vector<u8>& changed) {

		std::vector<int> delta(changed.size());

		u32 grid{};

		const auto table{ read_table(stream_data, grid) };
		const _time_grid time_grid{ grid };

		read_indices(stream_data[tag], k, table, delta.size(),
			[&](size_t i, i64 now) {

				return grid_ctx(changed[i], now, i, time_grid);
			},
			[&](size_t i, int d) {

				delta[i] = d;

			});

		return delta;
	}

}
