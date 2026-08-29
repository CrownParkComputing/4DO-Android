# Third-party notices

This project is MIT licensed. It also carries obligations from the sources
below, which must ship with any binary distribution.

## MAME — BSD-3-Clause

Parts of the CLIO expansion-bus implementation in `src/core/clio.cpp` are
derived from MAME's 3DO devices (`src/mame/misc/3do_clio.cpp`), specifically
the poll-register bit layout and the fact that SELECTION names a device by the
value written rather than by the address written to.

> Copyright (c) Angelo Salese, Wilbert Pol
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice,
>    this list of conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright notice,
>    this list of conditions and the following disclaimer in the documentation
>    and/or other materials provided with the distribution.
> 3. Neither the name of the copyright holder nor the names of its contributors
>    may be used to endorse or promote products derived from this software
>    without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
> ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
> LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
> CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
> INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
> CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
> ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.

## MAME's CR-560B device — BSD-3-Clause

The CD-ROM drive model in `src/core/xbus.cpp` follows MAME's `cr560b` device
(`src/devices/machine/cr560b.cpp`), copyright Angelo Salese, BSD-3-Clause. The
drive is a Panasonic CR-560B on a pre-IDE MKE interface - the drive the FZ-1 and
FZ-10 actually contain - and that device is where its command set is written
down. Nothing else consulted here documents it: not the patents, not The 3DO
Company's SDK, not the community register map.

Taken from it: the command opcodes, that every command echoes its opcode in the
first reply byte, the shape of the version and data-path-check replies, and the
drive status bits.

Worth recording that two of those were established here independently first, by
sweeping the reply against the real boot ROM, and MAME then confirmed them: the
opcode echo, and the twelve-byte length of the version reply. The same notice as
above applies - see the MAME entry.

## libchdr — BSD-3-Clause

`third_party/libchdr`, vendored whole, is used to read CHD images. CHD is how
nearly every 3DO dump in the wild is stored, so this is the ordinary path into
the emulator rather than an optional extra. It is self-contained: it brings its
own LZMA, miniz and zstd, so there is nothing to find on the host and nothing
extra to cross compile for Android or iOS.

> Copyright Romain Tisserand
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice,
>    this list of conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright notice,
>    this list of conditions and the following disclaimer in the documentation
>    and/or other materials provided with the distribution.
> 3. Neither the name of the copyright holder nor the names of its contributors
>    may be used to endorse or promote products derived from this software
>    without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. See
> `third_party/libchdr/LICENSE.txt` for the full text.

Its bundled dependencies carry their own permissive terms: the LZMA SDK is
public domain, miniz is MIT, and zstd is BSD/GPLv2 dual licensed (BSD taken).

## What is deliberately NOT used

FreeDO, 4DO, Opera and `opera-libretro`, whose licence forbids commercial use
of the sources "or any knowledge obtained by studying or reverse engineering"
them. Every other 3DO emulator with published source - 3doh, 3dox, 3doplay -
descends from that same lineage and is excluded for the same reason. Phoenix is
closed source.
