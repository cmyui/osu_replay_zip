# ORZ format outline

An overview of the `.orz` file format: what a file holds, how it is laid out, and what a decoder must do to get the
replay back. It describes the format rather than this repository's code. Where the format depends on data that is
too large to restate (model tables, symbol orders, context definitions), the outline names that data and where it
is defined.

The format is version 0 and still changing. Every format change so far has been breaking, and the version number
has not yet been raised.

## 1. Scope and guarantees

An `.orz` file holds one osu! replay, given either as a full `.osr` file or as bare replay data (the LZMA stream of
replay text, as servers store it).

Decoding gives back:

- **the replay text exactly** (the LZMA-decompressed `delta|x|y|keys,` list);
- **the `.osr` bytes around the replay data exactly**: the header, the life bar text, the timestamp and the bytes
  after the replay data (exceptions in §13);
- **the LZMA bytes of the replay data recompressed**, so generally not the original bytes. Files that store the
  replay data verbatim (§10) are the exception.

Frames are reproduced to the text they print as, not always bit for bit. With classic 7-digit text (§4), a float
only has to print the same 7 significant digits.

## 2. Conventions

- **Bit packing.** Bit fields are written least significant bit first, and bits fill each byte starting from its low
  bit. Reading past the end of a bit field gives 0.
- **`bucket(b1, b2, …)`** is a variable-length unsigned code. For each width `b` in turn:
  - if the value is below `2^b − 1`, write it in `b` bits and stop;
  - otherwise write `b` one bits, subtract `2^b − 1` and continue.

  If every width escapes, let `v = value + 1` and `e = floor(log2 v)`; write `e` in 5 bits, then the `e` bits of
  `v` below its leading 1. `bucket()` with no widths is that final form alone.
- **`small<N>(v)`**: `N` bits if `v < 2^N − 1`. Otherwise `N` one bits, then `floor(log2 v) − (N − 1)` further
  one bits, a zero, and the `floor(log2 v)` bits of `v` below its leading 1.
- **zigzag**: `zz(v) = 2v` for `v ≥ 0`, `−2v − 1` for `v < 0`.
- **ULEB128**: the usual 7-bits-per-byte length used by `.osr` strings.
- **Coded values** inside the arithmetic-coded body (§5) are binary decisions. "Raw" bits there are coded at
  probability 1/2. `gamma(v)` there is Elias gamma: `floor(log2 v)` as unary decisions, then the bits below the
  leading 1, most significant first.

## 3. Container

```
byte 0     magic 'r' (0x72)
byte 1..   header, a bit field padded to a byte boundary
then       stream payloads, concatenated in stream-table order
```

### 3.1 Header (in order)

| Field | Coding | Notes |
|---|---|---|
| version | `bucket(4, 8)` | 0. A reader rejects a larger version as "made by a newer version". |
| flags | `bucket(12)` | §3.3 |
| crc | 32 bits | §3.4 |
| seeded | 1 bit | the replay ended with a closing seed frame (§3.5) |
| seed | `length_bucket<3,18>`, if seeded | see below |
| frame count | `bucket(16)` | frames held by the frames stream, without the seed frame; at most 2^22 |
| stream count | `bucket(3)` | at most 256 |
| per stream: tag, size | tag `bucket(3)`, size `bucket()` | sizes in bytes |

`length_bucket<3,18>`:

- a 3-bit code `min(bitlen − 18, 7)`, where `bitlen` is the value's bit length;
- code 7 escapes: 5 bits of `bitlen < 18 ? bitlen : bitlen − 7` follow;
- then the `bitlen − 1` bits below the leading 1.

A reader finds streams by tag; for a repeated tag the first one counts. Each stream must fit in the file. Bytes after
the last stream are ignored.

### 3.2 Streams

| Tag | Stream | Present when |
|---|---|---|
| 0 | `frames`: the frames (§6, §7) | the replay is not stored verbatim |
| 1 | `osr_parts`: the `.osr` bytes around the replay data (§8) | the input was an `.osr` |
| 2 | `life_bar`: the life bar text (§9) | the `.osr` had a life bar |
| 3 | `osr_replay_data`: the original LZMA replay data (§10) | the replay is stored verbatim |

### 3.3 Flags

