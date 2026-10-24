/*
 * glchild_overlay.c — minimal reproduction: GDI child windows over an OpenGL window under winemac.drv.
 *
 * Creates a top-level window that renders with D3D9 (wined3d -> OpenGL) or with native
 * OpenGL (WGL), and GDI child windows (one created before the device, one after it, one
 * late). The 3D scene is asymmetric — top half green, bottom half magenta — so a
 * window-only screenshot shows both whether the GDI child windows are visible above the
 * GL surface and whether the GL frame is upside down. The children paint solid colours
 * and a live counter, so "not painted" and "painted but hidden" can be told apart.
 *
 * Build (32-bit):
 *   i686-w64-mingw32-gcc -O2 -o glchild_overlay32.exe glchild_overlay.c -ld3d9 -lopengl32 -lgdi32 -luser32
 *
 * Options: --no-gl            no 3D device (control run: children must be visible)
 *          --opengl           native OpenGL (WGL) instead of D3D9
 *          --hostile-state    (OpenGL) leave a pixel pack buffer, odd pack parameters and
 *                             GL_READ_BUFFER=GL_FRONT bound across SwapBuffers and report
 *                             whether the driver changed them
 *          --no-children      no child windows (fast path)
 *          --late-only        only the late child (created after 2.5 s)
 *          --no-poke          do not move child 1 at 3.5 s
 *          --hide-at MS       hide all child windows after MS milliseconds
 *          --resize-at MS W H resize the window to W x H after MS milliseconds
 *          --seconds N        auto-exit after N seconds (default 25)
 *          --pos X Y          window position (default 2100 100)
 * Prints machine-readable status lines to stdout: "STATE ...", "RESULT ...".
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER         0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_STREAM_READ               0x88E1
#endif

typedef void (APIENTRY *PFN_GENBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_BINDBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFN_BUFFERDATA)(GLenum, ptrdiff_t, const void *, GLenum);

static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static UINT g_bb_width, g_bb_height;
static HDC g_hdc;
static HGLRC g_glrc;
static GLuint g_pbo;
static HWND g_main, g_child1, g_child2, g_child3;
static int g_running = 1, g_use_gl = 1, g_opengl, g_hostile, g_seconds = 25, g_pos_x = 2100, g_pos_y = 100;
static volatile LONG g_paints1, g_paints2, g_paints3;
static unsigned g_frames, g_state_changed;
static int g_late_ms = 2500, g_poke_ms = 3500, g_hide_ms = -1, g_resize_ms = -1, g_resize_w, g_resize_h;
static int g_late_done, g_poke_done, g_hide_done, g_resize_done;
static int g_no_children, g_late_only;

static void paint_child(HWND hwnd, HDC hdc, COLORREF bg, const char *label, volatile LONG *counter)
{
    RECT rc;
    HBRUSH br;
    char buf[128];
    LONG n = InterlockedIncrement(counter);

    GetClientRect(hwnd, &rc);
    br = CreateSolidBrush(bg);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
    TextOutA(hdc, 12, 12, label, (int)strlen(label));
    wsprintfA(buf, "paint #%ld", n);
    TextOutA(hdc, 12, 34, buf, (int)strlen(buf));
}

static LRESULT CALLBACK child_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PAINTSTRUCT ps;
    HDC hdc;
    LONG_PTR id;

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        hdc = BeginPaint(hwnd, &ps);
        id = GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        if (id == 1)      paint_child(hwnd, hdc, RGB(220, 30, 30), "CHILD 1 (before GL)", &g_paints1);
        else if (id == 2) paint_child(hwnd, hdc, RGB(30, 60, 220), "CHILD 2 (after GL)", &g_paints2);
        else              paint_child(hwnd, hdc, RGB(230, 200, 20), "CHILD 3 (late, live parent)", &g_paints3);
        EndPaint(hwnd, &ps);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_DESTROY:
        g_running = 0;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_running = 0; PostMessageA(hwnd, WM_CLOSE, 0, 0); }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void print_state(const char *tag)
{
    RECT r, c;
    GetWindowRect(g_main, &r);
    GetClientRect(g_main, &c);
    printf("STATE %s main=%p child1=%p child2=%p child3=%p frames=%u paints1=%ld paints2=%ld paints3=%ld "
           "win=(%ld,%ld)-(%ld,%ld) client=%ldx%ld glstate_changed=%u\n",
           tag, (void *)g_main, (void *)g_child1, (void *)g_child2, (void *)g_child3, g_frames,
           (LONG)g_paints1, (LONG)g_paints2, (LONG)g_paints3, r.left, r.top, r.right, r.bottom,
           c.right, c.bottom, g_state_changed);
    fflush(stdout);
}

static int create_device(void)
{
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    D3DADAPTER_IDENTIFIER9 id;
    RECT rc;
    HRESULT hr;

    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) { printf("RESULT d3d_create=FAILED\n"); return 0; }
    if (FAILED(hr = g_d3d->lpVtbl->GetAdapterIdentifier(g_d3d, D3DADAPTER_DEFAULT, 0, &id)))
        printf("RESULT adapter_id=hr=0x%08lx\n", (unsigned long)hr);
    else
        printf("STATE adapter=%s desc=%s\n", id.Driver, id.Description);

    memset(&mode, 0, sizeof(mode));
    g_d3d->lpVtbl->GetAdapterDisplayMode(g_d3d, D3DADAPTER_DEFAULT, &mode);

    GetClientRect(g_main, &rc);
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = g_bb_width = rc.right - rc.left;
    pp.BackBufferHeight = g_bb_height = rc.bottom - rc.top;
    pp.hDeviceWindow = g_main;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.EnableAutoDepthStencil = FALSE;

    hr = g_d3d->lpVtbl->CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_main,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev);
    printf("RESULT create_device=0x%08lx\n", (unsigned long)hr);
    fflush(stdout);
    return SUCCEEDED(hr);
}

static void draw_d3d9(void)
{
    D3DRECT top = {0, 0, g_bb_width, g_bb_height / 2}, bottom = {0, g_bb_height / 2, g_bb_width, g_bb_height};

    g_dev->lpVtbl->Clear(g_dev, 1, &top, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 200, 0), 1.0f, 0);
    g_dev->lpVtbl->Clear(g_dev, 1, &bottom, D3DCLEAR_TARGET, D3DCOLOR_XRGB(200, 0, 200), 1.0f, 0);
    g_dev->lpVtbl->BeginScene(g_dev);
    g_dev->lpVtbl->EndScene(g_dev);
    g_dev->lpVtbl->Present(g_dev, NULL, NULL, NULL, NULL);
}

static int create_gl(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    int format;

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;

    g_hdc = GetDC(g_main);
    if (!(format = ChoosePixelFormat(g_hdc, &pfd)) || !SetPixelFormat(g_hdc, format, &pfd))
    {
        printf("RESULT pixel_format=FAILED err=%lu\n", GetLastError());
        return 0;
    }
    if (!(g_glrc = wglCreateContext(g_hdc)) || !wglMakeCurrent(g_hdc, g_glrc))
    {
        printf("RESULT gl_context=FAILED err=%lu\n", GetLastError());
        return 0;
    }
    printf("STATE gl_renderer=%s gl_version=%s\n", (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));

    if (g_hostile)
    {
        PFN_GENBUFFERS gen = (PFN_GENBUFFERS)wglGetProcAddress("glGenBuffers");
        PFN_BINDBUFFER bind = (PFN_BINDBUFFER)wglGetProcAddress("glBindBuffer");
        PFN_BUFFERDATA data = (PFN_BUFFERDATA)wglGetProcAddress("glBufferData");

        if (!gen || !bind || !data) { printf("RESULT hostile=unavailable\n"); g_hostile = 0; }
        else
        {
            gen(1, &g_pbo);
            bind(GL_PIXEL_PACK_BUFFER, g_pbo);
            data(GL_PIXEL_PACK_BUFFER, 4096, NULL, GL_STREAM_READ);
            glPixelStorei(GL_PACK_ALIGNMENT, 8);
            glPixelStorei(GL_PACK_ROW_LENGTH, 7);
            glPixelStorei(GL_PACK_SKIP_ROWS, 3);
            glReadBuffer(GL_FRONT);
            printf("STATE hostile pbo=%u pack_alignment=8 row_length=7 skip_rows=3 read_buffer=GL_FRONT\n", g_pbo);
        }
    }
    fflush(stdout);
    return 1;
}

static void check_gl_state(void)
{
    GLint pbo = -1, alignment = -1, row_length = -1, skip_rows = -1, read_buffer = -1;

    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    if (pbo != (GLint)g_pbo || alignment != 8 || row_length != 7 || skip_rows != 3 || read_buffer != GL_FRONT)
    {
        if (!g_state_changed)
            printf("STATE glstate CHANGED after SwapBuffers: pbo=%d alignment=%d row_length=%d skip_rows=%d "
                   "read_buffer=0x%x\n", pbo, alignment, row_length, skip_rows, read_buffer);
        g_state_changed++;
    }
}

static void draw_gl(void)
{
    RECT rc;
    int w, h;

    GetClientRect(g_main, &rc);
    w = rc.right;
    h = rc.bottom;
    glViewport(0, 0, w, h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, h - h / 2, w, h / 2);   /* GL origin is bottom-left: this is the top half */
    glClearColor(0.0f, 200 / 255.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(0, 0, w, h - h / 2);
    glClearColor(200 / 255.0f, 0.0f, 200 / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    SwapBuffers(g_hdc);
    if (g_hostile) check_gl_state();
}

static void hide_children(void)
{
    if (g_child1) ShowWindow(g_child1, SW_HIDE);
    if (g_child2) ShowWindow(g_child2, SW_HIDE);
    if (g_child3) ShowWindow(g_child3, SW_HIDE);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    MSG msg;
    DWORD start, last_stats = 0;
    int i, ok = 1;
    HINSTANCE inst = GetModuleHandleA(NULL);

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--no-gl")) g_use_gl = 0;
        else if (!strcmp(argv[i], "--opengl")) g_opengl = 1;
        else if (!strcmp(argv[i], "--hostile-state")) g_hostile = 1;
        else if (!strcmp(argv[i], "--no-poke")) g_poke_done = 1;
        else if (!strcmp(argv[i], "--no-children")) g_no_children = 1;
        else if (!strcmp(argv[i], "--late-only")) g_late_only = 1;
        else if (!strcmp(argv[i], "--hide-at") && i + 1 < argc) g_hide_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--resize-at") && i + 3 < argc)
        {
            g_resize_ms = atoi(argv[++i]);
            g_resize_w = atoi(argv[++i]);
            g_resize_h = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pos") && i + 2 < argc) { g_pos_x = atoi(argv[++i]); g_pos_y = atoi(argv[++i]); }
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "GLCHILD_MAIN";
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = child_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "GLCHILD_CHILD";
    RegisterClassA(&wc);

    g_main = CreateWindowExA(0, "GLCHILD_MAIN", "GLCHILD TEST", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             g_pos_x, g_pos_y, 816, 639, NULL, NULL, inst, NULL);
    if (!g_main) { printf("RESULT window=FAILED err=%lu\n", GetLastError()); return 1; }
    SetWindowPos(g_main, HWND_TOP, g_pos_x, g_pos_y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(g_main, 1, 250, NULL);
    printf("STATE window=%p api=%s\n", (void *)g_main, !g_use_gl ? "none" : g_opengl ? "opengl" : "d3d9");
    fflush(stdout);

    if (!g_late_only && !g_no_children)
    {
        g_child1 = CreateWindowExA(0, "GLCHILD_CHILD", "c1", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                   40, 40, 340, 240, g_main, NULL, inst, NULL);
        SetWindowLongPtrA(g_child1, GWLP_USERDATA, 1);
    }

    if (g_use_gl) ok = g_opengl ? create_gl() : create_device();
    printf("RESULT gl=%s\n", !g_use_gl ? "off" : ok ? "on" : "unavailable");

    if (!g_late_only && !g_no_children)
    {
        g_child2 = CreateWindowExA(0, "GLCHILD_CHILD", "c2", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                   400, 40, 340, 240, g_main, NULL, inst, NULL);
        SetWindowLongPtrA(g_child2, GWLP_USERDATA, 2);
    }

    SetTimer(g_child1, 2, 300, NULL);
    SetTimer(g_child2, 3, 300, NULL);
    InvalidateRect(g_child1, NULL, FALSE);
    InvalidateRect(g_child2, NULL, FALSE);
    print_state("created");

    start = GetTickCount();
    while (g_running && GetTickCount() - start < (DWORD)g_seconds * 1000)
    {
        int elapsed;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { g_running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        if (g_dev) draw_d3d9();
        else if (g_glrc) draw_gl();
        g_frames++;
        elapsed = (int)(GetTickCount() - start);

        if (!g_no_children && !g_late_done && elapsed > g_late_ms)
        {
            g_late_done = 1;
            g_child3 = CreateWindowExA(0, "GLCHILD_CHILD", "c3", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                       40, 330, 700, 180, g_main, NULL, inst, NULL);
            SetWindowLongPtrA(g_child3, GWLP_USERDATA, 3);
            SetTimer(g_child3, 4, 300, NULL);
            printf("STATE late-child created at %d ms\n", elapsed);
            fflush(stdout);
        }
        if (!g_no_children && !g_poke_done && elapsed > g_poke_ms && g_child1)
        {
            RECT r;
            g_poke_done = 1;
            GetWindowRect(g_child1, &r);
            MapWindowPoints(NULL, g_main, (POINT *)&r, 2);
            SetWindowPos(g_child1, 0, r.left + 2, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(g_child1, 0, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(g_child1, NULL, FALSE);
            InvalidateRect(g_child2, NULL, FALSE);
            printf("STATE poked children at %d ms\n", elapsed);
            fflush(stdout);
        }
        if (g_resize_ms >= 0 && !g_resize_done && elapsed > g_resize_ms)
        {
            g_resize_done = 1;
            SetWindowPos(g_main, 0, 0, 0, g_resize_w, g_resize_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            printf("STATE resized to %dx%d at %d ms\n", g_resize_w, g_resize_h, elapsed);
            fflush(stdout);
        }
        if (g_hide_ms >= 0 && !g_hide_done && elapsed > g_hide_ms)
        {
            g_hide_done = 1;
            hide_children();
            printf("STATE children hidden at %d ms\n", elapsed);
            fflush(stdout);
        }

        if (GetTickCount() - last_stats > 2000)
        {
            last_stats = GetTickCount();
            print_state("running");
        }
        InvalidateRect(g_child1, NULL, FALSE);
        InvalidateRect(g_child2, NULL, FALSE);
        Sleep(30);
    }

    print_state("final");
    if (g_glrc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(g_glrc); }
    if (g_child3) DestroyWindow(g_child3);
    if (g_child1) DestroyWindow(g_child1);
    if (g_child2) DestroyWindow(g_child2);
    if (g_main) DestroyWindow(g_main);
    return 0;
}
