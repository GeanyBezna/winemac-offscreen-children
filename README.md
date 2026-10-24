# GDI child windows are invisible over OpenGL windows in Wine's macOS driver

**Status:** workaround, not upstreamed (2026-10-24). Feedback and better implementations are welcome.

> **Vibe-coded / AI-assisted.** This was produced in one debugging session by a human + an AI coding agent
> (DeepSeek Harness): the agent ran the experiments, wrote the patch, the harness and these notes, with the human
> steering and testing. It is **not production-grade**: no warranty, no support, no guarantee it will not break
> your Wine/CrossOver setup. Read the diff, keep a backup of your driver, test in a throwaway prefix, and improve
> it as you see fit.

**Tested on:** Wine 11.0 built from CrossOver 26.2.0 sources, macOS 26, Apple Silicon (Rosetta/WoW64 prefix).

## Problem

Under Wine's macOS driver (`winemac.drv`), a window that renders with OpenGL shows its GDI child windows
**invisible**. The typical victim is windowed CEF / Chromium (WebView2-style) content embedded in a game or
launcher: the panel exists, its process paints, but the area stays blank (the game's own background shows through).

Reproduced with a minimal app (`test/glchild_overlay.c`): a D3D9 window (wined3d -> OpenGL) with two GDI child
windows painted solid red/blue. Without GL the children are visible; with GL they are not, although they keep
painting.

Things that do **not** help:

- `HKCU\\Software\\Wine\\AppDefaults\\<exe>\\Mac Driver\\OpenGLSurfaceMode` = `behind` or `transparent`
  (CGL surface order/opacity has no visible effect on a view-attached context on macOS 26);
- DXVK / Vulkan / Metal: the Metal view is composited, but the window's GDI content (and therefore all child
  windows) is not presented at all when the GL/Vulkan surface becomes the visible client view;
- CEF switches such as `--in-process-gpu --disable-gpu` alone (they are necessary, but not sufficient).

## Root cause

In Wine 11's mac driver the window's GDI content (the window surface, which also contains the pixels of every
child window) is shown by the window's content view. An OpenGL drawable creates a **separate view** whose surface
is composited **above** that content. When the GL surface is presented, the GDI content - including all child
windows - is hidden. `OpenGLSurfaceMode` only changes CGL surface parameters, which are ignored for a
view-attached `NSOpenGLContext` on current macOS.

## Fix

`winemac-offscreen-children.patch` (5 files in `dlls/winemac.drv`) makes presentation conditional:

1. the GL window drawable starts with its view hidden;
2. at every `SwapBuffers`, if the window has **visible child windows with a non-zero client area**, the driver
   reads the frame (`glReadPixels`), flips it vertically and blits it into the window surface with GDI
   (`NtGdiSetDIBitsToDeviceInternal`), clipping out the child rectangles; the children stay above the GL content;
3. with no visible children, the GL view is shown again and presentation is normal (zero copy);
4. switching happens automatically and costs at most one frame.

## Build

```sh
# in a Wine source tree that has the mac driver (64-bit unix side):
patch -p1 < winemac-offscreen-children.patch
make -j8 dlls/winemac.drv/winemac.so
# back up the installed driver first
cp "$WINE_PREFIX/lib/wine/x86_64-unix/winemac.so" "$WINE_PREFIX/lib/wine/x86_64-unix/winemac.so.orig"
cp dlls/winemac.drv/winemac.so "$WINE_PREFIX/lib/wine/x86_64-unix/winemac.so"
```

Rollback: restore `winemac.so.orig`.

## Reproduce

```sh
i686-w64-mingw32-gcc -O2 -o glchild_overlay32.exe test/glchild_overlay.c -ld3d9 -lgdi32 -luser32
# in a WoW64 prefix using the mac driver (no DISPLAY):
wine glchild_overlay32.exe --seconds 12 --pos 200 100
```

Options: `--no-gl` (control, children must be visible), `--no-children` (fast path), `--late-only` (child created
after 2.5 s, tests the automatic switch), `--no-poke`, `--seconds N`, `--pos X Y`. Capture the window only
(`screencapture -l <windowid>` on macOS) and count the red/blue/yellow pixels - the program prints paint counters
to stdout, so you can tell "not painted" from "painted but hidden".

## Limitations

- The copy runs **only** while visible child windows exist (login/shop panels); gameplay keeps the normal, free
  path. While a panel is open the per-frame readback + GDI blit costs CPU (roughly 5 MB read + 5 MB copy at
  1440x900, plus a GPU->CPU sync); fine for dialogs, not for full-screen video.
- Child clipping is rectangular; rounded/translucent child windows may show stale corners.
- Fullscreen, Retina/HiDPI and multi-monitor moves are untested.
- The patch changes every OpenGL window in that Wine build (use a dedicated build).
- It is a workaround; a cleaner fix would share the GPU surface with the compositor (IOSurface/CALayer) instead of
  copying pixels.

## Evidence (`evidence/`)

1. control without GL - both child windows visible;
2. GL window without the patch - children painted but invisible;
3. patch, before the late child - normal GL presentation;
4. patch, after the late child appeared - child visible above GL, automatic switch;
5. patch, no children - fast path (no copy).

The screenshots show only our own test app; no game files, accounts or third-party content.

## License

- patch: **LGPL-2.1-or-later** (derivative of Wine's `dlls/winemac.drv`; see `COPYING.LIB`);
- test harness (`test/`): **MIT**;
- text and screenshots: CC BY 4.0.

No warranty. Not affiliated with WineHQ or CodeWeavers.