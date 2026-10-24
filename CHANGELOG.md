# Changelog

## v0.2.0 — 2026-10-25

Rewrite after a code review of v0.1.0 (Claude Code, Anthropic — model Claude Opus 5.5; human-steered and
confirmed in a real game).

- Use win32u's `client_surface.offscreen` / `present(hdc)` like `winex11.drv` instead of a custom blit in the
  swap path; the offscreen decision is recomputed on every `SwapBuffers` and on window moves/resizes.
- Read the frame from the drawable's framebuffer 0, saving/restoring the read framebuffer, read buffer, pixel
  pack buffer and pack parameters.
  - fixes native OpenGL apps shown upside down;
  - fixes the scene missing when the app keeps a pixel pack buffer bound, and `GL_READ_BUFFER` being changed;
- No manual row flip: the DIB is bottom-up, the row order of `glReadPixels`.
- Fix a heap overflow when the window became wider but shorter (row buffer not reallocated).
- Fix GL content never shown for a window whose client area was empty when its GL surface was created.
- A child window counts only if it is visible and overlaps the client area.
- 3 files instead of 5 (`vulkan.c`, `d3dmetal.c` untouched); dead pbuffer code removed.
- Test app: asymmetric scene (orientation), `--opengl`, `--hostile-state`, `--resize-at`, `--hide-at`;
  new `test/pixstat.swift` capture analyser.

## v0.1.0 — 2026-10-24

First public snapshot (AI coding agent via DeepSeek Harness; human-steered and tested): conditional offscreen
presentation — read back + GDI blit while visible children exist, normal presentation otherwise. Worked in the
target game; see v0.2.0 for the defects found later.