| Bit | Name | Meaning |
|---|---|---|
| 0–3 | game mode | `1 << mode` from the `.osr` (osu!, osu!taiko, osu!catch, osu!mania). Not needed to decode. |
| 4 | `has_osr_header` | the input was a full `.osr`, so the decoder rebuilds one |
| 5 | `trailing_comma` | the replay text ended with `,` |
| 6 | `replay_verbatim` | the replay data is stored as-is (§10) |
| 7 | `text_shortest` | floats print as shortest round-trip text, as in osu!lazer; otherwise as 7 significant digits, as in osu!stable (§4) |
| 8 | `mania_columns` | frames stream in the osu!mania form (§7) |
| 9 | `taiko_keys` | frames stream in the osu!taiko form (§7) |
| 10 | `fruits_catcher` | frames stream in the osu!catch form (§7) |
| 11 | `relax` | a relax replay; selects the relax prior sets (§5.6) |

None of bits 8–10 means the osu!standard form (§6). That form also carries relax osu!catch (where the cursor drives the
catcher) and osu!mania replays that do not fit the column form.

### 3.4 CRC

CRC-32C (Castagnoli polynomial), starting from 0 with no final XOR, over:

1. **Every decoded frame** as 16 little-endian bytes: `i32 delta`, `f32 x`, `f32 y`, `u32 keys`. This includes the
   closing seed frame `{−12345, +0, +0, seed}` when there is one. These are the frames as the decoder rebuilds them
   (§1), not the parsed input.
2. **For tags 1, 2 and 3 in that order**, when the stream is present and not empty: its `u32` size, then its bytes.
3. **The flags**, as a `u32`.

The CRC does not cover the magic, the header layout or the frames stream's own bytes, only what they decode to.

### 3.5 Seed frame

A replay text that ends with the frame `-12345|0|0|<seed>` stores `<seed>` in the header instead of as a frame. The
decoder appends `{−12345, +0, +0, seed}` after the decoded frames.

## 4. Replay text

The replay text is a list of frames `delta|x|y|keys` separated by `,`, with an optional trailing `,`. Each frame is
an `int` delta, `float` x and y, and `u32` keys. A file stores frames and rebuilds the text by printing them, so the
format requires the text to be exactly what printing would give.

Frames are joined by `,` with no trailing `,` (bit 5 adds one). `delta` and `keys` print as plain integers, and x and
y print in one of two float styles:

- **7 significant digits** (bit 7 clear), as .NET Framework's default formatting:
  - the digits are the float's exact binary value rounded half away from zero, with trailing zeros removed;
  - both 0 and −0 print as `0`.
- **Shortest round-trip** (bit 7 set): the shortest digits that parse back to the same float; −0 prints as `-0`.

Both styles share one layout, where `e` is the decimal exponent of the leading digit:

- `e ≤ −5` or `e ≥ 7`: `d.ddddE±XX`, with at least two exponent digits;
- `e < 0`: `0.`, then `−e − 1` zeros, then the digits;
- otherwise: the digits with a decimal point after `e + 1` of them, zero-padded, and no point when there are no
  fractional digits.

`NaN` and `±Infinity` print as `NaN`, `Infinity` and `-Infinity`.

An encoder must store the replay verbatim (§10) instead of as frames when any of these holds:

- the replay data does not decompress to its declared size;
- neither float style reprints the text exactly;
- there are more than 2^22 frames;
- some `|x|` or `|y|` exceeds 2^17 (x may reach 2^24 in osu!mania), or a value is NaN.

## 5. The binary coder

Every frames stream is a short bit-field head, then one arithmetic-coded body. The body is a sequence of binary
decisions, each predicted by context mixing. Encoder and decoder must compute identical predictions, so all model
arithmetic is integer-only and normative.

### 5.1 Arithmetic coder

The state is two `u32` values, `x1 = 0` and `x2 = 0xFFFFFFFF`. To code a bit `y` with probability `p` (`P(y=1)`
scaled to 16 bits):

- `xmid = x1 + ((x2 − x1) · p >> 16)`, in 64-bit arithmetic;
- `y = 1` keeps the low part (`x2 = xmid`), `y = 0` the high part (`x1 = xmid + 1`);
- while the top bytes of `x1` and `x2` are equal, output that byte and shift: `x1 <<= 8`, `x2 = (x2 << 8) | 255`.

