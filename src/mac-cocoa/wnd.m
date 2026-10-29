/* Window management and the event loop for the Win32 shim.
 *
 * XPico Code creates one top-level window and six child windows (activity bar,
 * sidebar, editor, panel, status bar, palette), each with its own WNDPROC. As
 * in the Carbon port, only the top-level HWND owns anything real -- an
 * NSWindow with a single NSView -- and the children are lightweight rectangles
 * drawn into that view, each given its own clipped, origin-shifted HDC at
 * paint time (see mac-cocoa/gdi.m).
 *
 * The app keeps its Win32 message loop, so this file does not call
 * -[NSApplication run]. GetMessage pumps NSEvents itself and hands them to
 * -sendEvent:, which is what keeps the menu bar's key equivalents working: the
 * menu is offered every Cmd-key first, and only what it does not claim reaches
 * the view's -keyDown:. */
#import <Cocoa/Cocoa.h>
#include "win32.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define MAX_CLASSES 16
#define MAX_WINDOWS 32
#define QUEUE_SIZE  128

/* Not "Class": that is the Objective-C runtime's own type for a class object,
 * and <objc/objc.h> has already taken the name. */
typedef struct { char name[32]; WNDPROC proc; HCURSOR cursor; } WinClass;

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
    NSWindow *wr;              /* top-level only */
    LPVOID    param;
};

static WinClass s_classes[MAX_CLASSES];
static int    s_nclasses;
static struct Win s_wins[MAX_WINDOWS];
static HWND   s_root;          /* the one real window */
static HWND   s_focus, s_capture, s_mouseIn;
static int    s_quit, s_quitCode;
static UINT   s_timerMS;
static HWND   s_timerWnd;
static DWORD  s_nextTimer;
static NSView *s_view;         /* the root window's content view */
static unsigned int s_mods;    /* modifiers from the last event seen */
static int    s_buttonDown;

/* Called once per pass through the event loop; mac-cocoa/panel_cocoa.c installs
 * the terminal poll here so the pty is drained without blocking the editor. */
static void (*s_idleProc)(void);
void shim_set_idle(void (*fn)(void)) { s_idleProc = fn; }

/* a small message queue, for PostMessage and the keyboard */
static MSG    s_queue[QUEUE_SIZE];
static int    s_qhead, s_qtail;

/* mac-cocoa/gdi.m */
void  gdi_set_window_context(void *ctx, int w, int h, float scale);
HDC   gdi_begin_window_paint(HWND w, const RECT *screenRC);
void  gdi_end_window_paint(HDC dc);
void  gdi_caret_tick(void);
void  gdi_caret_owner_gone(HWND w);
void  gdi_set_paint_dc(HDC dc);
void  gdi_paint_caret(HWND w, HDC dc);
int   gdi_caret_active(void);

/* mac-cocoa/sys.m */
void  menubar_install(HMENU m);
/* the editor's bytes are UTF-8; see the note on shim_string in sys.m */
NSString *shim_string(const char *s, int n);
NSData   *shim_bytes(NSString *s);

/* mac-cocoa/edit.c */
void  edit_register(void);
void  edit_set_text(HWND w, const char *s);
int   edit_get_text(HWND w, char *buf, int n);
int   edit_is_edit(HWND w);

static void paint_tree(HWND w, const RECT *clip);

/* ---------------------------------------------------------------- helpers */

static WinClass *find_class(LPCSTR name)
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
    if (!parent) return NULL;
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

/* ------------------------------------------------- event translation */

/* NSEvent's key codes are the same virtual key codes the Toolbox used, so this
 * table is the Carbon port's unchanged. */
