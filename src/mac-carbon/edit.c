/* A minimal single-line EDIT control.
 *
 * XPico Code creates three of these: the find and replace fields, the sidebar's
 * search box and the command palette's input. Each is subclassed by the app,
 * which handles Return/Escape/arrows itself and chains the rest back here, so
 * this control only has to manage text, a caret and a selection.
 *
 * It is registered under the name "XPEdit"; mac-carbon/wnd.c maps CreateWindowEx with
 * the class "EDIT" onto it. */
/* This file uses the app's own palette and text helpers, not the Toolbox, so it
 * includes xpcode.h and is built with the app sources (see the override note in
 * mac-carbon/override/strings.h). */
#include "../xpcode.h"
#include <string.h>

#define EDIT_MAX 512

typedef struct {
    char text[EDIT_MAX];
    int  len;
    int  sel0, sel1;    /* selection range; equal means a plain caret */
    int  focus;
} EditState;

/* One state per control: there are only ever three, and the shim keeps them in
 * a small table keyed by HWND rather than adding per-window storage. */
#define MAX_EDITS 6
static struct { HWND w; EditState st; } s_edits[MAX_EDITS];

static EditState *state_for(HWND w)
{
    int i, free = -1;
    for (i = 0; i < MAX_EDITS; i++) {
        if (s_edits[i].w == w) return &s_edits[i].st;
        if (!s_edits[i].w && free < 0) free = i;
    }
    if (free < 0) return NULL;
    memset(&s_edits[free], 0, sizeof s_edits[free]);
    s_edits[free].w = w;
    return &s_edits[free].st;
}

static void notify_parent(HWND w, int code)
{
    /* Win32 posts EN_CHANGE to the PARENT, with the control id in the low word
     * of wParam and the control handle in lParam. Sending it to the control
     * itself meant the command palette and the sidebar search box never
     * refiltered as you typed. */
    HWND parent = shim_parent(w);
    if (!parent) return;
    SendMessage(parent, WM_COMMAND, (WPARAM)MAKELONG(shim_child_id(w), code), (LPARAM)w);
}

static void replace_selection(EditState *e, const char *ins, int n)
{
    int a = e->sel0 < e->sel1 ? e->sel0 : e->sel1;
    int b = e->sel0 < e->sel1 ? e->sel1 : e->sel0;
    int tail = e->len - b;
    if (a < 0) a = 0;
    if (b > e->len) b = e->len;
    if (n > EDIT_MAX - 1 - (a + tail)) n = EDIT_MAX - 1 - (a + tail);
    if (n < 0) n = 0;
    memmove(e->text + a + n, e->text + b, (size_t)tail);
    if (n) memcpy(e->text + a, ins, (size_t)n);
    e->len = a + n + tail;
    e->text[e->len] = 0;
    e->sel0 = e->sel1 = a + n;
}

static void paint(HWND w, EditState *e)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT rc;
    GetClientRect(w, &rc);
    if (!dc) return;

    SetBkColor(dc, C_INPUT_BG);
    ExtTextOut(dc, 0, 0, ETO_OPAQUE, &rc, NULL, 0, NULL);

    if (e->len) {
        /* draw the selection behind the text */
        if (e->sel0 != e->sel1) {
            int a = e->sel0 < e->sel1 ? e->sel0 : e->sel1;
            int b = e->sel0 < e->sel1 ? e->sel1 : e->sel0;
            int xa = 2 + text_w(dc, g_fUI, e->text, a);
            int xb = 2 + text_w(dc, g_fUI, e->text, b);
            RECT sr;
            SetRect(&sr, xa, 1, xb, rc.bottom - 1);
            SetBkColor(dc, C_SELECTION);
            ExtTextOut(dc, 0, 0, ETO_OPAQUE, &sr, NULL, 0, NULL);
        }
        text_at(dc, 2, 2, e->text, e->len, C_TEXT, g_fUI);
    }

    if (e->focus) {
        int cx = 2 + text_w(dc, g_fUI, e->text, e->sel1);
        SetCaretPos(cx, 2);
    }
    EndPaint(w, &ps);
}