The encoder ends by writing the 4 bytes of `x1`, most significant first. The decoder reads 4 bytes into `x` and
decodes `y = (x ≤ xmid)`. It shifts input in the same way, reading 0 for any byte past the end of the stream.

### 5.2 Counters

A counter holds a 16-bit probability `p` (starting at 32768) and a count `n` (starting at 0, at most 255). After a bit
`y`:

- `p += ((y ? 65535 : 0) − p) · RATE[n] >> 16`, where `RATE[n] = 131072 / (2n + 3)`;
- then `n` increases by 1 if it is below 255.

### 5.3 Models

Each kind of decision has a model:

- **a table of counters in lines**: `2^bits` lines of `width` counters each, where a line is found by
  `hash & (2^bits − 1)`, with no collision check;
- **a set of mixer weights** per selector, each starting at 19661 (0.3 in 16.16 fixed point);
- **an APM table** of 33 entries per context.

Binding a decision's `N` contexts (`N` ≤ 8) gives one line per input: input `i`'s line is
`hash(salt · 8 + i, context_i)`. The decision then uses the counter at its slot in each bound line. `hash(a, b)` works
on `u64`:

```
h = (a + 1) · 0x9E3779B97F4A7C15 ^ b · 0xD6E8FEB86659FD93
h ^= h >> 29;  h *= 0xBF58476D1CE4E5B9;  h ^= h >> 32
```

### 5.4 Mixing and the coded probability

`squash` maps a stretched value `d` to a 12-bit probability:

- `squash(d) = 4095` for `d > 2047`, and `1` for `d < −2047`;
- otherwise it interpolates between table entries:
  `T[(d >> 7) + 16] · (128 − (d & 127)) + T[(d >> 7) + 17] · (d & 127) + 64 >> 7`.

The table is:

```
T = {1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047, 2549, 2994, 3348, 3607, 3785,
     3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094}
```

`stretch(p)` is the smallest `d` in −2047…2047 with `squash(d) ≥ p` (2047 if none).

For one decision with weight set `w` and APM context `a`:

1. `x_i = stretch(counter_i.p >> 4)`, and `dot = Σ x_i · w_i` (64-bit);
2. mixer output: `pm = squash(clamp(dot >> 16, −2047, 2047))`;
3. APM output: let `s = stretch(pm) + 2048`, `lo = a · 33 + (s >> 7)` and `t = s & 127`, then
   `pa = APM[lo] · (128 − t) + APM[lo + 1] · t >> 7`;
4. coded probability: `p = clamp((pm · 16 + 3 · pa) >> 2, 32, 65504)`.

After coding `y`, in this order:

1. for each input: update its counter (§5.2), then `w_i += (x_i · ((y << 12) − pm) · 40 + 0x8000) >> 16`;
2. move both APM entries toward `y ? 65535 : 0`: `APM[lo] += ((target − APM[lo]) · (128 − t) >> 7) · 5 >> 8` and
   `APM[lo + 1] += ((target − APM[lo + 1]) · t >> 7) · 5 >> 8`.

APM entries start at `squash((j − 16) · 128) · 16` for `j` = 0…32.

### 5.5 Table sizes

A model's table size depends on the header's frame count (`b = bit_width(frame count)`), so a decoder must size every
table exactly as the encoder did. The sizes are part of the format:

