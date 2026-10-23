# UplinkUIScale

UI scaling for **Uplink** (Introversion Software) at high resolutions.

At 1440p or 4K, Uplink's interface becomes tiny. This mod makes the game lay
out its UI as if the screen were 1024x768, then scales it up to your native
resolution. The interface keeps the size and proportions it has at 1024x768.

- Works with the **original, GOG and Steam** versions (no hardcoded addresses)
- Shapes, lines and textures are drawn at full native resolution
- Smooth or pixel-perfect scaling for text and images
- Correct mouse input and a scaled mouse cursor
- Fullscreen, windowed or borderless; DPI-aware
- Everything tunable in an `.ini` file


## Download

**[⬇️ Download Latest Release](../../releases/latest)**

## Install

1. Download the latest release.
2. Copy `version.dll` and `UplinkUIScale.ini` into the Uplink folder, next to `uplink.exe`.
3. Start the game.

To uninstall, delete the two files.

If you use an ASI loader, you can rename `version.dll` to `UplinkUIScale.asi`.

## Scaling

The scale is `min(width / 1024, height / 768)`:

| Resolution | Scale | Game layout |
|---|---|---|
| 1920x1080 | 1.41 | 1365x768 |
| 2560x1440 | 1.88 | 1365x768 |
| 3840x2160 | 2.81 | 1365x768 |

The layout never goes below 800x600.

The in-game resolution list shows only the layout size. Set the real
resolution in the ini.

## Settings

| Section | Key | Default | Description |
|---|---|---|---|
| Display | `Width`, `Height` | `0` | Output resolution (0 = desktop) |
| Display | `Mode` | `borderless` | `auto`, `fullscreen`, `windowed`, `borderless` |
| Display | `DPIAware` | `1` | Prevent Windows DPI blurring |
| Scale | `Scale` | `0` | 0 = automatic, or a fixed scale such as `2.0` |
| Scale | `Multiplier` | `1.0` | Adjusts the automatic scale (<1 smaller, >1 bigger) |
| Scale | `IntegerScale` | `0` | Whole-number scale only (pixel-perfect) |
| Scale | `BaseWidth`, `BaseHeight` | `1024`, `768` | Reference layout |
| Rendering | `TextFilter` | `auto` | `auto`, `smooth`, `sharp`, `linear` |
| Rendering | `TextBoldness` | `0.05` | Weight of smooth text (0.0–0.4) |
| Rendering | `ImageFilter` | `auto` | `auto`, `sharp`, `linear` |
| Rendering | `LineScale` | `1.0` | Line thickness multiplier |
| Cursor | `ScaleCursor` | `1` | Scaled cursor (0 = system cursor) |
| Cursor | `CursorSize` | `1.0` | Cursor size multiplier |
| General | `Enabled` | `1` | Turn the mod off without removing it |
| General | `Log` | `0` | Write `UplinkUIScale.log` |

## How it works

The DLL loads as a `version.dll` proxy. It redirects the game's own calls into
SDL 1.2 and OpenGL, which it finds by name:

- `SDL_ListModes` / `SDL_SetVideoMode`: the game is offered one logical
  resolution, while the real window opens at native size.
- `glBitmap`: text glyphs are redrawn as scaled textures.
- `glDrawPixels`: images are redrawn as scaled textures.
- `glScissor`, `glLineWidth`, `glLineStipple`, `glReadPixels`: pixel units are
  converted.
- `SDL_PollEvent` / `SDL_GetMouseState`: mouse coordinates are mapped back.

## Building

Requires Visual Studio with the x86 toolset. Run `src\build.cmd`. The output
goes to `build\`.

## Credits

- **Uplink** © Introversion Software. You need a legitimate copy of the game.
- The mod was designed by studying the **Uplink source code** (Introversion
  Software, *Uplink Developer CD* / Humble Bundle source release, under the
  *Uplink Developers License*). The parts studied were the Gucci graphics layer
  (`gucci_sdl.cpp`, `gucci.cpp`, `image.cpp`), `opengl.cpp` and the bundled
  **gltt 2.5** font renderer. No Uplink source code is included or
  redistributed.
- [SDL 1.2](https://www.libsdl.org/) for the event, rectangle and video API
  layouts.
