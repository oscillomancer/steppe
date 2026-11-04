# Application icon

Steppe grass bent by the wind on a flat horizon, under a pale moon. The
blades grow taller from left to right, so the grass is also a level meter, and
the last, tallest blade is clay: its red zone.

| File | Use |
|---|---|
| `app.svg` | Master on Apple's icon grid (824 of 1024); source of `app.icns` |
| `app-square.svg` | Nearly full-bleed variant for Windows and Linux (`png/`, `app.ico`) |
| `app-small.svg` | Six broader blades and no moon for 32 px and below, every platform |
| `png/app-<n>.png` | 16 to 1024 px, for Linux icon themes and the window icon |
| `app.ico` | Windows: 16, 24, 32, 48, 64, 256 (PNG entries) |
| `app.icns` | macOS: 16 to 1024, including the @2x sizes |

Colours come from the stock plugins' theme
(`plugins/suite/glue/include/supple/suite/script_theme.h`: walnut, parchment,
clay, and tones either side of sage).

Everything here is generated. Change the geometry in `tools/icon/build.py`,
then rebuild and commit the results:

```sh
python3 tools/icon/build.py      # needs resvg or rsvg-convert
```
