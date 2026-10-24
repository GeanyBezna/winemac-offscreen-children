# Where to publish / upstream

This is a macOS **presentation** workaround, not the Windows-side CEF/DComp bugs. It fixes the case where the
window's GDI content (and its child windows) disappears behind the GL/Vulkan surface.

## 1. WineHQ GitLab (upstream) - best long-term

- Repo: https://gitlab.winehq.org/wine/wine
- Create an account, fork, add the patch as a commit with a proper message and a `Signed-off-by:` line
  (Developer Certificate of Origin), open a Merge Request against `dlls/winemac.drv`.
- Suggested commit subject: `winemac.drv: Present GL frames offscreen while the window has visible children.`
- Expect discussion: upstream may prefer sharing the surface with the compositor (IOSurface/CALayer) instead of
  reading pixels back; the MR is still the right place to make the problem visible.

## 2. WineHQ Bugzilla (a new bug)

- https://bugs.winehq.org/ - product Wine, component `wine-mac-driver`.
- Suggested summary: `winemac: GDI child windows are invisible over an OpenGL window (CEF/Chromium panels blank)`
- Attach or link the patch and the test app; mention reproduction steps and the negative results
  (`OpenGLSurfaceMode=behind/transparent`, DXVK/Metal).
- Related but different (Windows-side, keep separate): bug 58921 (WebView2 + Windows >= 8.1) and 59370
  (CEF 109 blank via D3D11 shared textures).

## 3. CodeWeavers (CrossOver)

- CrossOver 26.2.0 ships the Wine source this patch was built against; a support ticket or forum post with the
  patch, the reproduction and the limitations is appropriate.
- They may prefer to ship it in `winemac.drv` or to handle it in compatdb (`append_cmd_line` is not enough; the
  fix is in the driver).

## 4. Community posts (discoverability)

- r/macgaming, r/wine_gaming: link the repo with a one-paragraph problem/solution.
- Game/launcher forums where users hit blank CEF panels under CrossOver (Metin2/Gameforge, other CEF launchers),
  link the repo rather than attaching files.
- WineHQ AppDB entry of the affected app: a short note + link.

## What NOT to publish

- account names, login tokens, personal paths, or screenshots of the game/client with other players or account UI;
- game or launcher files (copyright);
- the private ticket documents from the department.

## Suggested repo metadata

- name: `winemac-offscreen-children`
- description: `Wine winemac.drv workaround: keep GDI child windows (CEF/Chromium panels) visible over OpenGL windows.`
- topics: `wine`, `macos`, `winemac`, `cef`, `chromium`, `opengl`, `winehq`
