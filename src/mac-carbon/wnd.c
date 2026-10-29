/* Window management for the Win32 shim.
 *
 * XPico Code creates one top-level window and six child windows (activity bar,
 * sidebar, editor, panel, status bar, palette), each with its own WNDPROC. The
 * Mac has no child windows, so only the top-level HWND owns a real WindowRef;
 * children are lightweight views that live in the same GrafPort and are given
 * their own clipped, origin-shifted HDC at paint time (see mac-carbon/gdi.c).
 *
 * The event loop is the classic WaitNextEvent one rather than the Carbon Event
 * Manager: it is supported by every CarbonLib version on Mac OS 9 as well as by
 * Carbon on OS X, and it maps onto GetMessage/DispatchMessage almost directly. */
#include <Carbon.h>     /* must precede win32.h: see the renames in that header */
#include "win32.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
    HCURSOR   cursor;          /* the class cursor, for WM_SETCURSOR */
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

/* Called once per pass through the event loop; mac-carbon/panel_mac.c installs the
 * terminal poll here so the socket is drained without blocking the editor. */
static void (*s_idleProc)(void);
void shim_set_idle(void (*fn)(void)) { s_idleProc = fn; }

/* a small message queue, for PostMessage and the synthesised WM_MOUSELEAVE */
static MSG    s_queue[QUEUE_SIZE];
static int    s_qhead, s_qtail;

/* mac-carbon/gdi.c */
void  gdi_init(WindowRef wr);
HDC   gdi_begin_window_paint(HWND w, const RECT *screenRC);
void  gdi_end_window_paint(HDC dc);
void  gdi_caret_tick(void);
void  gdi_caret_owner_gone(HWND w);
void  gdi_set_paint_dc(HDC dc);
void  gdi_paint_caret(HWND w, HDC dc);
int   gdi_have_port(void);

extern int paint_probe_count;

/* mac-carbon/sys.c */
void  menubar_install(HMENU m);
int   menubar_command(long selection);

