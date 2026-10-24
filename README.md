# GDI child windows are invisible over OpenGL windows in Wine's macOS driver

**Status:** v0.2.0 (2026-10-25) — workaround, not upstreamed. Feedback and better implementations are welcome.

**Upstream bug:** [WineHQ Bugzilla 60578](https://bugs.winehq.org/show_bug.cgi?id=60578) (component `winemac.drv`).

**Tested on:** Wine 11.0 built from CrossOver 26.2.0 sources, macOS 26, Apple Silicon (M5), WoW64 prefix,
1× (non-Retina) displays.

> **AI-assisted ("vibe-coded"), with a human in the loop — read this first.**
> - **v0.1.0** (2026-10-24): the first patch, the test app and the notes were produced by an AI coding agent
>   (**DeepSeek Harness**), steered and tested by the repository owner.
> - **v0.2.0** (2026-10-25): code review of v0.1.0, comparison with how `winex11.drv` solves the same problem,
>   rewrite of the patch, extended test app and A/B tests by **Claude Code** (Anthropic, model Claude Opus 5.5),
>   steered by the repository owner, who confirmed the fix in a real game.
>
> It is **not production-grade**: no warranty, no support, no guarantee it will not break your Wine/CrossOver
> setup. Read the diff, keep a backup of your driver, test in a throwaway prefix, and improve it as you see fit.

## Problem

Under Wine's macOS driver (`winemac.drv`), a window that renders with OpenGL shows its GDI child windows
**invisible**. The typical victim is CEF / Chromium content embedded as a child window in a game or launcher:
the panel exists, its process paints, but the area stays blank (the game's own background shows through).

Reproduced with a minimal app (`test/glchild_overlay.c`): a D3D9 (wined3d → OpenGL) or native OpenGL window
with GDI child windows painted solid red/blue/yellow. Without GL the children are visible; with GL they are not,
although they keep painting (`evidence/00-control-no-gl.png`, `evidence/01-unpatched-children-hidden.png`).

Things that do **not** help: `HKCU\Software\Wine\AppDefaults\<exe>\Mac Driver\OpenGLSurfaceMode` = `behind` /
`transparent`; DXVK/Vulkan/Metal (the GDI content is not presented at all under the Metal view); CEF switches such
as `--in-process-gpu --disable-gpu` alone (necessary for the CEF side, not sufficient).

## Root cause

The window's GDI content — the window surface, which also holds the pixels of every child window — is shown by
the window's content view. An OpenGL drawable gets a **separate view composited above it**, so presenting GL
hides the children.

Wine already has the mechanism for this: win32u's `client_surface` has an `offscreen` flag and a
`present(surface, hdc)` callback ("hdc != NULL when offscreen"). `winex11.drv` uses it exactly for windows with
children (`needs_offscreen_rendering()`): it renders offscreen and copies the frame into the window's DC, below
the children. `winemac.drv` never set `offscreen` and ignored the `hdc`.

## Fix (v0.2.0)

`winemac-offscreen-children.patch` (3 files in `dlls/winemac.drv`: `macdrv.h`, `window.c`, `opengl.c`;
Vulkan/D3DMetal untouched) implements that missing half for OpenGL:

1. `macdrv_client_surface_update()` marks an OpenGL client surface **offscreen** while visible child windows
   overlap its client area. win32u calls `update` before every `SwapBuffers` and when the window moves/resizes,
   so the decision follows the children frame by frame.
2. While offscreen, `SwapBuffers` reads the finished frame from the **drawable's own back buffer** (`glReadPixels`
   on framebuffer 0), **saving and restoring** whatever the application had bound: read framebuffer, read buffer,
   pixel pack buffer and pack parameters. Then it calls `client_surface_present()`, like winex11.
3. `present(hdc)` copies the frame into the window surface (bottom-up DIB, the row order of `glReadPixels` — no
   manual flip) with the child windows clipped out (even without `WS_CLIPCHILDREN`), and keeps the GL view hidden.
   Without an `hdc` the GL view is shown again.
4. Without overlapping children nothing changes: the GL view is presented as before, **no copy, no extra call**.

## What changed since v0.1.0

v0.1.0 worked in the game it was written for, but a review found real defects; v0.2.0 is a rewrite. Each defect
below was reproduced with the extended test app (A/B, same machine, same day):

| v0.1.0 defect | Effect | v0.2.0 |
|---|---|---|
| Read whatever framebuffer the app left bound, then flipped rows by hand | Native OpenGL apps shown **upside down** (`03-v1-opengl-upside-down.png`); D3D9 only worked because wined3d leaves its own FBO bound | Reads framebuffer 0 explicitly; orientation by DIB height (`03-v2-opengl.png`) |
| Did not guard the app's GL state | With a pixel pack buffer bound, the frame went into the app's buffer: **scene missing** (`04-v1-app-gl-state-scene-missing.png`); `GL_READ_BUFFER` changed on every frame | State saved/restored; app state unchanged on every frame (`04-v2-app-gl-state.png`) |
| Row buffer only reallocated when the total area grew | **Heap overflow** when the window became wider but shorter (e.g. 808×605 → 1208×366: 4832-byte rows copied into a 3232-byte buffer) | No row buffer; one frame buffer sized to the current area (`05-v2-resize-wider-shorter.png`) |
| Offscreen mode enabled only if the window had a non-empty client area when the GL surface was created | That window never showed GL content | Decided every frame |
| Custom path next to win32u (view toggled outside `client_view` bookkeeping), 5 files, dead pbuffer code | Harder to review/upstream | Uses win32u's `offscreen` / `present(hdc)` like winex11; 3 files |

## Results (v0.2.0)

| Scenario (`test/glchild_overlay.c`) | Result |
|---|---|
| D3D9 (wined3d), children before/after the device + one late child | children visible, scene upright (`02-v2-d3d9.png`) |
| Native OpenGL (WGL, legacy 2.1 context) | children visible, upright (`03-v2-opengl.png`) |
| OpenGL + hostile app state (`--hostile-state`) | correct scene, app GL state unchanged on 100% of frames |
| Window resized to wider-but-shorter | correct, no crash (`05-v2-resize-wider-shorter.png`) |
| No children (`--no-children`, `WINEDEBUG=+wgl`) | 87/87 swaps on the normal path, 0 `present` calls (`06-…`) |
| Child appears, then is hidden | switches to offscreen and back automatically (`07-…`) |
| Real game: Metin2 (Gameforge client), whose in-game login is a CEF child window | login panel renders, login → character select → world; confirmed by the repository owner |

## Build

```sh
# in a Wine source tree that has the mac driver (64-bit unix side):
patch -p1 < winemac-offscreen-children.patch
make -j8 dlls/winemac.drv/winemac.so
# back up the installed driver, then replace it atomically (no Wine process running)
cp "$WINE_PREFIX/lib/wine/x86_64-unix/winemac.so" "$WINE_PREFIX/lib/wine/x86_64-unix/winemac.so.orig"
cp dlls/winemac.drv/winemac.so "$WINE_PREFIX/lib/wine/x86_64-unix/winemac.so.new"
mv "$WINE_PREFIX/lib/wine/x86_64-unix/winemac.so.new" "$WINE_PREFIX/lib/wine/x86_64-unix/winemac.so"
```

Rollback: restore `winemac.so.orig`. (`$WINE_PREFIX` here is the Wine **installation** prefix, not `WINEPREFIX`.)

## Reproduce

```sh
sh test/build.sh                       # glchild_overlay32.exe (mingw-w64) + pixstat (Xcode CLT)
# in a WoW64 prefix using the mac driver (no DISPLAY):
wine test/glchild_overlay32.exe --seconds 8 --pos 200 100             # D3D9
wine test/glchild_overlay32.exe --opengl --hostile-state --seconds 8  # native OpenGL, hostile app state
wine test/glchild_overlay32.exe --opengl --resize-at 3000 1216 400 --seconds 8
wine test/glchild_overlay32.exe --late-only --hide-at 5500 --seconds 8
# capture the window only, then count colours / check orientation:
screencapture -o -l <windowid> shot.png && test/pixstat shot.png
```

Other options: `--no-gl` (control), `--no-children` (normal path), `--no-poke`, `--pos X Y`. The app prints
`STATE`/`RESULT` lines (paint counters, `glstate_changed`), so "not painted" and "painted but hidden" can be told
apart. `pixstat` prints the pixel count and row range of each colour and `orientation UPRIGHT/FLIPPED`.

## Limitations

- While children overlap the GL window, every frame is read back synchronously (≈5 MB at 1440×900 plus a GPU→CPU
  sync). Fine for login/shop panels, not for video playing on top of a game. A PBO-based async readback, or
  sharing an IOSurface with the compositor, would be better.
- OpenGL only (wined3d). DXVK/MoltenVK and D3DMetal surfaces keep the original behaviour.
- Only the GL window's direct children are considered; child clipping is rectangular.
- HiDPI/Retina, fullscreen, multi-monitor moves and other GPUs/macOS versions are untested.
- The patch changes every OpenGL window in that Wine build (use a dedicated build).

## Evidence (`evidence/`)

Window-only captures of the test app — no game files, accounts or third-party content.
`00` control without GL · `01` unpatched (v0.1.0-era harness, single clear colour) · `02`–`07` v0.2.0 results and
v0.1.0 A/B (`03-v1-…`, `04-v1-…`), as listed in the tables above.

## License

- patch: **LGPL-2.1-or-later** (derivative of Wine's `dlls/winemac.drv`; see `COPYING.LIB`);
- test app and tools (`test/`): **MIT**;
- text and screenshots: CC BY 4.0.

No warranty. Not affiliated with WineHQ, CodeWeavers, Gameforge or Webzen.
