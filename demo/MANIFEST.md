# Built-in ROM manifest

| Field | Value |
|---|---|
| Display name | Retro-3DO Original Hardware Demo |
| Copyright owner | Crown Park Computing |
| Source | `demo/retro3do-demo.s` |
| Authored image size | 1,794 bytes |
| Authored SHA-256 | `f019b2a87431cfaec321dd894201f64b36ce964684661bb3eaf18d49fe1e0a4b` |
| Loader image size | 1,048,576 bytes, zero-padded |
| Loader SHA-256 | `4d76c53f120b6f0465ae434108773747265ad91e6ce7b9050dfb19a16e3cab49` |
| Licence | MIT |

The generated C++ byte array is `src/core/demo_rom_data.h`. It contains only
the assembled bytes from the source named above. The remaining bytes in the
3DO ROM window are supplied as zeroes by the ordinary Retro-3DO BIOS loader.

No Portfolio headers, libraries, source, object code, retail BIOS, game code,
artwork, music, or third-party ROM material is present in this image.