| Model | Inputs | Table bits | Width | Mixer sets | APM contexts |
|---|---|---|---|---|---|
| keys changed | 7 | clamp(b+4, 16, 22) | 1 | 64 | 64 |
| key bits | 6 | clamp(b+2, 14, 20) | 1 | 64 | 64 |
| high key bits | 1 | 8 | 1 | 1 | 1 |
| time | 7 | clamp(b, 12, 16) | 64 | 576 | 192 |
| repeat | 8 | clamp(b+4, 16, 22) | 1 | 64 | 64 |
| move | 6 | clamp(b+3, 12, 19) | 32 | 384 | 1536 |
| move bits | 2 | clamp(b+2, 12, 20) | 1 | 32 | 256 |
| slack | 1 | clamp(b−2, 10, 16) | 16 | 64 | 64 |
| escape | 5 | clamp(b+2, 12, 20) | 1 | 48 | 48 |
| escape value | 2 | 10 | 64 | 1 | 1 |
| autopilot event | 5 | clamp(b, 10, 16) | 1 | 8 | 8 |
| autopilot value | 2 | 12 | 64 | 1 | 1 |
| catcher moved | 6 | clamp(b+3, 12, 20) | 1 | 64 | 64 |
| catcher direction | 4 | clamp(b+1, 10, 18) | 1 | 16 | 16 |
| catcher residual | 5 | clamp(b, 10, 18) | 64 | 64 | 64 |
| catcher residual bits | 2 | clamp(b+1, 10, 18) | 1 | 8 | 8 |

The first four serve every mode. The next eight are osu!standard only, and the last four osu!catch only.

### 5.6 Priors

Before coding, the models start from a built-in prior: a starting state trained on real replays. The prior is chosen
by the kind of replay:

| Set | Replays |
|---|---|
| 0, 1, 2 | osu!standard whose device model (§6.3) is rational, mouse or absolute |
| 3, 4, 5 | the same for relax replays (flag 11) |
| 6 | relax without a device model |
| 7 | osu!standard without a device model (mostly autopilot) |
| 8, 9, 10 | osu!taiko, osu!catch, osu!mania |

The device is the higher `axis mode` value (§6.3) of the two axes' first segments. A text axis therefore counts as "no
device model".

A prior holds three things per model:

- **counters**, each given by the low 32 bits of its context hash, a slot, a probability and a confidence (the
  starting `n`);
- **the mixer weights**;
- **the APM table**.

Counters are written into the line their key selects under the replay's table size, and a later entry replaces an
earlier one that lands on the same counter. Weights and APM tables are copied whole.

The prior tables are normative data. Encoder and decoder need bit-identical copies, and changing them is a format
change.

## 6. The frames stream: osu!standard form

### 6.1 Head (bit field, in order)

1. **Delta table.**
   - `small<5>` entry count `n`, then the `n` distinct frame deltas as symbols:
     - symbol `v` for 0 ≤ v < 32;
     - otherwise symbol `32 + 2 · (bit_width(|v|) − 1) + (v < 0)`, followed by the bits of `|v|` below its leading 1,
       most significant first.
   - Each symbol is written as its rank in a fixed ordering row, coded as Elias gamma of `rank + 1`. Symbols below 32
     already in the table are skipped when ranking.
   - The first entry uses row 33, and each later entry uses row `min(previous symbol, 32)`. The 34 rows, each a
     permutation of the 96 symbols, are normative data.
2. **Time grid**: 3 bits choosing a period from `{50/3, 25, 25/2, 100/3, 20, none}` ms. It is only used as context.
3. **Device model** for x, then for y: §6.3.
4. **Autopilot**: 1 bit. If set, the game speed (`small<2>`: 1.0, 1.5 or 0.75) and the screen width and height (§6.6).
5. **Native grid** for x, then y: 1 bit. If set, a 48-bit step and a 64-bit offset in 32.32 fixed point, with
   `value(j) = (step · j + offset) >> 32`. It maps the coded integers of a single rational or absolute segment onto
   the device's own lattice.
6. **start**: 1 bit; if set, **start_time**: 1 bit.
   - `start`: frames 0 and 1 are at (256, −500), the position every replay starts at.
   - `start_time`: their deltas are also 0 and −1, and are not coded.

The coded body starts at the next byte.

### 6.2 Coded body

Two raw bits come first: whether axis x has escapes, then whether axis y does. Then, for each frame:

1. **Keys**, folded so that K1 and K2 imply M1 and M2 (`k ^ ((k >> 2) & 3)`):
   - a "keys changed" decision;
   - if they changed: one flip decision for each of the 5 low key bits, then any flips above them;
   - the bits above are 27 raw bits, preceded by a "high bits flipped" decision unless no low bit flipped.
2. **Delta**: the index into the delta table.
   - Unary decisions "is it entry `at`?" for `at` = 0, 1, …, up to 64 entries; the last listed entry is implied.
   - An index of 64 or more continues as `gamma(index − 63)`.
   - Nothing is coded for frames 0 and 1 when `start_time` is set.