static LRESULT CALLBACK EditProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    EditState *e = state_for(w);
    if (!e) return DefWindowProc(w, m, wp, lp);

    switch (m) {
    case WM_CREATE:
        e->len = 0; e->text[0] = 0; e->sel0 = e->sel1 = 0;
        return 0;

    case WM_PAINT:
        paint(w, e);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SETFOCUS:
        e->focus = 1;
        CreateCaret(w, NULL, 2, 14);
        ShowCaret(w);
        InvalidateRect(w, NULL, FALSE);
        return 0;

    case WM_KILLFOCUS:
        e->focus = 0;
        DestroyCaret();
        InvalidateRect(w, NULL, FALSE);
        return 0;

    case WM_CHAR: {
        char ch = (char)wp;
        if (ch == 8) {                       /* backspace */
            if (e->sel0 == e->sel1 && e->sel0 > 0) e->sel0--;
            replace_selection(e, NULL, 0);
        } else if ((unsigned char)ch >= 32) {
            replace_selection(e, &ch, 1);
        } else {
            return 0;                        /* Return/Tab/Escape: the app's job */
        }
        notify_parent(w, EN_CHANGE);
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        switch (wp) {
        case VK_LEFT:  if (e->sel1 > 0) e->sel1--; if (!shift) e->sel0 = e->sel1; break;
        case VK_RIGHT: if (e->sel1 < e->len) e->sel1++; if (!shift) e->sel0 = e->sel1; break;
        case VK_HOME:  e->sel1 = 0; if (!shift) e->sel0 = 0; break;
        case VK_END:   e->sel1 = e->len; if (!shift) e->sel0 = e->len; break;
        case VK_DELETE:
            if (e->sel0 == e->sel1 && e->sel0 < e->len) e->sel1++;
            replace_selection(e, NULL, 0);
            notify_parent(w, EN_CHANGE);
            break;
        default:
            return 0;
        }
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        /* place the caret at the clicked character */
        HDC dc = GetDC(w);
        int x = (int)(short)LOWORD(lp), i, best = e->len;
        if (dc) {
            for (i = 0; i <= e->len; i++)
                if (2 + text_w(dc, g_fUI, e->text, i) >= x) { best = i; break; }
            ReleaseDC(w, dc);
        }
        e->sel0 = e->sel1 = best;
        SetFocus(w);
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }

    case EM_SETSEL:
        e->sel0 = (int)wp; e->sel1 = (int)lp;
        if (e->sel1 < 0 || e->sel1 > e->len) e->sel1 = e->len;
        if (e->sel0 < 0 || e->sel0 > e->len) e->sel0 = e->len;
        InvalidateRect(w, NULL, FALSE);
        return 0;

    case WM_SETFONT:
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    default:
        return DefWindowProc(w, m, wp, lp);
    }
}

/* mac-carbon/wnd.c calls SetWindowText/GetWindowText for these fields, so the shim's
 * generic text storage has to stay in step with the control's own buffer. */
void edit_set_text(HWND w, const char *s)
{
    EditState *e = state_for(w);
    if (!e) return;
    strncpy(e->text, s ? s : "", EDIT_MAX - 1);
    e->text[EDIT_MAX - 1] = 0;
    e->len = (int)strlen(e->text);
    e->sel0 = e->sel1 = e->len;
    InvalidateRect(w, NULL, FALSE);
}

int edit_get_text(HWND w, char *buf, int n)
{
    EditState *e = state_for(w);
    if (!e || n <= 0) { if (n > 0) buf[0] = 0; return 0; }
    strncpy(buf, e->text, n - 1);
    buf[n - 1] = 0;
    return (int)strlen(buf);
}

int edit_is_edit(HWND w)
{
    int i;
    for (i = 0; i < MAX_EDITS; i++)
        if (s_edits[i].w == w) return 1;
    return 0;
}

void edit_register(void)
{
    WNDCLASS wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = EditProc;
    wc.lpszClassName = "XPEdit";
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    RegisterClass(&wc);
}
