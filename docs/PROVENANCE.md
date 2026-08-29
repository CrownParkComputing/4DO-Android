# Provenance and licensing position

This file records where the emulator core came from, which parts carry which
terms, and what this project is doing about it. It is written to be accurate
rather than reassuring: if a claim here cannot be supported, it should be
removed rather than softened.

## The chain

```
FreeDO (early 2000s, dormant)
   └── Opera / opera-libretro (actively maintained)
          └── this project's app/cpp/native_core/
```

Three separate things are often run together, and they are not the same:

| Project | What it is | State |
|---|---|---|
| **FreeDO** | The original emulator. Its authors wrote the licence clause below. | Dormant. No activity since the 2000s. |
| **Opera / opera-libretro** | The maintained descendant. Our core is derived from it. | **Active** — last pushed 2026-08-21. |
| **4DO** | An old separate frontend. | Dead. Not used here. |

This matters for one specific reason: the approval the clause requires is from
**the FreeDO authors**, not from Opera's maintainer. Opera being actively
developed does not grant it, and Opera's maintainer is not in a position to.

## The clause

Seven files under `app/cpp/native_core/` carry the FreeDO notice. In full, the
part that binds:

> Any commercial uses of FreeDO sources or any knowledge obtained by studying or
> reverse engineering of the sources, or any other material published by FreeDO
> is strictly forbidden without owners approval.
>
> The above notes are taking precedence over GNU LGPL in conflicting situations.

That last line is the one that matters. The files are nominally LGPL, but the
addendum explicitly overrides the LGPL where they conflict — and permission to
sell is exactly such a conflict.

## What is actually affected

| File | Lines | Clause |
|---|---|---|
| `native_madam.c` | 3007 | yes |
| `native_arm.c` | 1944 | yes |
| `native_dsp.c` | 1306 | yes |
| `native_cdrom.c` | 1121 | yes |
| `native_clio.c` | 911 | yes |
| `native_3do.c` | 335 | yes |
| `native_sport.c` | 214 | yes |

Everything else is unaffected: `native_vdlp.c`, `native_xbus.c`, `native_pbus.c`,
`native_mem.c`, the backend headers, and the entire Android application.

## Contact

The FreeDO authors have been contacted several times about commercial use. No
reply has been received.

Two things follow from that, and only one of them is comfortable:

- The good-faith effort is real and is recorded here.
- **Silence is not permission, and dormancy is not abandonment.** Copyright does
  not lapse because a project stops being updated, and an unanswered email does
  not become approval by default. Nothing in this section should be read as
  saying the restriction has gone away.

If any FreeDO author makes contact, this project will comply with whatever they
ask, including withdrawal.

## What is being done about it

The seven files are being replaced with clean-room implementations written from
permitted sources only — The 3DO Company's own patents and SDK documentation,
the community 3DOessence register map, MAME's independently written 3DO devices
(BSD-3-Clause), and black-box observation of the boot ROM. That work lives in
the `Retro-3DO` project and already has working equivalents for four of the
seven:

| Encumbered file | Clean-room replacement | State |
|---|---|---|
| `native_arm.c` | `arm60.cpp` | working, decode-cached |
| `native_clio.c` | `clio.cpp` | working |
| `native_3do.c` | `console.cpp` | working |
| `native_sport.c` | `sport.cpp` | working |
| `native_madam.c` | `madam.cpp` | written, not yet exercised |
| `native_cdrom.c` | `xbus.cpp` | transport working |
| `native_dsp.c` | — | not started |

Each replacement can be swapped in and verified independently against the
existing core, so this does not need to be a single flag-day rewrite.

## Status

Until those replacements are complete, the core in this repository carries the
FreeDO restriction. That is a statement of fact, not of intent, and the decision
about what to do in the meantime sits with the project owner rather than with
this document.
