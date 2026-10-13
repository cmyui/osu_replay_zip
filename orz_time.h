#pragma once

#include "time_prior.h"

#include <algorithm>
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

	
	FORCEINLINE u64 rate_class(const int common_delta) {

		return common_delta < 14 ? 0 :
			(common_delta <= 20 ? 1 : 2);
	}

	FORCEINLINE u64 grid_bin(i64 now) {

		const i64 phase{ ((now * 3) % 50 + 50) % 50 }; // in 1/3 ms

		return (u64)(phase * 25 / 50);
	}

	inline u64 osu_ctx(const std::vector<_osr_frame>& R, size_t i, i64 now, u64 rate) {

		if (i < 2)
			return 0;

		const int a{ R[i].keys != R[i - 1].keys }, b{ R[i - 1].keys != R[i - 2].keys };

		return ((u64)(a * 2 + b) * 25 + grid_bin(now)) * 3 + rate;
	}

	FORCEINLINE u64 grid_ctx(bool changed, i64 now, size_t i) {
		return i < 2 ? 0 : (u64)changed * 25 + grid_bin(now);
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
	
	inline std::vector<int> write_table(_orz_construct& orz, std::span<const int> delta) {

		std::vector<std::pair<u32, u32>> counts{};

		counts.reserve(64);

		for (const int d : delta)
			unique_add(std::bit_cast<u32>(d), counts);

		std::sort(counts.begin(), counts.end(), pair_sort);

		std::vector<int> table(counts.size());

		for (size_t k{}; k < counts.size(); ++k)
			table[k] = std::bit_cast<int>(counts[k].first);

		bit_stream t{};

		small_write<5>((u32)table.size(), t);

		for (const int v : table)
			small_write<5>((u32)std::abs(v), t);

		for (const int v : table)
			t.push_back(v < 0);

		orz.add_stream(orz_tag::time_delta_table, t.data.data(), t.data.size());

		return table;
	}

	inline std::vector<int> read_table(const _stream_map& stream_data) {

		const _bit_view t{ stream_data[orz_tag::time_delta_table] };

		size_t bit{};

		std::vector<int> table(std::min<size_t>(small_read<5>(t, bit), t.max_size() / 6));

		for (auto& v : table)
			v = (int)small_read<5>(t, bit);

		for (auto& v : table)
			if (small_read<1>(t, bit))
				v = (int)(0u - (u32)v); // negate without overflow

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

		const auto table{ write_table(orz, delta) };

		write_indices(orz, tag, k, table, delta, [&](size_t i, i64 now) {

			return grid_ctx(changed[i], now, i);
		});

	}

	inline std::vector<int> read_grid(const _stream_map& stream_data, orz_tag tag, kind k, const std::vector<u8>& changed) {

		std::vector<int> delta(changed.size());

		read_indices(stream_data[tag], k, read_table(stream_data), delta.size(),
			[&](size_t i, i64 now) {

				return grid_ctx(changed[i], now, i);
			},
			[&](size_t i, int d) {

				delta[i] = d;

			});

		return delta;
	}

}