3. **Repeat** (frames after the first): whether the position is bit-identical to the previous frame's.
4. **For a new position** (not a repeat):
   - an autopilot event, if autopilot is on (§6.6);
   - then **each axis**, in the order the move predictor expects to move more first:
     1. the move residual: the coded integer's step minus the predicted step;
     2. an escape decision, if the axis has escapes;
     3. for a mouse axis that moved and was not escaped, the slack index (§6.3).

After all frames come the escape values: every escaped position on axis x in order, then axis y (§6.4).

The contexts of each decision are part of the format but are not restated here. They are the key rhythm, time history,
60 Hz tick phase, move history and so on, defined in the reference implementation (`orz_frames.h`).

### 6.3 Device models

Each axis carries a list of segments (a `bucket(4, 8, 16)` count). Each segment after the first starts with its
distance from the previous one, counted in new positions. A segment carries one axis model. When both axes have one
segment, y may instead be marked "same as x".

The models turn the coded integer `K` of each new position into a float:

| Mode | Value |
|---|---|
| rational | `(a · K + b) / D`, in double, rounded to float |
| mouse | window pixels move by `counts · sensitivity` per input report, then map to the playfield |
| absolute | a tablet coordinate `L = K` in 0…65535 maps to the window, then to the playfield |
| text | `K` is the float's place in the order of printable values (§4) |

**rational**: integers `D`, `a`, `b`.

**mouse**: a sensitivity, a polling rate (125–8000 Hz) and the window.

- The step `ΔK` is split into `round(dt · poll / 1000)` reports, at least 1 and at most `|ΔK|`.
- The pixel position is advanced in double and rounded to float after each report, and clamped to the window.
- Report splits and float rounding can leave several candidate floats. A slack index chooses one: first the
  neighbouring report counts, then values a few ulps away.
- The first position of each mouse segment is escaped, so its exact value seeds the position.

**absolute**: the tablet mapping (to the window, or to the desktop with an origin and a size), an area speed, and
whether the game computed in SSE single precision or x87 extended precision.

**The window** is either one of 24 common sizes, by index, or an explicit scale and offset.

**Window to playfield**: `field = (pixel − offset) / scale`, computed the way osu!stable does. The rounding of each
step, and an "unrounded subtraction" variant, are part of the model.

**text**:

- with 7-digit text, `K = ±((e + 50) · 9 000 000 + (m − 1 000 000) + 1)` for 7 significant digits `m` and exponent
  `e`, with 0 at `K = 0`;
- with shortest text, `K` is ± the float's bit pattern.

Rational, absolute and mouse axes **hold** after an escape: until the next move, frames keep the escaped value.

### 6.4 Escape values

An escape value is the true position where the model's value is wrong. It is coded against the model's prediction:

- **near**: the distance in 7-digit mantissas (or in ulps, with shortest text), when small;
- **otherwise**: 32 raw bits.

On an axis with a native grid, a value may instead be one of the two lattice values next to the grid value.

### 6.5 Rebuilding the frames

1. Each new position takes the next decoded value of each axis; repeated frames take the previous frame's position.
2. Deltas come from the delta table and keys are unfolded.
3. With `start`, frames 0 and 1 are set to (256, −500).

### 6.6 Autopilot

Autopilot replays move the cursor along a path the game generates, so their positions are predicted from that path:

- points at notes;
- holds and straight moves;
- in-out cubic eased moves, sampled every `1000/60 · speed` ms;
- slider and spinner points.

**A path event** is coded when the path ends before a new position. It is the move's type, wait and duration, its
start (an offset from the last path point) and its target (an integer or an offset from a guess).

**A frame on the path** is predicted as the linear interpolation between the path points around its time. That value is
mapped field → screen → field with the stored screen size and printed as 7-digit text. Residuals are still coded.

## 7. The frames stream: osu!taiko, osu!catch and osu!mania form

**Head**: the delta table and time grid as in §6.1. The coded body starts at the next byte.

**Coded body**, in order:

1. **Escaped frames**: `gamma(count + 1) − 1`. Then, per escaped frame:
   - the gap to it, as `gamma(gap + 1) − 1`;
   - a bit for "at (256, −500)", or else its x and y as 32 raw bits each.
