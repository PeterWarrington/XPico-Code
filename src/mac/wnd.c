/* Window management for the Win32 shim.
 *
 * XPico Code creates one top-level window and six child windows (activity bar,
 * sidebar, editor, panel, status bar, palette), each with its own WNDPROC. The
 * Mac has no child windows, so only the top-level HWND owns a real WindowRef;
 * children are lightweight views that live in the same GrafPort and are given
 * their own clipped, origin-shifted HDC at paint time (see mac/gdi.c).
 *
 * The event loop is the classic WaitNextEvent one rather than the Carbon Event
 * Manager: it is supported by every CarbonLib version on Mac OS 9 as well as by
 * Carbon on OS X, and it maps onto GetMessage/DispatchMessage almost directly. */
#include <Carbon.h>     /* must precede win32.h: see the renames in that header */
#include "win32.h"
#include <string.h>
#include <stdlib.h>

#define MAX_CLASSES 16
#define MAX_WINDOWS 32
#define QUEUE_SIZE  64

typedef struct { char name[32]; WNDPROC proc; HCURSOR cursor; } Class;

struct Win {
    WNDPROC   proc;
    char      cls[32];
    char      text[256];
    RECT      rc;              /* relative to the parent's client area */
    DWORD     style, exstyle;
    HWND      parent;
    HMENU     menu;
    int       used, visible, dirty;
    WindowRef wr;              /* top-level only */
    LPVOID    param;
};

static Class  s_classes[MAX_CLASSES];
static int    s_nclasses;
static struct Win s_wins[MAX_WINDOWS];
static HWND   s_root;          /* the one real window */
static HWND   s_focus, s_capture, s_mouseIn;
static int    s_quit, s_quitCode;
static UINT   s_timerMS;
static HWND   s_timerWnd;
static DWORD  s_nextTimer;

/* a small message queue, for PostMessage and the synthesised WM_MOUSELEAVE */
static MSG    s_queue[QUEUE_SIZE];
static int    s_qhead, s_qtail;

/* mac/gdi.c */
void  gdi_init(WindowRef wr);
HDC   gdi_begin_window_paint(HWND w, const RECT *screenRC);
void  gdi_end_window_paint(HDC dc);
void  gdi_caret_tick(void);
void  gdi_caret_owner_gone(HWND w);
void  gdi_set_paint_dc(HDC dc);

/* ---------------------------------------------------------------- helpers */

static Class *find_class(LPCSTR name)
{
    int i;
    for (i = 0; i < s_nclasses; i++)
        if (strcmp(s_classes[i].name, name) == 0) return &s_classes[i];
    return NULL;
}

static HWND alloc_win(void)
{
    int i;
    for (i = 0; i < MAX_WINDOWS; i++)
        if (!s_wins[i].used) { memset(&s_wins[i], 0, sizeof s_wins[i]); s_wins[i].used = 1; return &s_wins[i]; }
    return NULL;
}

/* client-area origin of w in the coordinates of the real window */
static void win_origin(HWND w, int *ox, int *oy)
{
    int x = 0, y = 0;
    while (w && w->parent) { x += w->rc.left; y += w->rc.top; w = w->parent; }
    *ox = x; *oy = y;
}

/* w's rect in the coordinates of the real window */
static void win_screen_rect(HWND w, RECT *r)
{
    int ox, oy;
    win_origin(w, &ox, &oy);
    r->left = ox; r->top = oy;
    r->right = ox + (w->rc.right - w->rc.left);
    r->bottom = oy + (w->rc.bottom - w->rc.top);
}

static int win_visible_chain(HWND w)
{
    for (; w; w = w->parent) if (!w->visible) return 0;
    return 1;
}

/* deepest visible child of parent containing the window-relative point */
static HWND hit_test(HWND parent, int x, int y)
{
    int i;
    HWND best = parent;
    for (i = 0; i < MAX_WINDOWS; i++) {
        HWND c = &s_wins[i];
        RECT r;
        POINT p;
        if (!c->used || c->parent != parent || !c->visible) continue;
        win_screen_rect(c, &r);
        p.x = x; p.y = y;
        if (PtInRect(&r, p)) best = hit_test(c, x, y);
    }
    return best;
}

static void queue_push(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    int next = (s_qtail + 1) % QUEUE_SIZE;
    if (next == s_qhead) return;             /* full: drop, as Win32 would coalesce */
    s_queue[s_qtail].hwnd = w; s_queue[s_qtail].message = m;
    s_queue[s_qtail].wParam = wp; s_queue[s_qtail].lParam = lp;
    s_qtail = next;
}

