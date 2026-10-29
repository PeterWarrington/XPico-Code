/* win95.c - what Windows 95 does not export, done another way.
 *
 * Only the Windows 95 build compiles this file (build.bat win95, which also
 * defines WIN95); the Windows 2000 and up build calls the API directly. Every
 * function here stands in for one name that a stock Windows 95 user32,
 * kernel32, shell32 or comctl32 does not have -- and a name it does not have
 * is not a call that fails, it is a program that will not load at all, so the
 * gating has to keep them out of the import table rather than guard them at
 * run time. The declarations are at the foot of the Windows section of
 * xpcode.h, next to the switches that route the call sites here.
 *
 * Which Windows 95 this is written for: the first one, 4.00.950, with the
 * shell and common controls it shipped with. Nothing here needs the Internet
 * Explorer 4 shell update, though it will use it when it is installed. */
#include "xpcode.h"

#ifndef WIN95
#error "win95.c belongs to the Windows 95 build only -- see build.bat win95"
#endif

/* ============================================================ common controls */

void w95_init_common_controls(void)
{
    /* InitCommonControlsEx came with comctl32 4.70, in Internet Explorer 3.
     * The editor draws its own controls and wants nothing from the library
     * but to be loaded with its classes registered, which the original
     * InitCommonControls does. */
    InitCommonControls();
}

/* ============================================================ %APPDATA% */

typedef BOOL (WINAPI *SHGetSpecialFolderPathFn)(HWND, LPSTR, int, BOOL);

/* Where the settings file goes. SHGetFolderPath is shell32 5.0, shipped with
 * Windows 2000; SHGetSpecialFolderPath is 4.71, from the Internet Explorer 4
 * shell update; SHGetSpecialFolderLocation is in every shell32 there has
 * ever been. Try them in that order and let the caller fall back to the
 * program's own folder -- which on a single-user system is no worse. */
int w95_appdata(char *out, int n)
{
    static SHGetSpecialFolderPathFn path;
    static int looked;
    LPITEMIDLIST pidl;
    if (n < MAX_PATH) return 0;
    if (!looked) {
        HMODULE shell = GetModuleHandle("shell32.dll");
        looked = 1;
        if (shell) path = (SHGetSpecialFolderPathFn)GetProcAddress(shell, "SHGetSpecialFolderPathA");
    }
    if (path && path(NULL, out, CSIDL_APPDATA, TRUE)) return 1;
    if (SHGetSpecialFolderLocation(NULL, CSIDL_APPDATA, &pidl) == NOERROR) {
        int ok = SHGetPathFromIDList(pidl, out) ? 1 : 0;
        CoTaskMemFree(pidl);
        if (ok) return 1;
    }
    return 0;
}

/* ============================================================ file times */

int w95_file_mtime(const char *path, FILETIME *ft)
{
    /* GetFileAttributesEx is Windows 98 and NT 4. FindFirstFile is as old as
     * Win32 and reports the same write time. */
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    *ft = fd.ftLastWriteTime;
    return 1;
}

/* ============================================================ fonts */

static int CALLBACK font_seen(const LOGFONT *lf, const TEXTMETRIC *tm, DWORD type, LPARAM p)
{
    (void)lf; (void)tm; (void)type;
    *(int *)p = 1;
    return 0;                   /* one is enough */
}

/* The first of these faces that is installed. Windows 95 shipped none of the
 * faces the editor asks for by name -- Tahoma and Trebuchet MS arrived with
 * Internet Explorer 4, which most machines did eventually get -- and a face
 * GDI cannot find is quietly replaced by one that often looks nothing like
 * it, so ask first. The last name in the list is returned whether it is there
 * or not: it is the one that cannot be missing. */
const char *w95_pick_font(const char *const *names)
{
    HDC dc = GetDC(NULL);
    int i;
    for (i = 0; names[i + 1]; i++) {
        int found = 0;
        EnumFontFamilies(dc, names[i], (FONTENUMPROC)font_seen, (LPARAM)&found);
        if (found) break;
    }
    ReleaseDC(NULL, dc);
    return names[i];
}

