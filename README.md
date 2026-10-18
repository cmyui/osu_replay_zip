# ORZ format

.orz is a lossless compression format to store osu replay frames. [SPEC_OUTLINE.md](SPEC_OUTLINE.md) outlines the format.

The format is still maturing, please do NOT use this format just yet!
There can and will be breaking changes as the initial V0 is tuned on a dataset thats over a terabyte graciously provided by cmyui (akatsuki private server)

Replay set: https://drive.google.com/file/d/1i2oe929loEzhvCfRYG-DeZZQ_YouaBYD/view

Sizes are totals over the whole set: the .osr replay data (LZMA) in, the .orz out, in bytes.

# osu!
27.566% of the original size (4,162 replays)

226,602,464 -> 62,465,962

# taiko
45.296% of the original size (456 replays)

2,794,427 -> 1,265,776

# mania
46.629% of the original size (1,561 replays)

14,573,725 -> 6,795,636

# fruit
44.945% of the original size (373 replays)

10,419,674 -> 4,683,086

# Total
29.565% of the original size (6,552 replays)

254,390,290 -> 75,210,460