static int mac_key_to_vk(char ch, unsigned short code)
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
    /* The editor binds these through their Win32 OEM names: Ctrl+/ comments a
     * line out, Ctrl+= and Ctrl+- zoom. */
    case '+': case '=':   return VK_OEM_PLUS;
    case '-': case '_':   return VK_OEM_MINUS;
    case '[': case '{':   return VK_OEM_4;
    case ']': case '}':   return VK_OEM_6;
    case '/': case '?':   return VK_OEM_2;
    case '\\': case '|':  return VK_OEM_5;
    case ';': case ':':   return VK_OEM_1;
    case '\'': case '"':  return VK_OEM_7;
    case '`': case '~':   return VK_OEM_3;
    default:
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 'A';
        if (ch >= 'A' && ch <= 'Z') return ch;
        if (ch >= '0' && ch <= '9') return ch;
        return 0;
    }
}

static void dispatch_mouse(UINT msg, int x, int y, WPARAM wp)
{
    HWND target = s_capture ? s_capture : hit_test(s_root, x, y);
    int ox, oy;
    if (!target) return;
    win_origin(target, &ox, &oy);
    SendMessage(target, msg, wp, MAKELONG(x - ox, y - oy));
}

/* Ask for the cursor the way Win32 does.
 *
 * WM_SETCURSOR goes to the window under the pointer -- or to the window with
 * capture, so that a splitter drag keeps its resize cursor once the pointer
 * has slid onto a panel. A window that does not claim the message leaves it to
 * DefWindowProc, which hands it to the parent; the editor claims it for the
 * I-beam, the main window for the two splitters, and the sidebar and palette
 * not at all.
 *
 * The last step is the one whose absence shows: when nobody claims it, Win32
 * falls back to the class cursor of the window under the pointer. Without
 * that, nothing ever puts the arrow back, and whatever the editor or a sash
 * last set stays on screen for the rest of the session. */
static void apply_cursor(HWND under)
{
    HWND target = s_capture ? s_capture : under;
    HWND p;
    if (!target) return;
    for (p = target; p; p = p->parent)
        if (SendMessage(p, WM_SETCURSOR, (WPARAM)target, MAKELONG(HTCLIENT, 0))) return;
    SetCursor(target->cursor ? target->cursor : LoadCursor(NULL, IDC_ARROW));
}

/* Win32 synthesises WM_MOUSELEAVE for whatever the pointer has just left; the
 * hover highlights in every panel depend on it. */
static void track_mouse_leave(int x, int y)
{
    HWND over = hit_test(s_root, x, y);
    if (over != s_mouseIn) {
        if (s_mouseIn && s_mouseIn->used) SendMessage(s_mouseIn, WM_MOUSELEAVE, 0, 0);
        s_mouseIn = over;
    }
}

/* ---------------------------------------------------------------- the view */

@interface XPView : NSView
@end

@implementation XPView

/* AppKit declares these as returning BOOL, which win32.h has turned into a
 * macro for int; "signed char" is the type AppKit actually means. */
- (signed char)isFlipped { return 1; }
- (signed char)acceptsFirstResponder { return 1; }
- (signed char)isOpaque { return 1; }
- (signed char)acceptsFirstMouse:(NSEvent *)ev { (void)ev; return 1; }

/* How many pixels the display puts in a point.
 *
 * -backingScaleFactor arrived in 10.7, five years after this target, and
 * -userSpaceScaleFactor in 10.4 itself, so both are asked for by name at
 * runtime and read through key-value coding -- which boxes the result whatever
 * width CGFloat happens to be, and keeps the selector out of the link. On
 * Tiger neither answers and the scale is 1, which is the truth there. */
static float window_scale(NSWindow *win)
{
    NSNumber *n = nil;
    float scale;
    if (!win) return 1.0f;
    if ([win respondsToSelector:@selector(backingScaleFactor)])
        n = [win valueForKey:@"backingScaleFactor"];
    else if ([win respondsToSelector:@selector(userSpaceScaleFactor)])
        n = [win valueForKey:@"userSpaceScaleFactor"];
    scale = n ? [n floatValue] : 1.0f;
    return scale >= 1.0f ? scale : 1.0f;
}