/* ============================================================ WM_MOUSELEAVE */

static HWND s_tracked;
static UINT_PTR s_timer;

static void CALLBACK leave_tick(HWND wnd, UINT msg, UINT_PTR id, DWORD now)
{
    POINT p;
    HWND under, left;
    (void)wnd; (void)msg; (void)now;
    if (!s_tracked) { KillTimer(NULL, id); s_timer = 0; return; }
    GetCursorPos(&p);
    under = WindowFromPoint(p);
    if (under == s_tracked || IsChild(s_tracked, under)) return;
    left = s_tracked;
    s_tracked = NULL;
    KillTimer(NULL, id);
    s_timer = 0;
    PostMessage(left, WM_MOUSELEAVE, 0, 0);
}

/* TrackMouseEvent is Windows 98 and NT 4; Windows 95 has only comctl32's
 * _TrackMouseEvent, and only from version 4.70. So one timer watches the
 * cursor instead and posts the WM_MOUSELEAVE the hover highlighting is
 * waiting for. It is a timer with no window behind it, which the message loop
 * dispatches straight to this callback, so none of the five windows that ask
 * for a leave needs a WM_TIMER case of its own. Only one window is tracked at
 * a time, because only one can be under the pointer; the previous one is told
 * it was left, which is what it would have been told anyway. */
void w95_track_leave(HWND w)
{
    if (s_tracked == w && s_timer) return;
    if (s_tracked && s_tracked != w) PostMessage(s_tracked, WM_MOUSELEAVE, 0, 0);
    s_tracked = w;
    if (!s_timer) s_timer = SetTimer(NULL, 0, 100, (TIMERPROC)leave_tick);
}

/* ============================================================ the mouse wheel */

static UINT s_wheelMsg;

/* Windows 95 has no WM_MOUSEWHEEL. The IntelliMouse driver sends this
 * registered message to the window with the focus instead, carrying the delta
 * in wParam. Turn it into the WM_MOUSEWHEEL that the editor, the side bar,
 * the panel and the palette already handle, aimed at whichever of our windows
 * the pointer is over -- which is where Windows 98 and later send it, and the
 * reason those handlers read the position out of lParam.
 *
 * Returns 1 when the message was ours, and app_hotkey then keeps it out of
 * the rest of the loop. WM_MOUSEWHEEL itself is still handled as well: a
 * Windows 95 machine with a newer driver, or this same build on a later
 * Windows, gets the real message and needs none of this. */
int w95_wheel(MSG *m)
{
    POINT p;
    HWND target, owner;
    int delta;
    if (!s_wheelMsg) s_wheelMsg = RegisterWindowMessage("MSWHEEL_ROLLMSG");
    if (!s_wheelMsg || m->message != s_wheelMsg) return 0;
    delta = (int)(LONG)m->wParam;
    if (!delta) return 1;
    GetCursorPos(&p);
    target = WindowFromPoint(p);
    for (owner = target; owner && owner != g_hwndMain; ) owner = GetParent(owner);
    if (!owner) target = GetFocus();            /* not one of ours: use the focus */
    if (!target) return 1;
    SendMessage(target, WM_MOUSEWHEEL, MAKEWPARAM(0, delta), MAKELPARAM(p.x, p.y));
    return 1;
}

/* ============================================================ OEM text */

typedef BOOL (WINAPI *OemToCharBuffFn)(LPCSTR, LPSTR, DWORD);

/* command.com writes code page 437 and the editor draws Windows 1252, so a
 * line drawing character or an accent would otherwise come out as something
 * else entirely. Resolved rather than imported only because TCC's own
 * user32.def is not guaranteed to carry it; it is in every Windows 95. */
void w95_oem_to_ansi(char *buf, int n)
{
    static OemToCharBuffFn fn;
    static int looked;
    if (!looked) {
        HMODULE user = GetModuleHandle("user32.dll");
        looked = 1;
        if (user) fn = (OemToCharBuffFn)GetProcAddress(user, "OemToCharBuffA");
    }
    if (fn && n > 0) fn(buf, buf, (DWORD)n);
}
