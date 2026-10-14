#pragma once

// AP paths are self-contained: inferred note endpoints, sampled cubic movement,
// sampled spinner motion, and slider polyline segments. Residuals preserve text.
namespace ap_path {

	enum class kind : u8 {

		hold,
		line,
		cubic,
		spinner,
		velocity

	};

	struct point {

		float x{}, y{};

	};

	struct knot {

		i64 time{};
		point p{};

	};

	struct segment {

		u32 begin{}, count{};
		kind type{};
		i64 t0{}, t1{};
		point a{}, b{};
		float angle{}, direction{ -1 };
		std::vector<knot> knots;

	};

	struct screen {

		int width{ 1920 }, height{ 1080 };
		float rate{ 1 };
		bool direct_divide{};

	};

	struct encoded {

		std::vector<u8> info, models, residuals;
		screen mapping;

	};

	inline void put(std::vector<u8>& b, u64 v) {

		while (v >= 128) {

			b.push_back((u8)v | 128);
			v >>= 7;

		}

		b.push_back((u8)v);

	}

	inline u64 get(const std::vector<u8>& b, size_t& at) {

		u64 v{};

		for (int s{}; s < 64; s += 7) {

			if (at >= b.size())

				throw std::runtime_error("AP short integer");

			const u8 c{ b[at++] };

			if (s == 63 && c > 1)

				throw std::runtime_error("AP integer overflow");

			v |= u64(c & 127) << s;

			if (!(c & 128))

				return v;

		}

		throw std::runtime_error("AP integer overflow");

	}

	inline void put_float(std::vector<u8>& b, const float f) {

		const u32 v{ std::bit_cast<u32>(f) };

		for (int i{}; i < 4; ++i)

			b.push_back((u8)(v >> (8 * i)));

	}

	inline float get_float(const std::vector<u8>& b, size_t& at) {

		if (at > b.size() || b.size() - at < 4)

			throw std::runtime_error("AP short float");

		u32 v{};

		for (int i{}; i < 4; ++i)

			v |= u32(b[at++]) << (8 * i);

		return float_from_bits(v);

	}

	inline void put_point(std::vector<u8>& b, const point p) {

		const bool integer{ std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x) < 16384 && std::abs(p.y) < 16384 &&
							p.x == std::round(p.x) && p.y == std::round(p.y) };

		b.push_back(integer);

