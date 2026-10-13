# Replay fixtures

These fixtures are synthetic and contain no production replay data.

- `frames.txt`: 3,000 integer-coordinate gameplay frames with varied time deltas
  and buttons, followed by a seed frame.
- `frames-sdk457.lzma`: LZMA-alone encoding produced by the existing SDK 4.57
  benchmark bridge before integrating this adapter. Settings: 2 MiB dictionary,
  lc=3/lp=0/pb=2, normal mode, BT4, fastBytes=255, default match-finder cycles,
  one thread, no end marker.
- `synthetic.osr`: the same payload in a synthetic version-20211103 OSR envelope.
- `sdk21.orz`: `synthetic.osr` encoded by upstream commit
  `006810908dfd19620655773f1c3de3d2d6430502` with SDK 21.02 and the extra
  round-trip check disabled. Its CRC is checked when the SDK 4.57 decoder reads it.

The golden LZMA bytes protect historical encoder parity. The ORZ fixture protects
compatibility with previously encoded containers. Tests also check a newly
encoded ORZ's complete stable OSR reconstruction and default fast reconstruction.