static int queue_pop(MSG *m)
{
    if (s_qhead == s_qtail) return 0;
    *m = s_queue[s_qhead];
    s_qhead = (s_qhead + 1) % QUEUE_SIZE;
    return 1;
}

/* ---------------------------------------------------------------- classes */

BOOL RegisterClass(const WNDCLASS *wc)
{
    Class *c;
    if (s_nclasses >= MAX_CLASSES || !wc->lpszClassName) return FALSE;
    c = &s_classes[s_nclasses++];
    strncpy(c->name, wc->lpszClassName, sizeof c->name - 1);
    c->name[sizeof c->name - 1] = 0;
    c->proc = wc->lpfnWndProc;
    c->cursor = wc->hCursor;
    return TRUE;
}

HWND CreateWindowEx(DWORD exstyle, LPCSTR cls, LPCSTR name, DWORD style,
                    int x, int y, int w, int h, HWND parent, HMENU menu,
                    HINSTANCE inst, LPVOID param)
{
    Class *c = find_class(cls);
    HWND win;
    (void)inst;

    /* The only system class used is EDIT, for three single-line fields. */
    if (!c && strcmp(cls, "EDIT") == 0) c = find_class("XPEdit");
    if (!c) return NULL;

    win = alloc_win();
    if (!win) return NULL;
    win->proc = c->proc;
    strncpy(win->cls, cls, sizeof win->cls - 1);
    if (name) { strncpy(win->text, name, sizeof win->text - 1); }
    win->style = style; win->exstyle = exstyle;
    win->parent = parent; win->menu = menu; win->param = param;
    win->visible = (style & WS_VISIBLE) ? 1 : 0;
    SetRect(&win->rc, x, y, x + w, y + h);

    if (!parent) {
        Rect b;
        if (s_root) { win->used = 0; return NULL; }   /* one top-level only */
        SetRect(&win->rc, 0, 0, w, h);
        b.left = (short)x; b.top = (short)y;
        b.right = (short)(x + w); b.bottom = (short)(y + h);
        if (CreateNewWindow(kDocumentWindowClass,
                            kWindowStandardDocumentAttributes | kWindowStandardHandlerAttribute,
                            &b, &win->wr) != noErr) { win->used = 0; return NULL; }
        SetWindowTitleWithCFString(win->wr,
            CFStringCreateWithCString(NULL, name ? name : "", kCFStringEncodingMacRoman));
        s_root = win;
        gdi_init(win->wr);
    }

    SendMessage(win, WM_CREATE, 0, (LPARAM)param);
    if (win->visible) InvalidateRect(win, NULL, TRUE);
    return win;
}

void DestroyWindow(HWND w)
{
    int i;
    if (!w || !w->used) return;
    for (i = 0; i < MAX_WINDOWS; i++)
        if (s_wins[i].used && s_wins[i].parent == w) DestroyWindow(&s_wins[i]);
    SendMessage(w, WM_DESTROY, 0, 0);
    gdi_caret_owner_gone(w);
    if (s_focus == w) s_focus = NULL;
    if (s_capture == w) s_capture = NULL;
    if (s_mouseIn == w) s_mouseIn = NULL;
    if (w->wr) { DisposeWindow(w->wr); s_root = NULL; }
    w->used = 0;
}

BOOL IsWindow(HWND w)        { return w && w->used; }
BOOL IsWindowVisible(HWND w) { return w && w->used && win_visible_chain(w); }

/* ---------------------------------------------------------------- geometry */

void GetClientRect(HWND w, LPRECT r)
{
    if (!w) { SetRectEmpty(r); return; }
    SetRect(r, 0, 0, w->rc.right - w->rc.left, w->rc.bottom - w->rc.top);
}

void GetWindowRect(HWND w, LPRECT r)
{
    if (!w) { SetRectEmpty(r); return; }
    win_screen_rect(w, r);
    if (s_root && s_root->wr) {
        Rect b;
        GetWindowBounds(s_root->wr, kWindowContentRgn, &b);
        r->left += b.left; r->right += b.left;
        r->top += b.top; r->bottom += b.top;
    }
}