- (void)drawRect:(NSRect)dirty
{
    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] graphicsPort];
    NSSize size = [self bounds].size;
    RECT clip;
    SetRect(&clip, (int)floor(dirty.origin.x), (int)floor(dirty.origin.y),
            (int)ceil(dirty.origin.x + dirty.size.width),
            (int)ceil(dirty.origin.y + dirty.size.height));
    gdi_set_window_context(ctx, (int)size.width, (int)size.height,
                           window_scale([self window]));
    if (s_root) paint_tree(s_root, &clip);
    gdi_set_window_context(NULL, 0, 0, 1.0f);
}

- (void)viewPoint:(NSEvent *)ev x:(int *)x y:(int *)y
{
    NSPoint p = [self convertPoint:[ev locationInWindow] fromView:nil];
    *x = (int)floor(p.x);
    *y = (int)floor(p.y);
}

- (void)mouseDown:(NSEvent *)ev
{
    int x, y;
    [self viewPoint:ev x:&x y:&y];
    s_buttonDown = 1;
    /* Control-click is the one-button mouse's right click, as it is everywhere
     * else on the Mac. */
    if ([ev modifierFlags] & NSControlKeyMask) { dispatch_mouse(WM_RBUTTONDOWN, x, y, 0); return; }
    {
        HWND target = hit_test(s_root, x, y);
        if (target) SetFocus(target);
    }
    dispatch_mouse([ev clickCount] >= 2 ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, x, y, MK_LBUTTON);
}

- (void)mouseUp:(NSEvent *)ev
{
    int x, y;
    [self viewPoint:ev x:&x y:&y];
    s_buttonDown = 0;
    dispatch_mouse(WM_LBUTTONUP, x, y, 0);
}

- (void)rightMouseDown:(NSEvent *)ev
{
    int x, y;
    [self viewPoint:ev x:&x y:&y];
    dispatch_mouse(WM_RBUTTONDOWN, x, y, 0);
}

- (void)otherMouseDown:(NSEvent *)ev
{
    int x, y;
    [self viewPoint:ev x:&x y:&y];
    dispatch_mouse(WM_MBUTTONDOWN, x, y, 0);
}

- (void)mouseDragged:(NSEvent *)ev
{
    int x, y;
    [self viewPoint:ev x:&x y:&y];
    track_mouse_leave(x, y);
    dispatch_mouse(WM_MOUSEMOVE, x, y, MK_LBUTTON);
    /* during a drag too: the splitter's resize cursor has to survive the
     * pointer moving over the panels on either side of it */
    apply_cursor(s_mouseIn);
}

- (void)mouseMoved:(NSEvent *)ev
{
    int x, y;
    [self viewPoint:ev x:&x y:&y];
    track_mouse_leave(x, y);
    dispatch_mouse(WM_MOUSEMOVE, x, y, 0);
    apply_cursor(s_mouseIn);
}

/* The editor divides a wheel delta by 40 to get three lines a notch, so a
 * notch has to arrive as a whole WHEEL_DELTA rather than a fraction of one. */
static int wheel_delta(float d)
{
    int notches = (int)(fabs(d) + 0.5f);
    if (notches < 1) notches = 1;
    return d > 0 ? 120 * notches : -120 * notches;
}

- (void)scrollWheel:(NSEvent *)ev
{
    int x, y;
    float dx = [ev deltaX], dy = [ev deltaY];
    [self viewPoint:ev x:&x y:&y];
    if (dy != 0.0f)
        dispatch_mouse(WM_MOUSEWHEEL, x, y, (WPARAM)MAKELONG(0, wheel_delta(dy)));
    /* Negated: a positive deltaX is a scroll to the left, where a positive
     * WM_MOUSEHWHEEL is a scroll to the right. Both axes arrive on the same
     * event from a trackpad, so neither is an else. */
    if (dx != 0.0f)
        dispatch_mouse(WM_MOUSEHWHEEL, x, y, (WPARAM)MAKELONG(0, wheel_delta(-dx)));
}

