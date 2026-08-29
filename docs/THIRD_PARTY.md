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

## What is deliberately NOT used

FreeDO, 4DO, Opera and `opera-libretro`, whose licence forbids commercial use
of the sources "or any knowledge obtained by studying or reverse engineering"
them. Every other 3DO emulator with published source - 3doh, 3dox, 3doplay -
descends from that same lineage and is excluded for the same reason. Phoenix is
closed source.
