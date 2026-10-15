#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace frame_time {

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

	// the delta table and time grid, at the front of the stream holding the times
	inline void write_table(bit_stream& t, const std::vector<int>& table, u32 grid) {

		small_write<5>((u32)table.size(), t);

		
		for (const int v : table) {

			if (v >= 0)
				small_write<5>((u32)v + 1, t);
			else {

				small_write<5>(0, t);
				small_write<5>(~(u32)v, t);

			}

		}

		for (int b{}; b < 3; ++b)
			t.push_back((grid >> b) & 1);

	}

	// from bit `bit` of t, which ends past it
	inline std::vector<int> read_table(const _bit_view t, size_t& bit, u32& grid) {

		std::vector<int> table(std::min<size_t>(small_read<5>(t, bit), t.max_size() / 5));

		for (auto& v : table) {

			const u32 u{ small_read<5>(t, bit) };

			v = (int)(u ? u - 1 : ~small_read<5>(t, bit));

		}

		grid = 0;

		for (int b{}; b < 3; ++b)
			grid |= (u32)small_read<1>(t, bit) << b;

		return table;
	}

}
