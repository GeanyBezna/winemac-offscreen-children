/*
 * glchild_overlay.c — minimal reproduction for ticket 001 / T-23.
 *
 * Creates a top-level window that may render with D3D9 (wined3d -> OpenGL) and two
 * GDI child windows (one created before the D3D device, one after it). The child
 * windows paint solid colours and a live counter, so a window-only screenshot tells
 * us whether GDI child windows are visible above the game-style GL surface under
 * the winemac driver, and whether the children keep painting.
 *
 * Build (32-bit, like Metin2):
 *   i686-w64-mingw32-gcc -O2 -o build/glchild_overlay32.exe glchild_overlay.c -ld3d9 -lgdi32 -luser32
 *
 * Options: --no-gl            no D3D device (control run: children must be visible)
 *          --seconds N        auto-exit after N seconds (default 25)
 *          --pos X Y          window position (default 2100 100)
 * Prints machine-readable status lines to stdout: "STATE ...", "CHILD1_PAINTS n", ...
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static HWND g_main, g_child1, g_child2, g_child3;
static int g_running = 1, g_use_gl = 1, g_seconds = 25, g_pos_x = 2100, g_pos_y = 100;
static volatile LONG g_paints1, g_paints2, g_paints3;
static unsigned g_frames;
static int g_late_ms = 2500, g_poke_ms = 3500;
static int g_late_done, g_poke_done;
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
    RECT r;
    POINT pt = { 0, 0 };
    GetWindowRect(g_main, &r);
    printf("STATE %s main=%p child1=%p child2=%p child3=%p frames=%u paints1=%ld paints2=%ld paints3=%ld "
           "win=(%ld,%ld)-(%ld,%ld)\n",
           tag, (void *)g_main, (void *)g_child1, (void *)g_child2, (void *)g_child3, g_frames,
           (LONG)g_paints1, (LONG)g_paints2, (LONG)g_paints3, r.left, r.top, r.right, r.bottom);
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
    pp.BackBufferWidth = rc.right - rc.left;
    pp.BackBufferHeight = rc.bottom - rc.top;
    pp.hDeviceWindow = g_main;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.EnableAutoDepthStencil = FALSE;

    hr = g_d3d->lpVtbl->CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_main,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev);
    printf("RESULT create_device=0x%08lx\n", (unsigned long)hr);
    fflush(stdout);
    return SUCCEEDED(hr);
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    MSG msg;
    DWORD start, last_stats = 0;
    int i;
    HINSTANCE inst = GetModuleHandleA(NULL);

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--no-gl")) g_use_gl = 0;
        else if (!strcmp(argv[i], "--no-poke")) g_poke_done = 1;
        else if (!strcmp(argv[i], "--no-children")) g_no_children = 1;
        else if (!strcmp(argv[i], "--late-only")) g_late_only = 1;
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
    wc.lpszMenuName = NULL;
    RegisterClassA(&wc);

    g_main = CreateWindowExA(0, "GLCHILD_MAIN", "GLCHILD TEST", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             g_pos_x, g_pos_y, 816, 639, NULL, NULL, inst, NULL);
    if (!g_main) { printf("RESULT window=FAILED err=%lu\n", GetLastError()); return 1; }
    SetWindowPos(g_main, HWND_TOP, g_pos_x, g_pos_y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(g_main, 1, 250, NULL);
    printf("STATE window=%p\n", (void *)g_main);
    fflush(stdout);

    if (!g_late_only && !g_no_children)
    {
        g_child1 = CreateWindowExA(0, "GLCHILD_CHILD", "c1", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                   40, 40, 340, 240, g_main, NULL, inst, NULL);
        SetWindowLongPtrA(g_child1, GWLP_USERDATA, 1);
    }

    if (g_use_gl && !create_device()) { printf("RESULT gl=unavailable\n"); }
    else printf("RESULT gl=%s\n", g_use_gl ? "on" : "off");

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
    fflush(stdout);

    start = GetTickCount();
    while (g_running && GetTickCount() - start < (DWORD)g_seconds * 1000)
    {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { g_running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        if (g_dev)
        {
            DWORD t = GetTickCount();
            D3DCOLOR color = D3DCOLOR_XRGB(20 + (t / 7) % 80, 90 + (t / 11) % 100, 40 + (t / 13) % 120);
            if (SUCCEEDED(g_dev->lpVtbl->Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, color, 1.0f, 0)))
            {
                g_dev->lpVtbl->BeginScene(g_dev);
                g_dev->lpVtbl->EndScene(g_dev);
                g_dev->lpVtbl->Present(g_dev, NULL, NULL, NULL, NULL);
            }
        }
        g_frames++;

        if (!g_no_children && !g_late_done && (int)(GetTickCount() - start) > g_late_ms)
        {
            g_late_done = 1;
            g_child3 = CreateWindowExA(0, "GLCHILD_CHILD", "c3", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                       40, 330, 700, 180, g_main, NULL, inst, NULL);
            SetWindowLongPtrA(g_child3, GWLP_USERDATA, 3);
            SetTimer(g_child3, 4, 300, NULL);
            printf("STATE late-child created at %lu ms\n", (unsigned long)(GetTickCount() - start));
            fflush(stdout);
        }
        if (!g_no_children && !g_poke_done && (int)(GetTickCount() - start) > g_poke_ms)
        {
            RECT r;
            g_poke_done = 1;
            GetWindowRect(g_child1, &r);
            SetWindowPos(g_child1, 0, r.left + 2, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(g_child1, 0, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(g_child1, NULL, FALSE);
            InvalidateRect(g_child2, NULL, FALSE);
            printf("STATE poked children at %lu ms\n", (unsigned long)(GetTickCount() - start));
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
    if (g_child3) DestroyWindow(g_child3);
    if (g_child1) DestroyWindow(g_child1);
    if (g_child2) DestroyWindow(g_child2);
    if (g_main) DestroyWindow(g_main);
    return 0;
}