- (void)keyDown:(NSEvent *)ev
{
    NSString *chars = [ev characters];
    NSData *bytes = shim_bytes(chars);
    const unsigned char *b = bytes ? (const unsigned char *)[bytes bytes] : NULL;
    int n = bytes ? (int)[bytes length] : 0;
    HWND target = s_focus ? s_focus : s_root;
    char first = 0;
    int vk, i;

    /* An arrow or function key arrives as a private-use character, which is no
     * use as text; its key code identifies it instead. */
    if ([chars length] && [chars characterAtIndex:0] >= 0xF700) n = 0;
    if (n && b[0] < 0x80) first = (char)b[0];
    vk = mac_key_to_vk(first, [ev keyCode]);

    /* Queued rather than handed straight to the window proc: WinMain's loop
     * has to see it through GetMessage so app_hotkey gets first look.
     *
     * A character outside ASCII is several UTF-8 bytes, and the editor stores
     * bytes, so each one rides in on its own key-down with no virtual key.
     * TranslateMessage turns them into WM_CHARs in order. */
    queue_push(target, WM_KEYDOWN, (WPARAM)vk, (LPARAM)(n ? b[0] : 0));
    for (i = 1; i < n; i++)
        queue_push(target, WM_KEYDOWN, 0, (LPARAM)b[i]);
}

/* Dropped files take the same WM_DROPFILES path as the Finder's 'odoc'. */
- (unsigned int)draggingEntered:(id)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (signed char)performDragOperation:(id)sender
{
    NSArray *files = [[sender draggingPasteboard] propertyListForType:NSFilenamesPboardType];
    const char *paths[32];
    int n = 0, i, count;
    if (!files) return 0;
    count = (int)[files count];
    for (i = 0; i < count && n < 32; i++) {
        const char *p = [[files objectAtIndex:i] fileSystemRepresentation];
        if (p) paths[n++] = p;
    }
    if (!n) return 0;
    mac_drop_files(paths, n);
    return 1;
}

@end

/* --------------------------------------------------- window and app delegate */

@interface XPDelegate : NSObject
@end

@implementation XPDelegate

- (signed char)windowShouldClose:(id)sender
{
    (void)sender;
    /* The app decides: its WM_CLOSE saves the session and posts the quit. */
    if (s_root) SendMessage(s_root, WM_CLOSE, 0, 0);
    return 0;
}

- (void)windowDidResize:(NSNotification *)note
{
    NSSize sz;
    (void)note;
    if (!s_root || !s_root->wr) return;
    sz = [[s_root->wr contentView] bounds].size;
    SetRect(&s_root->rc, 0, 0, (int)sz.width, (int)sz.height);
    SendMessage(s_root, WM_SIZE, 0, MAKELONG((int)sz.width, (int)sz.height));
    InvalidateRect(s_root, NULL, TRUE);
}

- (void)windowDidBecomeKey:(NSNotification *)note
{
    (void)note;
    if (!s_root) return;
    SendMessage(s_root, WM_ACTIVATEAPP, 1, 0);
    if (s_focus) SendMessage(s_focus, WM_SETFOCUS, 0, 0);
    /* The selection and the focus rings differ between the active and inactive
     * states, so the whole window is redrawn either way. */
    InvalidateRect(s_root, NULL, TRUE);
}

- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    if (!s_root) return;
    SendMessage(s_root, WM_ACTIVATEAPP, 0, 0);
    if (s_focus) SendMessage(s_focus, WM_KILLFOCUS, 0, 0);
    InvalidateRect(s_root, NULL, TRUE);
}

/* Files opened from the Finder or dropped on the Dock icon. */
- (signed char)application:(NSApplication *)app openFile:(NSString *)path
{
    const char *p = [path fileSystemRepresentation];
    (void)app;
    if (!p) return 0;
    mac_drop_files(&p, 1);
    return 1;
}