void MoveWindow(HWND w, int x, int y, int cw, int ch, BOOL repaint)
{
    int ow, oh;
    if (!w) return;
    ow = w->rc.right - w->rc.left; oh = w->rc.bottom - w->rc.top;
    SetRect(&w->rc, x, y, x + cw, y + ch);
    if (w->wr) {
        Rect b;
        GetWindowBounds(w->wr, kWindowContentRgn, &b);
        b.right = (short)(b.left + cw); b.bottom = (short)(b.top + ch);
        SetWindowBounds(w->wr, kWindowContentRgn, &b);
    }
    if (cw != ow || ch != oh) SendMessage(w, WM_SIZE, 0, MAKELONG(cw, ch));
    if (repaint) InvalidateRect(w, NULL, TRUE);
}

void ShowWindow(HWND w, int cmd)
{
    if (!w) return;
    w->visible = cmd ? 1 : 0;
    /* ShowHide, not Carbon's ShowWindow: that name is taken by the shim. */
    if (w->wr) {
        ShowHide(w->wr, w->visible ? true : false);
        if (w->visible) SelectWindow(w->wr);
    }
    InvalidateRect(w->parent ? w->parent : w, NULL, TRUE);
}

void SetRect(LPRECT r, int l, int t, int rt, int b)
{
    r->left = l; r->top = t; r->right = rt; r->bottom = b;
}

void SetRectEmpty(LPRECT r) { r->left = r->top = r->right = r->bottom = 0; }

BOOL IsRectEmpty(const RECT *r)
{
    return r->right <= r->left || r->bottom <= r->top;
}

void ClientToScreen(HWND w, LPPOINT p)
{
    RECT r;
    GetWindowRect(w, &r);
    p->x += r.left; p->y += r.top;
}

void ScreenToClient(HWND w, LPPOINT p)
{
    RECT r;
    GetWindowRect(w, &r);
    p->x -= r.left; p->y -= r.top;
}

void GetCursorPos(LPPOINT p)
{
    Point pt;
    GetGlobalMouse(&pt);
    p->x = pt.h; p->y = pt.v;
}

BOOL PtInRect(const RECT *r, POINT p)
{
    return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom;
}

/* ---------------------------------------------------------------- painting */

void InvalidateRect(HWND w, const RECT *r, BOOL erase)
{
    RECT sr;
    Rect mr;
    (void)erase;
    if (!w || !w->used) return;
    w->dirty = 1;
    if (!s_root || !s_root->wr) return;
    win_screen_rect(w, &sr);
    if (r) {
        int ox, oy;
        win_origin(w, &ox, &oy);
        sr.left = ox + r->left; sr.top = oy + r->top;
        sr.right = ox + r->right; sr.bottom = oy + r->bottom;
    }
    mr.left = (short)sr.left; mr.top = (short)sr.top;
    mr.right = (short)sr.right; mr.bottom = (short)sr.bottom;
    InvalWindowRect(s_root->wr, &mr);
}

/* Paint every visible window that overlaps the window's update region, parents
 * before children, which is the order Win32's WS_CLIPCHILDREN painting gives. */
static void paint_tree(HWND w)
{
    int i;
    RECT sr;
    if (!w->used || !w->visible) return;
    win_screen_rect(w, &sr);
    if (sr.right > sr.left && sr.bottom > sr.top) {
        HDC dc = gdi_begin_window_paint(w, &sr);
        if (dc) {
            /* BeginPaint inside the proc hands back this DC. */
            gdi_set_paint_dc(dc);
            w->proc(w, WM_PAINT, 0, 0);
            gdi_set_paint_dc(NULL);
            gdi_end_window_paint(dc);
        }
        w->dirty = 0;
    }
    for (i = 0; i < MAX_WINDOWS; i++)
        if (s_wins[i].used && s_wins[i].parent == w) paint_tree(&s_wins[i]);
}

void UpdateWindow(HWND w)
{
    if (w) paint_tree(w);
}

/* ---------------------------------------------------------------- messages */

LRESULT SendMessage(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (!w || !w->used || !w->proc) return 0;
    return w->proc(w, m, wp, lp);
}

BOOL PostMessage(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    queue_push(w, m, wp, lp);
    return TRUE;
}

LRESULT DefWindowProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    (void)w; (void)wp; (void)lp;
    switch (m) {
    case WM_CLOSE: PostQuitMessage(0); return 0;
    default: return 0;
    }
}

HWND SetFocus(HWND w)
{
    HWND old = s_focus;
    if (old == w) return old;
    if (old) SendMessage(old, WM_KILLFOCUS, (WPARAM)w, 0);
    s_focus = w;
    if (w) SendMessage(w, WM_SETFOCUS, (WPARAM)old, 0);
    return old;
}

