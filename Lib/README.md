# LZMA SDK 4.57

This directory vendors the C/C++ LZMA SDK 4.57 subset from
[cmyui/osu-replay-compress](https://github.com/cmyui/osu-replay-compress/tree/5abbe70b16af84c3b9500d0d0e26e21aa141d49d/vendor/lzma457).
The encoder, decoder, match finder, buffers, and range coder are included;
unused archive and filter implementations are omitted.

The files retain that bundle's portability adjustments, including fixed-width
Windows compatibility types in `CPP/Common/MyWindows.h`. No additional SDK
source modifications are applied here. The application adapter lives outside
this directory in `lzma.cpp`; GUID definitions live in `lzma_guids.cpp`.

Copyright 1999–2007 Igor Pavlov. See `lzma.txt` for the original license terms
and linking exceptions, and `LGPL.txt` for the LGPL text. The SDK 4.57 license
is different from the public-domain license of recent SDK releases.