		if (integer) {

			put(b, rc::zz((i64)p.x));
			put(b, rc::zz((i64)p.y));

		} else {

			put_float(b, p.x);
			put_float(b, p.y);

		}

	}

	inline point get_point(const std::vector<u8>& b, size_t& at) {

		if (at >= b.size())

			throw std::runtime_error("AP short point");

		const u8 integer{ b[at++] };

		if (integer > 1)

			throw std::runtime_error("AP invalid point encoding");

		if (integer)

			return { (float)rc::unzz(get(b, at)), (float)rc::unzz(get(b, at)) };

		return { get_float(b, at), get_float(b, at) };

	}

	inline point pos(const _osr_frame& f) {

		return { f.x, f.y };

	}

	inline double distance(const point a, const point b) {

		const double x{ (double)a.x - b.x }, y{ (double)a.y - b.y };

		return x * x + y * y;

	}

	inline point note(point p) {

		if (std::abs(p.x - std::round(p.x)) < 0.001f)

			p.x = std::round(p.x);

		if (std::abs(p.y - std::round(p.y)) < 0.001f)

			p.y = std::round(p.y);

		return p;

	}

	inline point screen_roundtrip(const point p, const screen& s) {

		const auto m{ dspace::playfield(s.width, s.height) };
		float x{ p.x * m.ratio + m.off_x }, y{ p.y * m.ratio + m.off_y };

		x -= m.off_x;
		y -= m.off_y;

		if (s.direct_divide)

			return { x / m.ratio, y / m.ratio };

		const float inverse{ 1.f / m.ratio };

		return { x * inverse, y * inverse };

	}

	inline float ease(const float a, const float b, const float elapsed, const float duration) {

		// Stable TweenValues(Vector2): float arguments and float subtraction, double easing.
		if (elapsed == 0 || duration == 0 || a == b)

			return a;

		const float change{ b - a };

		if (elapsed == duration)

			return (float)((double)a + change);

		double u{ (double)elapsed / ((double)duration / 2.0) };

		if (u < 1)

			return (float)((double)change / 2 * u * u * u + a);

		u -= 2;

		return (float)((double)change / 2 * (u * u * u + 2) + a);

	}

	inline double sine(double x) {

		// Fixed arithmetic avoids platform libm differences in the residual predictor.
		constexpr double pi{ 3.1415926535897932384626433832795 }, tau{ 2 * pi };

		x -= std::floor((x + pi) / tau) * tau;

		if (x > pi / 2)

			x = pi - x;

		else if (x < -pi / 2)

			x = -pi - x;

		const double q{ x * x };

		return x * (1 + q * (-1.0 / 6 +
							 q * (1.0 / 120 +
								  q * (-1.0 / 5040 +
									   q * (1.0 / 362880 + q * (-1.0 / 39916800 +
																q * (1.0 / 6227020800 + q * (-1.0 / 1307674368000 +
																							 q / 355687428096000.0))))))));

	}

	inline point spinner_point(const segment& s, const float time, const float rate) {

		const float elapsed{ time - (float)s.t0 };
		const float game_time{ (float)((double)elapsed / (double)rate) * s.direction };
		const float angle{ game_time / 20.f + s.angle };
		const float x{ (float)(sine((double)angle + 1.57079632679489661923) * 50) }, y{ (float)(sine(angle) * 50) };

		return { 256.f + x, 192.f + y };

	}

	inline void prepare(segment& s, const screen& map) {

		s.knots.clear();

		if (s.type != kind::cubic && s.type != kind::spinner)

			return;

		const float step{ (float)((1000.0 / 60.0) * (double)map.rate) };

		if (s.t1 <= s.t0 || s.t1 - s.t0 > 120000)

			throw std::runtime_error("AP invalid path duration");

		const point initial{ s.type == kind::cubic ? s.a : spinner_point(s, (float)s.t0, map.rate) };

		s.knots.push_back({ s.t0, initial });

		float last{ (float)s.t0 };

		for (float t{ (float)s.t0 + step }; t < (float)s.t1; t += step) {

			if (t <= last || s.knots.size() > 10000)

				throw std::runtime_error("AP invalid sampling");

			last = t;

			const point p{ s.type == kind::cubic ? point{ ease(s.a.x, s.b.x, t - (float)s.t0, (float)(s.t1 - s.t0)),
														  ease(s.a.y, s.b.y, t - (float)s.t0, (float)(s.t1 - s.t0)) }
												 : spinner_point(s, t, map.rate) };

			s.knots.push_back({ (i64)(int)t, p });

		}

		s.knots.push_back({ s.t1, s.type == kind::cubic ? s.b : spinner_point(s, (float)s.t1, map.rate) });

	}

	inline point interpolate(const point a, const point b, const i64 t, const i64 ta, const i64 tb) {

		if (ta == tb)

			return a;

		const float u{ std::max(0.f, 1.f - (float)(tb - t) / (float)(tb - ta)) };

		return { a.x + u * (b.x - a.x), a.y + u * (b.y - a.y) };

	}

	inline point predict(const segment& s, const i64 t, const screen& map) {

		point p{ s.a };

		if (s.type == kind::line)

			p = interpolate(s.a, s.b, t, s.t0, s.t1);

		else if (s.type == kind::cubic || s.type == kind::spinner) {

			const auto it{ std::upper_bound(s.knots.begin(), s.knots.end(), t, [](const i64 v, const knot& k) {

				return v < k.time;

			}) };

			if (it == s.knots.begin())

				p = it->p;

			else if (it == s.knots.end())

				p = s.knots.back().p;

			else

				p = interpolate((it - 1)->p, it->p, t, (it - 1)->time, it->time);

		}

		return screen_roundtrip(p, map);

	}

	inline double error(const segment& s, const _osr& r, const std::vector<i64>& times, const screen& map,
						const size_t samples = 24) {

		double total{};
		const size_t count{ std::min<size_t>(samples, s.count) };

		for (size_t j{}; j < count; ++j) {

			const size_t i{ s.begin + (count > 1 ? j * (s.count - 1) / (count - 1) : 0) };

			total += distance(pos(r[i]), predict(s, times[i], map));

		}

		return total / std::max<size_t>(count, 1);

	}

	inline bool fit_cubic(segment& s, const _osr& r, const std::vector<i64>& times, const screen& map) {

		const double dx{ (double)s.b.x - s.a.x }, dy{ (double)s.b.y - s.a.y }, length{ dx * dx + dy * dy };

		if (length < 4 || s.count < 4 || times[s.begin + s.count - 1] <= times[s.begin])

			return false;

		double su{}, st{}, suu{}, sut{};
		size_t count{};

		for (size_t j{}; j < std::min<size_t>(32, s.count); ++j) {

			const size_t i{ s.begin + j * (s.count - 1) / (std::min<size_t>(32, s.count) - 1) };
			const point p{ pos(r[i]) };
			const double q{ (((double)p.x - s.a.x) * dx + ((double)p.y - s.a.y) * dy) / length };
			const double perpendicular{ ((double)p.x - s.a.x) * dy - ((double)p.y - s.a.y) * dx };

			if (perpendicular * perpendicular / length > 0.1)

				return false;

			if (q < 0.005 || q > 0.995)

				continue;

			const double u{ q < 0.5 ? std::cbrt(q / 4) : 1 - std::cbrt((1 - q) / 4) };
			const double t{ (double)times[i] };

			su += u;
			st += t;
			suu += u * u;
			sut += u * t;
			++count;

		}

		if (count < 3 || count * suu - su * su <= 1e-10)

			return false;

		const double duration{ (count * sut - su * st) / (count * suu - su * su) }, start{ (st - duration * su) / count };

		if (duration < 15 || duration > 2500)

			return false;

		i64 t0{ (i64)std::llround(start) }, t1{ (i64)std::llround(start + duration) };
		segment best{ s };
		double best_error{ INFINITY };

		for (int da{ -12 }; da <= 12; da += 3)

			for (int db{ -12 }; db <= 12; db += 3) {

				segment q{ s };

				q.type = kind::cubic;
				q.t0 = t0 + da;
				q.t1 = t1 + db;

				if (q.t1 <= q.t0 || q.t1 - q.t0 > 3000)

					continue;

				prepare(q, map);

				const double e{ error(q, r, times, map, 12) };

				if (e < best_error) {

					best_error = e;
					best = std::move(q);

				}

			}

		t0 = best.t0;
		t1 = best.t1;

		for (int da{ -2 }; da <= 2; ++da)

			for (int db{ -2 }; db <= 2; ++db) {

				segment q{ s };

				q.type = kind::cubic;
				q.t0 = t0 + da;
				q.t1 = t1 + db;

				if (q.t1 <= q.t0)

					continue;

				prepare(q, map);

				const double e{ error(q, r, times, map, 24) };

				if (e < best_error) {

					best_error = e;
					best = std::move(q);

				}

			}

		if (best_error > 0.04)

			return false;

		s = std::move(best);

		return true;

	}

	inline bool circle_point(const point p) {

		const double x{ (double)p.x - 256 }, y{ (double)p.y - 192 }, r{ x * x + y * y };

		return r > 35 * 35 && r < 51 * 51;

	}

	inline bool fit_spinner(segment& s, const _osr& r, const std::vector<i64>& times, const screen& map) {

		if (s.count < 8 || times[s.begin + s.count - 1] - times[s.begin] < 100 ||
			times[s.begin + s.count - 1] - times[s.begin] > 119900)

			return false;

		for (size_t j{}; j < std::min<size_t>(20, s.count); ++j)

			if (!circle_point(pos(r[s.begin + j * (s.count - 1) / (std::min<size_t>(20, s.count) - 1)])))

				return false;

		segment best{ s };
		double best_error{ INFINITY };

		for (float direction : { -1.f, 1.f })

			for (int offset{}; offset <= 40; offset += 2) {

				segment q{ s };

				q.type = kind::spinner;
				q.direction = direction;
				q.t0 = times[s.begin] - offset;
				q.t1 = times[s.begin + s.count - 1] + (i64)std::ceil(20 * map.rate);

				double sx{}, sy{};

				for (size_t j{}; j < 16; ++j) {

					const size_t i{ s.begin + j * (s.count - 1) / 15 };
					const point p{ pos(r[i]) };
					const double phase{ std::atan2((double)p.y - 192, (double)p.x - 256) - direction * 0.05 * (times[i] - q.t0) / map.rate };

					sx += std::cos(phase);
					sy += std::sin(phase);

				}

				if (sx * sx + sy * sy < 64)

					continue;

				q.angle = (float)std::atan2(sy, sx);

				for (int iteration{}; iteration < 3; ++iteration) {

					prepare(q, map);

					double cross{}, dot{};

					for (size_t j{}; j < 16; ++j) {

						const size_t i{ s.begin + j * (s.count - 1) / 15 };
						const point p{ predict(q, times[i], map) }, a{ pos(r[i]) };
						const double x{ (double)p.x - 256 }, y{ (double)p.y - 192 }, ax{ (double)a.x - 256 }, ay{ (double)a.y - 192 };

						cross += x * ay - y * ax;
						dot += x * ax + y * ay;

					}

					q.angle += (float)std::atan2(cross, dot);

				}

				prepare(q, map);

				const double e{ error(q, r, times, map, 32) };

				if (e < best_error) {

					best_error = e;
					best = std::move(q);

				}

			}

		if (best_error > 0.5)

			return false;

		s = std::move(best);

		return true;

	}

	inline void polyline(std::vector<segment>& out, const _osr& r, const std::vector<i64>& times, const size_t begin, const size_t end,
						 const screen& map, const double tolerance, const unsigned depth = 0) {

		if (begin >= end)

			return;

		if (depth >= 48) {

			out.push_back({ (u32)begin, (u32)(end - begin), kind::velocity });

			return;

		}

		for (size_t i{ begin + 1 }; i < end; ++i)

			if (times[i] < times[i - 1]) {

				polyline(out, r, times, begin, i, map, tolerance, depth + 1);
				polyline(out, r, times, i, end, map, tolerance, depth + 1);

				return;

			}

		if (end - begin < 3 || times[end - 1] <= times[begin]) {

			out.push_back({ (u32)begin, (u32)(end - begin), kind::velocity });

			return;

		}

		segment s{ (u32)begin, (u32)(end - begin), kind::line, times[begin],
				   times[end - 1], note(pos(r[begin])), note(pos(r[end - 1])) };
		double worst{};
		size_t split{ begin + (end - begin) / 2 };

		for (size_t i{ begin + 1 }; i + 1 < end; ++i) {

			const double e{ distance(pos(r[i]), predict(s, times[i], map)) };

			if (e > worst) {

				worst = e;
				split = i;

			}

		}

		if (worst <= tolerance * tolerance) {

			out.push_back(std::move(s));

			return;

		}

		// Split into contiguous, nonoverlapping frame ranges.

		if (end - begin < 8) {

			out.push_back({ (u32)begin, (u32)(end - begin), kind::velocity });

			return;

		}

		split = std::clamp(split, begin + 2, end - 2);
		polyline(out, r, times, begin, split, map, tolerance, depth + 1);
		polyline(out, r, times, split, end, map, tolerance, depth + 1);

	}

	inline std::vector<segment> fit(const _osr& r, const std::vector<i64>& times, const screen& map, const double tolerance) {

		struct rest {
			size_t begin, end;
			point p;
		};
		std::vector<rest> holds;

		for (size_t i{}; i < r.size();) {

			size_t j{ i + 1 };

			while (j < r.size() && times[j] >= times[j - 1] && distance(pos(r[j]), pos(r[i])) < 1e-6)

				++j;

			const bool integer_target{ distance(pos(r[i]), note(pos(r[i]))) < 1e-10 &&
									   std::abs(r[i].x - std::round(r[i].x)) < 0.001f &&
									   std::abs(r[i].y - std::round(r[i].y)) < 0.001f };

			if ((j - i >= 2 && times[j - 1] - times[i] >= 2) || integer_target)

				holds.push_back({ i, j, note(pos(r[i])) });

			i = j;

		}

		std::vector<segment> out;
		size_t at{};

		for (size_t h{}; h < holds.size(); ++h) {

			const auto hold{ holds[h] };

			if (at < hold.begin) {

				bool modeled{ false };

				if (h) {

					const auto previous{ holds[h - 1] };
					segment s{ (u32)at,
							   (u32)(hold.begin - at),
							   kind::cubic,
							   times[previous.end - 1],
							   times[hold.begin],
							   previous.p,
							   hold.p };

					if (fit_cubic(s, r, times, map)) {

						out.push_back(std::move(s));
						modeled = true;

					}

				}

				if (!modeled) {

					size_t cursor{ at };

					for (size_t i{ at }; i < hold.begin;) {

						if (!circle_point(pos(r[i]))) {

							++i;
							continue;

						}

						size_t end{ i + 1 };

						while (end < hold.begin && times[end] >= times[end - 1] && circle_point(pos(r[end])))

							++end;

						segment s{ (u32)i, (u32)(end - i) };

						if (fit_spinner(s, r, times, map)) {

							polyline(out, r, times, cursor, i, map, tolerance);
							out.push_back(std::move(s));
							cursor = end;

						}

						i = end;

					}

					polyline(out, r, times, cursor, hold.begin, map, tolerance);

				}

			}

			out.push_back({ (u32)hold.begin, (u32)(hold.end - hold.begin), kind::hold, times[hold.begin],
							times[hold.end - 1], hold.p, hold.p });
			at = hold.end;

		}

		polyline(out, r, times, at, r.size(), map, tolerance);

		std::vector<segment> merged;

		for (auto& s : out) {

			if (!merged.empty() && s.type == kind::velocity && merged.back().type == kind::velocity)

				merged.back().count += s.count;

			else

				merged.push_back(std::move(s));

		}

		return merged;

	}

	inline std::vector<u8> write_models(const std::vector<segment>& segments) {

		std::vector<u8> b;

		put(b, segments.size());

		i64 last{};

		for (auto& s : segments) {

			b.push_back((u8)s.type);
			put(b, s.count);

			if (s.type == kind::velocity)

				continue;

			put(b, rc::zz(s.t0 - last));
			last = s.t0;
			put(b, (u64)std::max<i64>(0, s.t1 - s.t0));

			if (s.type == kind::spinner) {

				put_float(b, s.angle);
				b.push_back(s.direction > 0);

			} else {

				put_point(b, s.a);

				if (s.type != kind::hold)

					put_point(b, s.b);

			}

		}

		return b;

	}

	inline std::vector<segment> read_models(const std::vector<u8>& b, const size_t frames, const screen& map) {

		size_t at{}, count{ (size_t)get(b, at) };

		if (count > frames)

			throw std::runtime_error("AP too many segments");

		std::vector<segment> out;

		out.reserve(count);

		size_t knots{};
		u32 begin{};
		i64 last{};

		for (size_t i{}; i < count; ++i) {

			if (at >= b.size())

				throw std::runtime_error("AP short model");

			segment s{};

			s.type = (kind)b[at++];

			const u64 n{ get(b, at) };

			if (n == 0 || n > frames - begin || s.type > kind::velocity)

				throw std::runtime_error("AP invalid model");

			s.begin = begin;
			s.count = (u32)n;
			begin += s.count;

			if (s.type != kind::velocity) {

				const i64 delta{ rc::unzz(get(b, at)) };

				if (delta < -(1ll << 40) || delta > (1ll << 40))

					throw std::runtime_error("AP time out of range");

				s.t0 = last + delta;

				if (s.t0 < -(1ll << 40) || s.t0 > (1ll << 40))

					throw std::runtime_error("AP time out of range");

				last = s.t0;

				const u64 duration{ get(b, at) };

				if (duration > 86400000)

					throw std::runtime_error("AP model too long");

				s.t1 = s.t0 + (i64)duration;

				if (s.type == kind::spinner) {

					s.angle = get_float(b, at);

					if (!std::isfinite(s.angle) || std::abs(s.angle) > 1000000)

						throw std::runtime_error("AP invalid angle");

					if (at >= b.size())

						throw std::runtime_error("AP short spinner");

					s.direction = b[at++] ? 1.f : -1.f;

				} else {

					s.a = get_point(b, at);
					s.b = s.type == kind::hold ? s.a : get_point(b, at);

					if (!std::isfinite(s.a.x) || !std::isfinite(s.a.y) || !std::isfinite(s.b.x) || !std::isfinite(s.b.y) ||
						std::abs(s.a.x) > 131072 || std::abs(s.a.y) > 131072 || std::abs(s.b.x) > 131072 ||
						std::abs(s.b.y) > 131072)

						throw std::runtime_error("AP invalid point");

				}

				if (s.type == kind::cubic || s.type == kind::spinner) {

					knots += (size_t)((s.t1 - s.t0) / (1000.0 / 60 * map.rate)) + 3;

					if (knots > std::min<size_t>(ORZ_MAX_STREAM / sizeof(knot), frames * 32 + 4096))

						throw std::runtime_error("AP excessive sampling");

				}

				prepare(s, map);

			}

			out.push_back(std::move(s));

		}

		if (begin != frames || at != b.size())

			throw std::runtime_error("AP model coverage");

		return out;

	}

	inline screen infer_screen(const _osr& r, const std::vector<segment>& models, const std::vector<i64>& times,
							   const bool shortest, const screen initial) {

		constexpr int resolutions[][2]{
			{ 1920, 1080 }, { 2560, 1440 }, { 1366, 768 }, { 1360, 768 },
			{ 1600, 900 }, { 1280, 720 }, { 1680, 1050 }, { 1920, 1200 },
			{ 1440, 900 }, { 1280, 800 }, { 1280, 1024 }, { 1024, 768 },
			{ 800, 600 }, { 3840, 2160 }, { 2560, 1080 }, { 3440, 1440 },
			{ 1536, 864 }, { 1280, 960 }, { 1400, 1050 }, { 1600, 1200 },
			{ 2560, 1600 }, { 1152, 864 }, { 1600, 1024 }, { 1920, 1440 }
		};
		screen best{ initial };
		double score{ INFINITY };

		for (auto wh : resolutions)

			for (bool direct : { false, true }) {

				const screen q{ wh[0], wh[1], initial.rate, direct };
				double cost{};
				size_t n{};

				for (auto& s : models)

					if ((s.type == kind::hold || s.type == kind::cubic) && n < 512) {

						for (size_t j{}; j < std::min<u32>(s.count, 4) && n < 512; ++j) {

							const size_t i{ s.begin + j * (s.count - 1) / std::max<u32>(1, std::min<u32>(s.count, 4) - 1) };
							const point p{ predict(s, times[i], q) }, actual{ pos(r[i]) };

							for (int a{}; a < 2; ++a) {

								const float value{ a ? actual.y : actual.x }, pred{ a ? p.y : p.x };

								if (std::abs(value - pred) > 0.0002f)

									continue;

								i64 v{}, w{};

								dotnet::text_ord(pred, shortest, v);
								dotnet::text_ord(value, shortest, w);
								cost += std::log2(1. + std::abs((double)v - w));

							}

							++n;

						}

					}

				if (cost < score) {

					score = cost;
					best = q;

				}

			}

		return best;

	}

	inline point velocity_prediction(const _osr& r, const size_t i) {

		if (!i)

			return {};

		const point p{ pos(r[i - 1]) };

		if (i < 2 || r[i].delta <= 0 || r[i - 1].delta <= 0)

			return p;

		const double rate{ (double)r[i].delta / r[i - 1].delta };
		const point q{ (float)((double)p.x + ((double)p.x - r[i - 2].x) * rate),
					   (float)((double)p.y + ((double)p.y - r[i - 2].y) * rate) };

		return std::isfinite(q.x) && std::isfinite(q.y) ? q : p;

	}

	inline encoded encode_positions(_osr& r, const float rate, const bool shortest, const double tolerance = 0.2) {

		encoded e{};

		e.mapping.rate = rate;

		for (auto& f : r)

			for (int a{}; a < 2; ++a) {

				i64 ord{};

				if (!dotnet::text_ord((&f.x)[a], shortest, ord))

					return {};

				(&f.x)[a] = dotnet::from_text_ord(ord, shortest);

			}

		std::vector<i64> times(r.size());
		i64 now{};

		for (size_t i{}; i < r.size(); ++i) {

			times[i] = now += r[i].delta;

			if (now < -(1ll << 24) || now > (1ll << 24))

				return {};

		}

		auto segments{ fit(r, times, e.mapping, tolerance) };

		e.mapping = infer_screen(r, segments, times, shortest, e.mapping);

		for (auto& s : segments)

			prepare(s, e.mapping);

		std::vector<segment> chosen;

		for (auto& s : segments) {

			if (s.type != kind::velocity) {

				double modeled{}, fallback{};

				for (size_t i{ s.begin }; i < s.begin + s.count; ++i) {

					const point p{ predict(s, times[i], e.mapping) }, q{ velocity_prediction(r, i) };

					for (int a{}; a < 2; ++a) {

						i64 actual{}, pr{}, prev{};

						dotnet::text_ord(a ? r[i].y : r[i].x, shortest, actual);
						dotnet::text_ord(a ? p.y : p.x, shortest, pr);
						dotnet::text_ord(a ? q.y : q.x, shortest, prev);
						modeled += 1 + std::log2(1. + std::abs((double)actual - pr));
						fallback += 1 + std::log2(1. + std::abs((double)actual - prev));

					}

				}

				const double overhead{ s.type == kind::hold ? 32.0 : s.type == kind::line ? 72.0 : 96.0 };

				if (modeled + overhead >= fallback) {

					s.type = kind::velocity;
					s.knots.clear();

				}

			}

			if (!chosen.empty() && s.type == kind::velocity && chosen.back().type == kind::velocity)

				chosen.back().count += s.count;

			else

				chosen.push_back(std::move(s));

		}

		segments = std::move(chosen);
		e.info = { 1, 0, (u8)e.mapping.direct_divide };
		put(e.info, e.mapping.width);
		put(e.info, e.mapping.height);
		put_float(e.info, rate);
		e.models = write_models(segments);

		rc::rc_encoder coder;
		rc::value_coder residual{ 16, 32 };
		int miss[2]{};

		for (auto& s : segments) {

			for (size_t i{ s.begin }; i < s.begin + s.count; ++i) {

				const point p{ s.type == kind::velocity ? velocity_prediction(r, i) : predict(s, times[i], e.mapping) };

				for (int a{}; a < 2; ++a) {

					i64 actual{}, pr{};

					dotnet::text_ord(a ? r[i].y : r[i].x, shortest, actual);
					dotnet::text_ord(a ? p.y : p.x, shortest, pr);

					const i64 diff{ actual - pr };
					const u64 ctx{ ((u64)s.type * 2 + a) * 4 + miss[a] };

					residual.encode(coder, ctx, rc::zz(diff));
					miss[a] = diff == 0 ? 0 : std::abs(diff) < 4 ? 1 : std::abs(diff) < 32 ? 2 : 3;

				}

			}

		}

		e.residuals = coder.finish();

		return e;

	}

	inline bool decode_positions(const std::vector<u8>& info, const std::vector<u8>& models,
								 const std::vector<u8>& residual_bytes, const u32 flags, _osr& r) {

		try {

			if (info.size() < 3 || info[0] != 1 || info[1] != 0 || info[2] > 1 || residual_bytes.empty())

				return false;

			size_t at{ 3 };
			screen map{};

			map.direct_divide = info[2];
			map.width = (int)get(info, at);
			map.height = (int)get(info, at);
			map.rate = get_float(info, at);

			if (at != info.size() || map.width < 128 || map.width > 32768 || map.height < 128 || map.height > 32768 ||
				!(map.rate >= 0.5f && map.rate <= 2.f))

				return false;

			auto segments{ read_models(models, r.size(), map) };
			std::vector<i64> times(r.size());
			i64 now{};

			for (size_t i{}; i < r.size(); ++i)

				times[i] = now += r[i].delta;

			rc::rc_decoder coder{ residual_bytes };
			rc::value_coder residual{ 16, 32 };
			int miss[2]{};
			const bool shortest{ (flags & ORZ_FLAG::text_shortest) != 0 };

			for (auto& s : segments)

				for (size_t i{ s.begin }; i < s.begin + s.count; ++i) {

					const point p{ s.type == kind::velocity ? velocity_prediction(r, i) : predict(s, times[i], map) };

					for (int a{}; a < 2; ++a) {

						i64 pr{};

						dotnet::text_ord(a ? p.y : p.x, shortest, pr);

						const u64 ctx{ ((u64)s.type * 2 + a) * 4 + miss[a] };
						const u64 coded{ residual.decode(coder, ctx) };

						if (coded > (1ull << 35))

							return false;

						const i64 diff{ rc::unzz(coded) };

						miss[a] = diff == 0 ? 0 : std::abs(diff) < 4 ? 1 : std::abs(diff) < 32 ? 2 : 3;
						(&r[i].x)[a] = dotnet::from_text_ord(pr + diff, shortest);

					}

				}

			return true;

		} catch (const std::exception&) {

			return false;

		}

	}

} // namespace ap_path