2. **osu!mania only**:
   - the column count, `gamma(columns)`, at most 18;
   - the scroll-speed changes: a count, then per change its frame (as a gap) and the speed as 32 raw bits.
3. **Per frame**:
   - keys, with the key coder of §6.2;
   - the delta;
   - for osu!catch, the catcher position.

| Mode | Coded keys | x | y | Escaped when |
|---|---|---|---|---|
| osu!taiko | the keys | 0, 320 or 640, from the keys | 9999 | x or y differs from that |
| osu!catch | the keys | catcher position, coded as a text value | 9999 | y ≠ 9999, or x negative or not finite |
| osu!mania | held columns (keys are always 0) | the columns as an integer | current scroll speed | the frame is at (256, −500) |

**The catcher** is coded as three decisions:

1. whether it moved;
2. whether in the same direction as last time;
3. the residual against a prediction of walking or dashing for the frame's delta, scaled by any hyperdash.

## 8. The `osr_parts` stream

It holds the `.osr` bytes outside the replay data and life bar:

- **the header**: game mode through mods;
- **the timestamp**: 8 bytes;
- **the tail**: what follows the replay data, usually the online score id.

Its first byte picks the form:

- **0, raw**: each of the three parts as a ULEB128 length and its bytes.
- **1, fields**: a bit field of:
  - mode;
  - client version, relative to 20200000;
  - player name;
  - beatmap MD5 as 16 bytes;
  - replay MD5, either as an index into 49 known formulas over the score fields or as 16 bytes;
  - hit counts, score, combo, perfect and mods;
  - the timestamp as Unix seconds, plus sub-second ticks only when present;
  - the online score id by kind (none, zero, 32-bit, 64-bit);
  - the rest of the tail.

An encoder only uses the fields form when decoding it gives back all three parts exactly. Strings are rebuilt with the
`0x0b` marker and a minimal ULEB128 length.

## 9. The `life_bar` stream

Its first byte picks the form:

- **0**: the life bar text as stored.
- **1, compact**: points of `time|value`, range coded.
  - Each time is coded as the frame it falls on (counted from a guess) or as an escaped difference.
  - Each value is coded in hundredths.
  - The decoder prints the canonical text: `0`, `1`, or `0.d` / `0.dd`.

An encoder only uses the compact form when it is smaller and decodes back to the same text. The range coder, its
frequency models and their priors are normative for this form.

## 10. Verbatim replay data

When the replay text cannot be stored as frames (§4), the original LZMA bytes are stored in stream 3, with no frames
stream and a frame count of 0.

## 11. Rebuilding the `.osr`

With `has_osr_header`, the decoder writes, in order:

1. the header bytes (§8);
2. the life bar string: `0x0b`, a ULEB128 length and the text, or `0x00` if there is none;
3. the 8 timestamp bytes;
4. a `u32` byte count;
5. the LZMA replay data;
6. the tail.

Without `has_osr_header`, it writes the replay data and tail only.

**The replay data** is the verbatim bytes (§10), or the text of §4 compressed again as an LZMA stream: 5 property
bytes, the `u64` size and no end mark. The compression settings are the decoder's choice. The reference decoder
defaults to fast settings, with an option that matches osu!stable's.

## 12. What the encoder chooses

A decoder only reads what the encoder chose. The format leaves these choices free:

- the device model of each axis, its segments, the native grids and whether autopilot is on;
- the order of the delta table;
- whether to use the compact forms of §8 and §9.

The reference encoder chooses by fitting candidates and comparing coded sizes. None of that search is part of the
format.

## 13. Known gaps in the reference implementation

Byte-exact round trips are expected. These edge cases currently break that, though no real client writes them:

- **A closing frame written as `-12345|-0|0|<seed>`** (shortest text) is taken as the seed frame and comes back as
  `-12345|0|0|<seed>`. The CRC does not catch it.
- **A life bar marker byte other than `0x00` or `0x0b`** comes back as an empty life bar (`0x0b 0x00`). A non-minimal
  ULEB128 length comes back minimal.
- **A replay data length larger than the bytes left in the file** comes back as the true length, with an empty tail.