HWND GetFocus(void)        { return s_focus; }
void SetCapture(HWND w)    { s_capture = w; }
void ReleaseCapture(void)  { s_capture = NULL; }

BOOL TrackMouseEvent(TRACKMOUSEEVENT *e)
{
    /* WM_MOUSELEAVE is synthesised in the loop; nothing to arm here. */
    (void)e;
    return TRUE;
}

void PostQuitMessage(int code) { s_quit = 1; s_quitCode = code; }

BOOL SetTimer(HWND w, UINT id, UINT ms, LPVOID proc)
{
    (void)id; (void)proc;
    s_timerWnd = w; s_timerMS = ms;
    s_nextTimer = TickCount() + (ms * 60) / 1000;
    return TRUE;
}

void SetWindowText(HWND w, LPCSTR s)
{
    if (!w) return;
    strncpy(w->text, s ? s : "", sizeof w->text - 1);
    w->text[sizeof w->text - 1] = 0;
    if (w->wr)
        SetWindowTitleWithCFString(w->wr,
            CFStringCreateWithCString(NULL, w->text, kCFStringEncodingMacRoman));
    else
        InvalidateRect(w, NULL, TRUE);
}

int GetWindowText(HWND w, LPSTR buf, int n)
{
    int len;
    if (!w || n <= 0) return 0;
    strncpy(buf, w->text, n - 1);
    buf[n - 1] = 0;
    len = (int)strlen(buf);
    return len;
}

short GetKeyState(int vk)
{
    KeyMap km;
    UInt32 mods;
    GetKeys(km);
    mods = GetCurrentKeyModifiers();
    switch (vk) {
    /* Ctrl in the source means "the command modifier" on the Mac, which is
     * where the editor's shortcuts live; the real control key is left alone. */
    case VK_CONTROL: return (mods & cmdKey) ? (short)0x8000 : 0;
    case VK_SHIFT:   return (mods & shiftKey) ? (short)0x8000 : 0;
    case VK_MENU:    return (mods & optionKey) ? (short)0x8000 : 0;
    default:         return 0;
    }
}

/* ---------------------------------------------------- event translation */

static int mac_key_to_vk(char ch, UInt32 code)
{
    switch (code) {
    case 0x7B: return VK_LEFT;
    case 0x7C: return VK_RIGHT;
    case 0x7E: return VK_UP;
    case 0x7D: return VK_DOWN;
    case 0x73: return VK_HOME;
    case 0x77: return VK_END;
    case 0x74: return VK_PRIOR;
    case 0x79: return VK_NEXT;
    case 0x75: return VK_DELETE;
    case 0x72: return VK_INSERT;
    case 0x7A: return VK_F1;
    case 0x78: return VK_F2;
    case 0x63: return VK_F3;
    case 0x76: return VK_F4;
    case 0x60: return VK_F5;
    case 0x61: return VK_F6;
    case 0x62: return VK_F7;
    case 0x64: return VK_F8;
    case 0x65: return VK_F9;
    case 0x6D: return VK_F10;
    case 0x67: return VK_F11;
    case 0x6F: return VK_F12;
    default: break;
    }
    switch (ch) {
    case 0x08: case 0x7F: return VK_BACK;
    case 0x09: return VK_TAB;
    case 0x0D: case 0x03: return VK_RETURN;
    case 0x1B: return VK_ESCAPE;
    case ' ':  return VK_SPACE;
    default:
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 'A';
        if (ch >= 'A' && ch <= 'Z') return ch;
        if (ch >= '0' && ch <= '9') return ch;
        return 0;
    }
}

/* window-relative mouse position */
static void local_mouse(Point *pt)
{
    GrafPtr save;
    GetPort(&save);
    if (s_root && s_root->wr) SetPortWindowPort(s_root->wr);
    GlobalToLocal(pt);
    SetPort(save);
}

static void dispatch_mouse(UINT msg, Point pt, WPARAM wp)
{
    HWND target = s_capture ? s_capture : hit_test(s_root, pt.h, pt.v);
    int ox, oy;
    if (!target) return;
    win_origin(target, &ox, &oy);
    SendMessage(target, msg, wp, MAKELONG(pt.h - ox, pt.v - oy));
}

static void track_mouse_leave(Point pt)
{
    HWND over = hit_test(s_root, pt.h, pt.v);
    if (over != s_mouseIn) {
        if (s_mouseIn && s_mouseIn->used) SendMessage(s_mouseIn, WM_MOUSELEAVE, 0, 0);
        s_mouseIn = over;
    }
}