- (void)application:(NSApplication *)app openFiles:(NSArray *)paths
{
    const char *buf[32];
    int n = 0, i, count = (int)[paths count];
    for (i = 0; i < count && n < 32; i++) {
        const char *p = [[paths objectAtIndex:i] fileSystemRepresentation];
        if (p) buf[n++] = p;
    }
    if (n) mac_drop_files(buf, n);
    [app replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

- (unsigned int)applicationShouldTerminate:(NSApplication *)app
{
    (void)app;
    /* The app's WM_CLOSE saves the session and posts the quit -- unless it
     * decides not to, which is what happens when a document has unsaved
     * changes and the user cancels the prompt. So the answer is whatever it
     * did: having posted the quit, everything is shut down already and AppKit
     * may take the process; otherwise the quit is genuinely refused. */
    if (s_root) SendMessage(s_root, WM_CLOSE, 0, 0);
    return s_quit ? NSTerminateNow : NSTerminateCancel;
}

@end

static XPDelegate *s_delegate;

/* ---------------------------------------------------------------- classes */

BOOL RegisterClass(const WNDCLASS *wc)
{
    WinClass *c;
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
    WinClass *c = find_class(cls);
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
        NSScreen *screen = [NSScreen mainScreen];
        NSRect avail = [screen visibleFrame];
        NSRect full = [screen frame];
        NSRect frame;
        XPView *view;

        if (s_root) { win->used = 0; return NULL; }   /* one top-level only */

        /* The size comes from the .ini, which travels between machines, so
         * clamp it to what this display can actually show. */
        if (w <= 0) w = 1024;
        if (h <= 0) h = 720;
        if (w > (int)avail.size.width) w = (int)avail.size.width;
        if (h > (int)avail.size.height) h = (int)avail.size.height;

        /* Win32 screen coordinates run down from the top left; Cocoa's run up
         * from the bottom left. */
        frame = NSMakeRect((float)x, full.size.height - (float)(y + h), (float)w, (float)h);

        win->wr = [[NSWindow alloc]
                      initWithContentRect:frame
                                styleMask:(NSTitledWindowMask | NSClosableWindowMask |
                                           NSMiniaturizableWindowMask | NSResizableWindowMask)
                                  backing:NSBackingStoreBuffered
                                    defer:NO];
        if (!win->wr) { win->used = 0; return NULL; }
        [win->wr setTitle:shim_string(name, -1)];
        [win->wr setReleasedWhenClosed:NO];
        [win->wr setContentMinSize:NSMakeSize(480, 360)];
        /* keep the title bar on screen whatever the .ini asked for */
        [win->wr setFrame:[win->wr constrainFrameRect:[win->wr frame] toScreen:screen]
                  display:NO];

        view = [[XPView alloc] initWithFrame:[[win->wr contentView] bounds]];
        [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [view registerForDraggedTypes:[NSArray arrayWithObject:NSFilenamesPboardType]];
        [win->wr setContentView:view];
        [win->wr setAcceptsMouseMovedEvents:YES];
        [win->wr makeFirstResponder:view];
        s_view = view;

        if (!s_delegate) s_delegate = [[XPDelegate alloc] init];
        /* Passed as a plain id: on 10.4 a delegate is an informal protocol and
         * these setters take id, so there is no protocol for XPDelegate to
         * declare conformance to. */
        [win->wr setDelegate:(id)s_delegate];
        [NSApp setDelegate:(id)s_delegate];

        {
            NSSize sz = [[win->wr contentView] bounds].size;
            SetRect(&win->rc, 0, 0, (int)sz.width, (int)sz.height);
        }
        s_root = win;
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
    if (w->wr) {
        [w->wr setDelegate:nil];
        [w->wr orderOut:nil];
        s_root = NULL;
        s_view = nil;
    }
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
        NSWindow *wr = s_root->wr;
        NSRect content = [wr contentRectForFrameRect:[wr frame]];
        NSScreen *screen = [wr screen] ? [wr screen] : [NSScreen mainScreen];
        LONG top = (LONG)([screen frame].size.height - (content.origin.y + content.size.height));
        r->left += (LONG)content.origin.x; r->right += (LONG)content.origin.x;
        r->top += top; r->bottom += top;
    }
}

void MoveWindow(HWND w, int x, int y, int cw, int ch, BOOL repaint)
{
    int ow, oh;
    if (!w) return;
    ow = w->rc.right - w->rc.left; oh = w->rc.bottom - w->rc.top;
    SetRect(&w->rc, x, y, x + cw, y + ch);
    if (w->wr) {
        /* Resize about the top left, which is the corner Win32 anchors a
         * window to; Cocoa would otherwise keep the bottom left still. */
        NSRect content = [w->wr contentRectForFrameRect:[w->wr frame]];
        content.origin.y -= (float)ch - content.size.height;
        content.size.width = (float)cw;
        content.size.height = (float)ch;
        [w->wr setFrame:[w->wr frameRectForContentRect:content] display:YES];
        return;                     /* the resize notification sends WM_SIZE */
    }
    if (cw != ow || ch != oh) SendMessage(w, WM_SIZE, 0, MAKELONG(cw, ch));
    if (repaint) InvalidateRect(w, NULL, TRUE);
}

void ShowWindow(HWND w, int cmd)
{
    if (!w) return;
    w->visible = cmd ? 1 : 0;
    if (w->wr) {
        if (!w->visible) { [w->wr orderOut:nil]; return; }
        if (cmd == SW_SHOWMAXIMIZED) {
            NSScreen *screen = [w->wr screen] ? [w->wr screen] : [NSScreen mainScreen];
            [w->wr setFrame:[screen visibleFrame] display:NO];
        }
        [w->wr makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        /* The app lays its panels out from WM_SIZE. Nothing has sent one yet,
         * so the first paint would use stale child rects and the window would
         * stay half-drawn until the user resized it. */
        {
            NSSize sz = [[w->wr contentView] bounds].size;
            SetRect(&w->rc, 0, 0, (int)sz.width, (int)sz.height);
            SendMessage(w, WM_SIZE, 0, MAKELONG((int)sz.width, (int)sz.height));
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
    NSPoint loc = [NSEvent mouseLocation];
    p->x = (LONG)loc.x;
    p->y = (LONG)([[NSScreen mainScreen] frame].size.height - loc.y);
}

BOOL PtInRect(const RECT *r, POINT p)
{
    return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom;
}

/* ---------------------------------------------------------------- painting */

void InvalidateRect(HWND w, const RECT *r, BOOL erase)
{
    RECT sr;
    (void)erase;
    if (!w || !w->used) return;
    w->dirty = 1;
    if (!s_view) return;
    win_screen_rect(w, &sr);
    if (r) {
        int ox, oy;
        win_origin(w, &ox, &oy);
        sr.left = ox + r->left; sr.top = oy + r->top;
        sr.right = ox + r->right; sr.bottom = oy + r->bottom;
    }
    [s_view setNeedsDisplayInRect:NSMakeRect((float)sr.left, (float)sr.top,
                                             (float)(sr.right - sr.left),
                                             (float)(sr.bottom - sr.top))];
}

static int rects_overlap(const RECT *a, const RECT *b)
{
    return a->left < b->right && b->left < a->right &&
           a->top < b->bottom && b->top < a->bottom;
}

/* Paint the tree, parents before children -- the order Win32's
 * WS_CLIPCHILDREN painting gives -- skipping anything the dirty rect does not
 * touch.
 *
 * Each panel renders its whole offscreen buffer inside its WM_PAINT and blits
 * it, so painting one that is not dirty is pure waste, and the editor's buffer
 * is the size of the window. Clipping stops the pixels reaching the screen but
 * it cannot stop the work from happening. */
static void paint_tree(HWND w, const RECT *clip)
{
    int i;
    RECT sr;
    if (!w->used || !w->visible) return;
    win_screen_rect(w, &sr);
    /* children live inside the parent, so skipping it skips the subtree */
    if (clip && !rects_overlap(&sr, clip)) return;
    if (sr.right > sr.left && sr.bottom > sr.top) {
        HDC dc = gdi_begin_window_paint(w, &sr);
        if (dc) {
            /* BeginPaint inside the proc hands back this DC. */
            gdi_set_paint_dc(dc);
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
    RECT sr;
    if (!w || !w->used || !s_view) return;
    win_screen_rect(w, &sr);
    [s_view setNeedsDisplayInRect:NSMakeRect((float)sr.left, (float)sr.top,
                                             (float)(sr.right - sr.left),
                                             (float)(sr.bottom - sr.top))];
    [s_view displayIfNeeded];
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
    /* WM_MOUSELEAVE is synthesised from the moved events; nothing to arm. */
    (void)e;
    return TRUE;
}

void PostQuitMessage(int code) { s_quit = 1; s_quitCode = code; }

BOOL SetTimer(HWND w, UINT id, UINT ms, LPVOID proc)
{
    (void)id; (void)proc;
    s_timerWnd = w; s_timerMS = ms;
    s_nextTimer = GetTickCount() + ms;
    return TRUE;
}

void SetWindowText(HWND w, LPCSTR s)
{
    if (!w) return;
    strncpy(w->text, s ? s : "", sizeof w->text - 1);
    w->text[sizeof w->text - 1] = 0;
    if (w->wr) { [w->wr setTitle:shim_string(w->text, -1)]; return; }
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
    switch (vk) {
    /* Ctrl in the source means "the command modifier" on the Mac, which is
     * where the editor's shortcuts live; the real control key is left for the
     * Control-click that stands in for a right click. */
    case VK_CONTROL: return (s_mods & NSCommandKeyMask) ? (short)0x8000 : 0;
    case VK_SHIFT:   return (s_mods & NSShiftKeyMask) ? (short)0x8000 : 0;
    case VK_MENU:    return (s_mods & NSAlternateKeyMask) ? (short)0x8000 : 0;
    default:         return 0;
    }
}

/* ---------------------------------------------------------------- the loop */

/* How long one pass may block. Anything that needs regular attention -- the
 * blinking caret, a drag in progress, the terminal's pty -- wants a short
 * wait; an idle editor should not wake the processor fifty times a second. */
static double wait_seconds(void)
{
    if (s_buttonDown || s_idleProc || gdi_caret_active()) return 0.02;
    return 0.1;
}

/* One autorelease pool per pass, released at the top of the next one so that
 * it also covers the app's own dispatch back in WinMain's loop.
 *
 * It must also be released before this returns for the last time. Pools nest,
 * and have to be released in the order they were made: leaving this one open
 * means the pool main() made -- which is older -- is released first, and that
 * is an error the runtime turns into a crash on the way out. */
static NSAutoreleasePool *s_pool;

BOOL GetMessage(LPMSG m, HWND w, UINT lo, UINT hi)
{
    (void)w; (void)lo; (void)hi;

    for (;;) {
        NSEvent *ev;

        if (s_pool) { [s_pool release]; s_pool = nil; }
        if (s_quit) return FALSE;
        s_pool = [[NSAutoreleasePool alloc] init];

        /* posted messages first, as Win32's queue does */
        if (queue_pop(m)) return TRUE;

        /* anything invalidated gets drawn before we block for input */
        if (s_view && [s_view needsDisplay]) [s_view displayIfNeeded];

        gdi_caret_tick();
        /* the terminal drains its pty from here */
        if (s_idleProc) s_idleProc();

        if (s_timerWnd && GetTickCount() >= s_nextTimer) {
            s_nextTimer = GetTickCount() + s_timerMS;
            SendMessage(s_timerWnd, WM_TIMER, 1, 0);
            continue;
        }

        ev = [NSApp nextEventMatchingMask:NSAnyEventMask
                                untilDate:[NSDate dateWithTimeIntervalSinceNow:wait_seconds()]
                                   inMode:NSDefaultRunLoopMode
                                  dequeue:YES];
        if (!ev) continue;
        s_mods = [ev modifierFlags];
        [NSApp sendEvent:ev];
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