/* mac-carbon/edit.c */
void  edit_register(void);
void  edit_set_text(HWND w, const char *s);
int   edit_get_text(HWND w, char *buf, int n);
int   edit_is_edit(HWND w);

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

    /* The only system class used is EDIT, for three single-line fields; the
     * shim registers its own on first use. */
    if (!c && strcmp(cls, "EDIT") == 0) {
        c = find_class("XPEdit");
        if (!c) { edit_register(); c = find_class("XPEdit"); }
    }
    if (!c) return NULL;

    win = alloc_win();
    if (!win) return NULL;
    win->proc = c->proc;
    win->cursor = c->cursor;
    strncpy(win->cls, cls, sizeof win->cls - 1);
    if (name) { strncpy(win->text, name, sizeof win->text - 1); }
    win->style = style; win->exstyle = exstyle;
    win->parent = parent; win->menu = menu; win->param = param;
    win->visible = (style & WS_VISIBLE) ? 1 : 0;
    SetRect(&win->rc, x, y, x + w, y + h);

    if (!parent) {
        Rect b, avail;
        if (s_root) { win->used = 0; return NULL; }   /* one top-level only */

        /* The saved size comes from a PC and is far bigger than these screens,
         * so the Mac build starts at a modest size and lets the user resize. */
        if (GetAvailableWindowPositioningBounds(GetMainDevice(), &avail) != noErr) {
            BitMap screen;
            GetQDGlobalsScreenBits(&screen);
            avail = screen.bounds;
            avail.top += 20;
        }
        w = 700; h = 600;
        {
            int maxw = avail.right - avail.left;
            int maxh = avail.bottom - avail.top;
            if (w > maxw) w = maxw;
            if (h > maxh) h = maxh;
        }
        /* Leave room for the title bar: these are content-region coordinates,
         * so starting at avail.top would put the drag bar under the menu bar
         * and the window could not be moved. RepositionWindow fixes it up. */
        x = avail.left + 8;
        y = avail.top + 28;

        SetRect(&win->rc, 0, 0, w, h);
        b.left = (short)x; b.top = (short)y;
        b.right = (short)(x + w); b.bottom = (short)(y + h);
        if (CreateNewWindow(kDocumentWindowClass,
                            kWindowStandardDocumentAttributes | kWindowStandardHandlerAttribute,
                            &b, &win->wr) != noErr) { win->used = 0; return NULL; }
        {
            CFStringRef cf = CFStringCreateWithCString(NULL, name ? name : "",
                                                       kCFStringEncodingMacRoman);
            if (cf) { SetWindowTitleWithCFString(win->wr, cf); CFRelease(cf); }
        }
        /* make sure the whole structure region, title bar included, is on screen */
        RepositionWindow(win->wr, NULL, kWindowCascadeOnMainScreen);
        s_root = win;
        gdi_init(win->wr);
        /* main.c passes its Win32 menu here; install it as the Mac menu bar. */
        if (menu) menubar_install(menu);
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

/* The parent of a child window, and the control id it was created with. The
 * EDIT control needs both to post its EN_CHANGE notification the way Win32
 * does: to the parent, with the id in the low word of wParam. */
HWND shim_parent(HWND w) { return w ? w->parent : NULL; }
HWND shim_root_window(void) { return s_root; }
int  shim_child_id(HWND w) { return w ? (int)(long)w->menu : 0; }

/* a child's rect within its parent, which GetClientRect does not expose */
void shim_child_rect(HWND w, LPRECT r)
{
    if (!w) { SetRectEmpty(r); return; }
    *r = w->rc;
}

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
        if (w->visible) {
            Rect b;
            SelectWindow(w->wr);
#ifdef XPCODE_DIAG
            /* Drawn by the Toolbox, so it shows even when none of the shim's
             * own painting works. Build without XPCODE_DIAG to remove. */
            {
                char msg[512];
                int nchild = 0, i;
                Rect wb;
                for (i = 0; i < MAX_WINDOWS; i++)
                    if (s_wins[i].used && s_wins[i].parent == w) nchild++;
                GetWindowBounds(w->wr, kWindowContentRgn, &wb);
                snprintf(msg, sizeof msg,
                         "port=%s  win=%dx%d  children=%d  rc=%dx%d",
                         gdi_have_port() ? "ok" : "NULL",
                         (int)(wb.right - wb.left), (int)(wb.bottom - wb.top),
                         nchild,
                         (int)(w->rc.right - w->rc.left),
                         (int)(w->rc.bottom - w->rc.top));
                MessageBox(NULL, msg, "XPico diag 1/2", MB_OK);
            }
#endif
            /* The app lays its panels out from WM_SIZE. Nothing has sent one
             * yet, so the first paint would use stale child rects and leave
             * the window half-drawn until the user resized it. */
            GetWindowBounds(w->wr, kWindowContentRgn, &b);
            SetRect(&w->rc, 0, 0, b.right - b.left, b.bottom - b.top);
            SendMessage(w, WM_SIZE, 0, MAKELONG(b.right - b.left, b.bottom - b.top));
#ifdef XPCODE_DIAG
            {
                char msg[512];
                int i, painted;
                char kids[256];
                kids[0] = 0;
                for (i = 0; i < MAX_WINDOWS; i++)
                    if (s_wins[i].used && s_wins[i].parent == w) {
                        char one[64];
                        snprintf(one, sizeof one, "%s %dx%d@%d,%d;",
                                 s_wins[i].cls,
                                 (int)(s_wins[i].rc.right - s_wins[i].rc.left),
                                 (int)(s_wins[i].rc.bottom - s_wins[i].rc.top),
                                 (int)s_wins[i].rc.left, (int)s_wins[i].rc.top);
                        strncat(kids, one, sizeof kids - strlen(kids) - 1);
                    }
                painted = paint_probe_count;
                snprintf(msg, sizeof msg, "after layout: paints=%d\n%s", painted, kids);
                MessageBox(NULL, msg, "XPico diag 2/2", MB_OK);
            }
#endif
        }
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
int paint_probe_count;

/* Paint the tree, skipping anything the update region does not touch.
 *
 * Each panel renders its whole offscreen buffer inside its WM_PAINT and blits
 * it, so painting one that is not invalid is pure waste -- and the editor's
 * buffer is the size of the window. Without this test every caret blink and
 * every step of a scrollbar drag re-rendered all five panels, which is what
 * made dragging crawl. BeginUpdate clips what reaches the screen, but it
 * cannot stop the work from happening. */
static void paint_tree(HWND w, RgnHandle clip)
{
    int i;
    RECT sr;
    if (!w->used || !w->visible) return;
    win_screen_rect(w, &sr);
    if (clip) {
        Rect r;
        r.left = (short)sr.left; r.top = (short)sr.top;
        r.right = (short)sr.right; r.bottom = (short)sr.bottom;
        /* children live inside the parent, so skipping it skips the subtree */
        if (!RectInRgn(&r, clip)) return;
    }
    if (sr.right > sr.left && sr.bottom > sr.top) {
        HDC dc = gdi_begin_window_paint(w, &sr);
        if (dc) {
            /* BeginPaint inside the proc hands back this DC. */
            gdi_set_paint_dc(dc);
            paint_probe_count++;
            w->proc(w, WM_PAINT, 0, 0);
            /* the text cursor goes on top of whatever the panel just blitted */
            gdi_paint_caret(w, dc);
            gdi_set_paint_dc(NULL);
            gdi_end_window_paint(dc);
        }
        w->dirty = 0;
    }
    for (i = 0; i < MAX_WINDOWS; i++)
        if (s_wins[i].used && s_wins[i].parent == w) paint_tree(&s_wins[i], clip);
}

void UpdateWindow(HWND w)
{
    if (w) paint_tree(w, NULL);
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
    if (w->wr) {
        CFStringRef cf = CFStringCreateWithCString(NULL, w->text, kCFStringEncodingMacRoman);
        if (cf) { SetWindowTitleWithCFString(w->wr, cf); CFRelease(cf); }
        return;
    }
    /* an EDIT field keeps its own buffer, so keep that in step */
    if (edit_is_edit(w)) edit_set_text(w, w->text);
    InvalidateRect(w, NULL, TRUE);
}

int GetWindowText(HWND w, LPSTR buf, int n)
{
    if (!w || n <= 0) return 0;
    if (edit_is_edit(w)) return edit_get_text(w, buf, n);
    strncpy(buf, w->text, n - 1);
    buf[n - 1] = 0;
    return (int)strlen(buf);
}

/* The three EDIT fields are subclassed: the app swaps in its own proc and
 * chains to the original through CallWindowProc. */
LONG_PTR SetWindowLongPtr(HWND w, int index, LONG_PTR v)
{
    WNDPROC old;
    if (!w || !w->used || index != GWLP_WNDPROC) return 0;
    old = w->proc;
    w->proc = (WNDPROC)v;
    return (LONG_PTR)old;
}

LRESULT CallWindowProc(WNDPROC proc, HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (!proc) return DefWindowProc(w, m, wp, lp);
    return proc(w, m, wp, lp);
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

/* Ask for the cursor the way Win32 does.
 *
 * WM_SETCURSOR goes to the window under the pointer -- or to the window with
 * capture, so that a splitter drag keeps its resize cursor once the pointer
 * has slid onto a panel. A window that does not claim the message leaves it to
 * DefWindowProc, which hands it to the parent; the editor claims it for the
 * I-beam, the main window for the two splitters, and the sidebar and palette
 * not at all. When nobody claims it, Win32 falls back to the class cursor of
 * the window under the pointer.
 *
 * Nothing here sent this message at all, so the app was never asked and the
 * cursor never changed: no I-beam over the text, and no resize cursor on the
 * splitters. */
static void apply_cursor(HWND under)
{
    HWND target = s_capture ? s_capture : under;
    HWND p;
    if (!target) return;
    for (p = target; p; p = p->parent)
        if (SendMessage(p, WM_SETCURSOR, (WPARAM)target, MAKELONG(HTCLIENT, 0))) return;
    SetCursor(target->cursor ? target->cursor : LoadCursor(NULL, IDC_ARROW));
}

/* Classic Mac OS has no mouse-moved event: you poll. Without this the app
 * never saw WM_MOUSEMOVE at all, so nothing that tracks the pointer worked --
 * dragging either scrollbar, dragging the splitters or the minimap, selecting
 * text by dragging, and every hover highlight. */
static Point s_lastMouse = { -32768, -32768 };

static void track_mouse_leave(Point pt)
{
    HWND over = hit_test(s_root, pt.h, pt.v);
    if (over != s_mouseIn) {
        if (s_mouseIn && s_mouseIn->used) SendMessage(s_mouseIn, WM_MOUSELEAVE, 0, 0);
        s_mouseIn = over;
    }
}

static void poll_mouse(void)
{
    Point pt;
    HWND target;
    int ox, oy;

    if (!s_root || !s_root->wr) return;
    GetGlobalMouse(&pt);
    local_mouse(&pt);
    if (pt.h == s_lastMouse.h && pt.v == s_lastMouse.v) return;
    s_lastMouse = pt;

    track_mouse_leave(pt);

    /* While a window has capture the moves belong to it even once the pointer
     * has left, which is what makes a drag keep working past the edge. */
    target = s_capture ? s_capture : hit_test(s_root, pt.h, pt.v);
    if (!target) return;
    win_origin(target, &ox, &oy);
    SendMessage(target, WM_MOUSEMOVE, Button() ? MK_LBUTTON : 0,
                MAKELONG(pt.h - ox, pt.v - oy));
    apply_cursor(s_mouseIn);
}

BOOL GetMessage(LPMSG m, HWND w, UINT lo, UINT hi)
{
    EventRecord ev;
    (void)w; (void)lo; (void)hi;

    for (;;) {
        if (s_quit) return FALSE;

        /* posted messages first, as Win32's queue does */
        if (queue_pop(m)) return TRUE;

        /* The Standard Window Handler resizes and zooms the window behind our
         * back, so pick the new content size up and let the app re-lay out. */
        if (s_root && s_root->wr) {
            Rect b;
            GetWindowBounds(s_root->wr, kWindowContentRgn, &b);
            if (b.right - b.left != s_root->rc.right - s_root->rc.left ||
                b.bottom - b.top != s_root->rc.bottom - s_root->rc.top) {
                int cw = b.right - b.left, ch = b.bottom - b.top;
                SetRect(&s_root->rc, 0, 0, cw, ch);
                SendMessage(s_root, WM_SIZE, 0, MAKELONG(cw, ch));
                InvalidateRect(s_root, NULL, TRUE);
            }
        }

        /* anything invalidated gets drawn before we block for input */
        if (s_root && s_root->wr) {
            RgnHandle rgn = NewRgn();
            GetWindowRegion(s_root->wr, kWindowUpdateRgn, rgn);
            if (!EmptyRgn(rgn)) {
                BeginUpdate(s_root->wr);
                paint_tree(s_root, rgn);
                EndUpdate(s_root->wr);
            }
            DisposeRgn(rgn);
        }

        poll_mouse();
        gdi_caret_tick();
        /* the terminal transport polls its socket from here */
        if (s_idleProc) s_idleProc();

        if (s_timerWnd && TickCount() >= s_nextTimer) {
            s_nextTimer = TickCount() + (s_timerMS * 60) / 1000;
            SendMessage(s_timerWnd, WM_TIMER, 1, 0);
            continue;
        }

        /* Sleeping 2 ticks a pass caps mouse polling at ~30 Hz, which a drag
         * feels as lag. While a button is down, give up the processor without
         * waiting so the pointer is sampled as fast as the repaint allows. */
        if (!WaitNextEvent(everyEvent, &ev, Button() ? 0 : 2, NULL)) continue;

        switch (ev.what) {
        case mouseDown: {
            WindowRef hitw;
            short part = FindWindow(ev.where, &hitw);
            Point pt = ev.where;
            if (part == inMenuBar) {
                long sel = MenuSelect(ev.where);
                int cmd = menubar_command(sel);
                HiliteMenu(0);
                if (cmd) SendMessage(s_root, WM_COMMAND, (WPARAM)cmd, 0);
                continue;
            }
            if (hitw != (s_root ? s_root->wr : NULL)) continue;
            if (part == inDrag) { DragWindow(hitw, ev.where, NULL); continue; }
            if (part == inGoAway) {
                if (TrackGoAway(hitw, ev.where)) SendMessage(s_root, WM_CLOSE, 0, 0);
                continue;
            }
            /* Growing and zooming are left to the Standard Window Handler;
             * the resulting size change arrives as an update event. */
            if (part == inGrow || part == inZoomIn || part == inZoomOut) continue;

            /* Clicking an inactive window brings it forward and is not passed
             * on, which is what every other Mac application does. */
            if (hitw != FrontWindow()) {
                SelectWindow(hitw);
                InvalidateRect(s_root, NULL, TRUE);
                continue;
            }

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
                RgnHandle rgn = NewRgn();
                GetWindowRegion(s_root->wr, kWindowUpdateRgn, rgn);
                BeginUpdate(s_root->wr);
                paint_tree(s_root, rgn);
                EndUpdate(s_root->wr);
                DisposeRgn(rgn);
            }
            continue;
        case activateEvt: {
            int active = (ev.modifiers & activeFlag) ? 1 : 0;
            SendMessage(s_root, WM_ACTIVATEAPP, (WPARAM)active, 0);
            /* The selection and focus rings differ between the active and
             * inactive states, so the whole window is redrawn either way. */
            InvalidateRect(s_root, NULL, TRUE);
            if (s_focus) SendMessage(s_focus, active ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
            continue;
        }

        case osEvt:
            /* Suspend and resume arrive here rather than as activate events
             * when the app is switched away from and back to. */
            if (((ev.message >> 24) & 0xFF) == suspendResumeMessage) {
                int resumed = (ev.message & resumeFlag) ? 1 : 0;
                SendMessage(s_root, WM_ACTIVATEAPP, (WPARAM)resumed, 0);
                if (resumed) InvalidateRect(s_root, NULL, TRUE);
                if (s_focus) SendMessage(s_focus, resumed ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
            }
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