BOOL GetMessage(LPMSG m, HWND w, UINT lo, UINT hi)
{
    EventRecord ev;
    (void)w; (void)lo; (void)hi;

    for (;;) {
        if (s_quit) return FALSE;

        /* posted messages first, as Win32's queue does */
        if (queue_pop(m)) return TRUE;

        /* anything invalidated gets drawn before we block for input */
        if (s_root && s_root->wr) {
            RgnHandle rgn = NewRgn();
            GetWindowRegion(s_root->wr, kWindowUpdateRgn, rgn);
            if (!EmptyRgn(rgn)) {
                BeginUpdate(s_root->wr);
                paint_tree(s_root);
                EndUpdate(s_root->wr);
            }
            DisposeRgn(rgn);
        }

        gdi_caret_tick();

        if (s_timerWnd && TickCount() >= s_nextTimer) {
            s_nextTimer = TickCount() + (s_timerMS * 60) / 1000;
            SendMessage(s_timerWnd, WM_TIMER, 1, 0);
            continue;
        }

        if (!WaitNextEvent(everyEvent, &ev, 2, NULL)) {
            Point pt;
            GetGlobalMouse(&pt);
            local_mouse(&pt);
            track_mouse_leave(pt);
            continue;
        }

        switch (ev.what) {
        case mouseDown: {
            WindowRef hitw;
            short part = FindWindow(ev.where, &hitw);
            Point pt = ev.where;
            if (part == inMenuBar) { MenuSelect(ev.where); continue; }
            if (hitw != (s_root ? s_root->wr : NULL)) continue;
            if (part == inDrag) { DragWindow(hitw, ev.where, NULL); continue; }
            if (part == inGoAway) {
                if (TrackGoAway(hitw, ev.where)) SendMessage(s_root, WM_CLOSE, 0, 0);
                continue;
            }
            /* Growing and zooming are left to the Standard Window Handler;
             * the resulting size change arrives as an update event. */
            if (part == inGrow || part == inZoomIn || part == inZoomOut) continue;
            local_mouse(&pt);
            {
                UINT msg = (ev.modifiers & controlKey) ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
                HWND target = hit_test(s_root, pt.h, pt.v);
                if (target) SetFocus(target);
                dispatch_mouse(msg, pt, 0);
            }
            continue;
        }
        case mouseUp: {
            Point pt = ev.where;
            local_mouse(&pt);
            dispatch_mouse(WM_LBUTTONUP, pt, 0);
            continue;
        }
        case keyDown:
        case autoKey: {
            char ch = (char)(ev.message & charCodeMask);
            UInt32 code = (ev.message & keyCodeMask) >> 8;
            int vk = mac_key_to_vk(ch, code);
            m->hwnd = s_focus ? s_focus : s_root;
            m->message = WM_KEYDOWN;
            m->wParam = (WPARAM)vk;
            m->lParam = (LPARAM)ch;
            return TRUE;
        }
        case updateEvt:
            if ((WindowRef)ev.message == (s_root ? s_root->wr : NULL)) {
                BeginUpdate(s_root->wr);
                paint_tree(s_root);
                EndUpdate(s_root->wr);
            }
            continue;
        case activateEvt:
            SendMessage(s_root, WM_ACTIVATEAPP, (ev.modifiers & activeFlag) ? 1 : 0, 0);
            continue;
        default:
            continue;
        }
    }
}

/* TranslateMessage turns a key-down into the WM_CHAR that the editor expects
 * for printable input; the character itself rode along in lParam. */
BOOL TranslateMessage(const MSG *m)
{
    if (m->message == WM_KEYDOWN) {
        char ch = (char)m->lParam;
        if ((unsigned char)ch >= 32 && (unsigned char)ch != 127 &&
            !(GetKeyState(VK_CONTROL) & 0x8000))
            queue_push(m->hwnd, WM_CHAR, (WPARAM)(unsigned char)ch, 0);
        else if (ch == '\r' || ch == '\t' || ch == 8 || ch == 127)
            queue_push(m->hwnd, WM_CHAR, (WPARAM)(unsigned char)(ch == 127 ? 8 : ch), 0);
    }
    return TRUE;
}

LRESULT DispatchMessage(const MSG *m)
{
    return SendMessage(m->hwnd, m->message, m->wParam, m->lParam);
}
