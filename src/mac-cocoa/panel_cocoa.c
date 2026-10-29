/* The bottom panel for the Cocoa build: Problems, Output and Terminals.
 *
 * This replaces terminal.c, which drives cmd.exe over Win32 pipes. The shells
 * here are login shells in ptys (mac-cocoa/term_pty.c), so this file is the
 * view: a scrollback buffer per terminal, a small ANSI parser, the list of
 * running terminals and the keyboard handling. The header toolbar and the
 * terminal list are laid out as terminal.c lays them out on Windows.
 *
 * Built with the app sources: it uses xpcode.h's drawing helpers, not the
 * frameworks. */
#include "../xpcode.h"
#include <string.h>

#define OUT_MAX    400
#define LINE_MAX   256
#define TERM_MAX   8            /* must match term_pty.c */
#define TERM_ROWS  600
#define TERM_COLS  200
#define LIST_W     150
#define HEADER_H   31

/* ---- output log ---- */
static char s_out[OUT_MAX][LINE_MAX];
static int  s_outCount, s_outFirst;

/* ---- one terminal: a pty and the screen it is drawn on ---- */
typedef struct {
    int   used;
    int   pty;                  /* term_pty id; -1 once the shell has gone */
    int   number;               /* the label the user sees */
    int   exited, exitCode;
    int   rows;                 /* rows in use */
    int   cx, cy;               /* cursor; cy indexes the scrollback */
    int   scroll;               /* rows scrolled back from the bottom */
    int   inEsc, inCsi;         /* the ANSI parser's state, per terminal */
    char *scr;                  /* TERM_ROWS rows of TERM_COLS + 1 bytes */
} Term;

static Term s_terms[TERM_MAX];
static int  s_nterms, s_cur = -1, s_counter;
static int  s_tab;

/* The header's right-hand toolbar, laid out as terminal.c lays it out on
 * Windows: close on every tab, and the terminal's own buttons beside it. */
static RECT s_btnNew, s_btnKill, s_btnClear, s_btnClose;
static int  s_hoverBtn = -1, s_hoverRow = -1;

/* mac-cocoa/term_pty.c */
int  term_pty_open(const char *cwd, int cols, int rows);
void term_pty_close(int id);
void term_pty_close_all(void);
void term_pty_send(int id, const char *data, int n);
void term_pty_resize(int id, int cols, int rows);
void term_pty_poll(void);
const char *term_pty_error(void);
const char *term_pty_shell_name(void);
int  term_pty_alive(int id);

static char *trow(Term *t, int i) { return t->scr + (size_t)i * (TERM_COLS + 1); }
static Term *curterm(void) { return s_cur >= 0 && s_cur < s_nterms ? &s_terms[s_cur] : NULL; }

static const char *tab_name(int i)
{
    return i == 0 ? "PROBLEMS" : i == 1 ? "OUTPUT" : "TERMINAL";
}

/* ---------------------------------------------------------------- output */

void panel_output(const char *line)
{
    char *dst;
    if (!line) return;
    if (s_outCount < OUT_MAX) dst = s_out[(s_outFirst + s_outCount++) % OUT_MAX];
    else { dst = s_out[s_outFirst]; s_outFirst = (s_outFirst + 1) % OUT_MAX; }
    strncpy(dst, line, LINE_MAX - 1);
    dst[LINE_MAX - 1] = 0;
    if (g_hwndPanel) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

void panel_show_tab(int tab)
{
    s_tab = tab;
    if (!g_panelVisible) { g_panelVisible = 1; app_layout(); }
    if (g_hwndPanel) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

int problems_count(int *errors, int *warnings)
{
    if (errors) *errors = 0;
    if (warnings) *warnings = 0;
    return 0;
}

/* ---------------------------------------------------------- terminal screen */

static void term_newline(Term *t)
{
    t->cx = 0;
    t->cy++;
    if (t->cy >= TERM_ROWS) {
        /* drop the oldest row and keep the cursor on the last one */
        memmove(trow(t, 0), trow(t, 1), (size_t)(TERM_ROWS - 1) * (TERM_COLS + 1));
        t->cy = TERM_ROWS - 1;
    }
    if (t->cy >= t->rows) t->rows = t->cy + 1;
    trow(t, t->cy)[0] = 0;
}

static void term_putc(Term *t, char c)
{
    char *row;
    int len;
    if (c == '\n') { term_newline(t); return; }
    if (c == '\r') { t->cx = 0; return; }
    if (c == '\b') { if (t->cx > 0) t->cx--; return; }
    if (c == '\t') {
        int next = (t->cx + 8) & ~7;
        while (t->cx < next && t->cx < TERM_COLS) term_putc(t, ' ');
        return;
    }
    /* Byte by byte, so a UTF-8 character takes as many cells as it has bytes.
     * The editor's own text handling counts bytes the same way. */
    if ((unsigned char)c < 32) return;
    if (t->cx >= TERM_COLS) term_newline(t);

    row = trow(t, t->cy);
    len = (int)strlen(row);
    while (len < t->cx) row[len++] = ' ';
    row[t->cx] = c;
    if (t->cx + 1 > len) row[t->cx + 1] = 0;
    t->cx++;
}

static Term *term_by_pty(int id)
{
    int i;
    for (i = 0; i < s_nterms; i++)
        if (s_terms[i].used && s_terms[i].pty == id) return &s_terms[i];
    return NULL;
}

/* A deliberately small ANSI handler: enough for a login shell's prompt and
 * colour codes, which are discarded rather than rendered. */
static void term_feed(Term *t, const char *data, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char c = data[i];
        if (t->inEsc) {
            if (!t->inCsi) {
                if (c == '[') { t->inCsi = 1; continue; }
                t->inEsc = 0;                    /* a two-character sequence */
                continue;
            }
            /* CSI runs until a final byte in 0x40-0x7E */
            if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) {
                if (c == 'J' || c == 'K') trow(t, t->cy)[t->cx] = 0;  /* erase to EOL */
                t->inEsc = t->inCsi = 0;
            }
            continue;
        }
        if (c == 0x1B) { t->inEsc = 1; t->inCsi = 0; continue; }
        term_putc(t, c);
    }
    t->scroll = 0;
}

void term_on_data(int id, const char *data, int n)
{
    Term *t = term_by_pty(id);
    if (!t) return;
    term_feed(t, data, n);
    if (g_hwndPanel) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

void term_on_exit(int id, int status)
{
    Term *t = term_by_pty(id);
    char msg[64];
    if (!t) return;
    sfmt(msg, sizeof msg, "[shell exited: %d]", status);
    term_feed(t, "\r\n", 2);
    term_feed(t, msg, (int)strlen(msg));
    term_feed(t, "\r\n", 2);
    t->exited = 1;
    t->exitCode = status;
    t->pty = -1;
    if (g_hwndPanel) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

/* ---------------------------------------------------------- the term_ API */

static void idle_poll(void) { term_pty_poll(); }

/* The terminal list takes the right-hand edge of the body once there is more
 * than one shell to list, so the shell's own width has to allow for it. */
static int list_shown(void) { return s_tab == 2 && s_nterms > 1; }

static int term_cols(void)
{
    RECT rc;
    if (!g_hwndPanel || g_codeCW <= 0) return 80;
    GetClientRect(g_hwndPanel, &rc);
    return (rc.right - 16 - (list_shown() ? LIST_W : 0)) / g_codeCW;
}

static int term_rows_visible(void)
{
    RECT rc;
    if (!g_hwndPanel || g_codeCH <= 0) return 24;
    GetClientRect(g_hwndPanel, &rc);
    return (rc.bottom - HEADER_H) / g_codeCH;
}

/* Start another shell, as the Windows build does: every New Terminal adds one
 * and makes it current. */
void term_new(const char *cwd)
{
    Term *t;
    int id;

    if (s_nterms >= TERM_MAX) {
        out_log("Terminal limit reached (%d)", TERM_MAX);
        return;
    }
    t = &s_terms[s_nterms];
    memset(t, 0, sizeof *t);
    t->scr = (char *)calloc(TERM_ROWS, TERM_COLS + 1);
    if (!t->scr) { out_log("Out of memory starting a terminal"); return; }

    id = term_pty_open(cwd, term_cols(), term_rows_visible());
    if (id < 0) {
        free(t->scr);
        t->scr = NULL;
        out_log("%s", term_pty_error());
        return;                                  /* the panel shows the reason */
    }

    t->used = 1;
    t->pty = id;
    t->number = ++s_counter;
    t->rows = 1;
    s_nterms++;
    s_cur = s_nterms - 1;
    shim_set_idle(idle_poll);
    if (g_hwndPanel) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

void term_send_line(const char *cmd)
{
    Term *t = curterm();
    if (!t || t->exited) { panel_show_tab(2); return; }
    term_pty_send(t->pty, cmd, (int)strlen(cmd));
    term_pty_send(t->pty, "\n", 1);
    panel_show_tab(2);
}

/* Remove the active terminal, as terminal.c does on Windows: the trash button
 * deletes the instance rather than interrupting what is running in it. An
 * exited shell has to go too -- that is the only way to clear it off the list.
 *
 * term_pty_close does the teardown: SIGHUP, reap, and close the master, which
 * also frees the pty slot for the next terminal. */
void term_kill(void)
{
    Term *t = curterm();
    if (!t) return;
    term_pty_close(t->pty);
    free(t->scr);
    /* Close the gap, so s_terms stays 0..s_nterms-1 with no holes -- term_new
     * appends at s_terms[s_nterms] and find_term_by_pty scans that range. */
    memmove(t, t + 1, (size_t)(s_nterms - s_cur - 1) * sizeof *t);
    s_nterms--;
    memset(&s_terms[s_nterms], 0, sizeof s_terms[s_nterms]);
    if (s_cur >= s_nterms) s_cur = s_nterms - 1;
    out_log("Terminal killed");
    if (g_hwndPanel) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

void term_clear(void)
{
    if (s_tab == 2) {
        Term *t = curterm();
        if (t) { t->rows = 1; t->cx = t->cy = 0; trow(t, 0)[0] = 0; t->scroll = 0; }
    } else {
        s_outCount = s_outFirst = 0;
    }
    if (g_hwndPanel) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

void term_focus(void)
{
    panel_show_tab(2);
    if (g_hwndPanel) SetFocus(g_hwndPanel);
}

void term_shutdown(void)
{
    int i;
    term_pty_close_all();
    for (i = 0; i < s_nterms; i++) {
        free(s_terms[i].scr);
        s_terms[i].scr = NULL;
        s_terms[i].used = 0;
    }
    s_nterms = 0;
    s_cur = -1;
}

/* ---------------------------------------------------------------- painting */

static void paint_terminal(HDC dc, const RECT *rc)
{
    Term *t = curterm();
    int bodyw = rc->right - (list_shown() ? LIST_W : 0);
    int step = g_codeCH > 0 ? g_codeCH : 12;
    int rows, y, i, first;

    if (!t) {
        int line = HEADER_H + 10;
        const char *err = term_pty_error();
        text_at(dc, 12, line, "No shell is running.", -1, C_TEXT, g_fUI);
        if (err && *err)
            text_at(dc, 12, line + step + 4, err, -1, C_ERROR, g_fUI);
        text_at(dc, 12, line + 2 * (step + 4),
                "Choose Terminal > New Terminal to start one.", -1, C_TEXT_DIM, g_fUI);
        return;
    }

    rows = (rc->bottom - HEADER_H - 4) / step;
    first = t->rows - rows - t->scroll;
    if (first < 0) first = 0;

    y = HEADER_H + 2;
    for (i = first; i < t->rows && i < first + rows; i++) {
        text_at(dc, 8, y, trow(t, i), -1, C_TEXT, g_fCode);
        /* block cursor on the active row */
        if (i == t->cy && !t->exited) {
            int cx = 8 + text_w(dc, g_fCode, trow(t, i), t->cx);
            if (cx < bodyw - 2) fill(dc, cx, y, 2, step, C_TEXT_BRIGHT);
        }
        y += step;
    }

    if (t->exited) {
        char b[96];
        sfmt(b, sizeof b, "The shell exited (code %d). Press Enter to start another.", t->exitCode);
        if (y < rc->bottom) {
            fill(dc, 12, y + 4, text_w(dc, g_fUI, b, -1) + 16, 20, C_SEL_INACTIVE);
            text_at(dc, 20, y + 7, b, -1, C_SIDEBAR_TEXT, g_fUI);
        }
    }

    /* the list of running terminals, down the right-hand edge */
    if (list_shown()) {
        int x = rc->right - LIST_W;
        vline(dc, x, HEADER_H, rc->bottom - HEADER_H, C_BORDER);
        for (i = 0; i < s_nterms; i++) {
            int ry = HEADER_H + i * 22;
            char b[48];
            if (ry + 22 > rc->bottom) break;
            if (i == s_cur) fill(dc, x + 1, ry, LIST_W - 1, 22, C_LIST_ACTIVE);
            else if (i == s_hoverRow) fill(dc, x + 1, ry, LIST_W - 1, 22, C_LIST_HOVER);
            draw_icon(dc, ICON_TERMINAL, x + 18, ry + 11, 14,
                      s_terms[i].exited ? C_TEXT_DIM : C_SIDEBAR_TEXT);
            sfmt(b, sizeof b, "%d: %s%s", s_terms[i].number, term_pty_shell_name(),
                 s_terms[i].exited ? " (exited)" : "");
            text_ellipsis(dc, x + 32, ry + 4, LIST_W - 40, b, C_SIDEBAR_TEXT, g_fUI);
        }
    }
}

static void panel_paint(HWND w)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT rc;
    int i, x;

    GetClientRect(w, &rc);
    if (!dc) return;

    fill_rc(dc, &rc, C_EDITOR_BG);
    hline(dc, 0, 0, rc.right, C_BORDER);

    x = 12;
    for (i = 0; i < 3; i++) {
        const char *name = tab_name(i);
        int tw = text_w(dc, g_fUISmall, name, -1);
        text_at(dc, x, 8, name, -1, i == s_tab ? C_TEXT_BRIGHT : C_TEXT_DIM, g_fUISmall);
        if (i == s_tab) fill(dc, x, 26, tw, 1, C_TEXT_BRIGHT);
        x += tw + 20;
    }

    SetRect(&s_btnClose, rc.right - 30, 7, rc.right - 8, 29);
    SetRect(&s_btnKill, rc.right - 56, 7, rc.right - 34, 29);
    SetRect(&s_btnClear, rc.right - 82, 7, rc.right - 60, 29);
    SetRect(&s_btnNew, rc.right - 108, 7, rc.right - 86, 29);
    if (s_tab == 2) {
        Term *t = curterm();
        if (s_hoverBtn == 0) fill_rc(dc, &s_btnNew, C_SEL_INACTIVE);
        if (s_hoverBtn == 1) fill_rc(dc, &s_btnKill, C_SEL_INACTIVE);
        if (s_hoverBtn == 3) fill_rc(dc, &s_btnClear, C_SEL_INACTIVE);
        draw_icon(dc, ICON_PLUS, s_btnNew.left + 11, 18, 14, C_SIDEBAR_TEXT);
        draw_icon(dc, ICON_TRASH, s_btnKill.left + 11, 18, 14, C_SIDEBAR_TEXT);
        draw_icon(dc, ICON_COLLAPSE, s_btnClear.left + 11, 18, 14, C_SIDEBAR_TEXT);
        if (t) {
            char b[48];
            sfmt(b, sizeof b, "%d: %s", t->number, term_pty_shell_name());
            text_at(dc, s_btnNew.left - 10 - text_w(dc, g_fUI, b, -1), 10, b, -1,
                    C_SIDEBAR_TEXT, g_fUI);
        }
    }
    if (s_hoverBtn == 2) fill_rc(dc, &s_btnClose, C_SEL_INACTIVE);
    draw_icon(dc, ICON_CLOSE, s_btnClose.left + 11, 18, 14, C_SIDEBAR_TEXT);

    hline(dc, 0, 30, rc.right, C_BORDER);

    if (s_tab == 0) {
        text_at(dc, 12, HEADER_H + 10, "No problems have been detected.",
                -1, C_TEXT_DIM, g_fUI);
    } else if (s_tab == 1) {
        int step = g_codeCH > 0 ? g_codeCH : 12;
        int rows = (rc.bottom - HEADER_H) / step;
        int y = HEADER_H + 2;
        for (i = s_outCount - rows; i < s_outCount; i++) {
            if (i < 0) { y += step; continue; }
            text_at(dc, 12, y, s_out[(s_outFirst + i) % OUT_MAX], -1, C_TEXT, g_fCode);
            y += step;
        }
    } else {
        paint_terminal(dc, &rc);
    }
    EndPaint(w, &ps);
}

/* ---------------------------------------------------------------- input */

static void send_key(Term *t, WPARAM vk)
{
    /* the few keys a shell needs that are not plain characters */
    switch (vk) {
    case VK_UP:    term_pty_send(t->pty, "\033[A", 3); break;
    case VK_DOWN:  term_pty_send(t->pty, "\033[B", 3); break;
    case VK_RIGHT: term_pty_send(t->pty, "\033[C", 3); break;
    case VK_LEFT:  term_pty_send(t->pty, "\033[D", 3); break;
    case VK_HOME:  term_pty_send(t->pty, "\033[H", 3); break;
    case VK_END:   term_pty_send(t->pty, "\033[F", 3); break;
    case VK_DELETE:term_pty_send(t->pty, "\033[3~", 4); break;
    case VK_BACK:  term_pty_send(t->pty, "\177", 1); break;
    case VK_RETURN:term_pty_send(t->pty, "\r", 1); break;
    case VK_TAB:   term_pty_send(t->pty, "\t", 1); break;
    case VK_ESCAPE:term_pty_send(t->pty, "\033", 1); break;
    default: break;
    }
}

static LRESULT CALLBACK PanelProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: panel_paint(w); return 0;
    case WM_ERASEBKGND: return 1;

    case WM_SIZE: {
        int i;
        for (i = 0; i < s_nterms; i++)
            if (!s_terms[i].exited) term_pty_resize(s_terms[i].pty, term_cols(), term_rows_visible());
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(w, NULL, FALSE);
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_KEYDOWN: {
        Term *t = curterm();
        if (s_tab != 2 || !t) return 0;
        /* A dead terminal takes Enter as "start another", as the Windows build
         * does; there is nothing else useful to do with it. */
        if (t->exited) {
            if (wp == VK_RETURN) app_command(CMD_NEW_TERMINAL);
            return 0;
        }
        send_key(t, wp);
        return 0;
    }

    case WM_CHAR: {
        Term *t = curterm();
        if (s_tab == 2 && t && !t->exited) {
            char c = (char)wp;
            if (c != '\r' && c != '\t' && c != 8 && (unsigned char)c >= 32)
                term_pty_send(t->pty, &c, 1);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp), i, x = 12;
        POINT pt;
        RECT rc;
        HDC dc;
        SetFocus(w);
        pt.x = mx; pt.y = my;
        GetClientRect(w, &rc);

        if (my > 30) {
            /* a click in the terminal list picks that terminal */
            if (list_shown() && mx >= rc.right - LIST_W) {
                int row = (my - HEADER_H) / 22;
                if (row >= 0 && row < s_nterms) { s_cur = row; InvalidateRect(w, NULL, FALSE); }
            }
            return 0;
        }

        if (PtInRect(&s_btnClose, pt)) { app_command(CMD_TOGGLE_PANEL); return 0; }
        if (s_tab == 2) {
            if (PtInRect(&s_btnNew, pt)) { app_command(CMD_NEW_TERMINAL); return 0; }
            if (PtInRect(&s_btnKill, pt)) { term_kill(); return 0; }
            if (PtInRect(&s_btnClear, pt)) { term_clear(); return 0; }
        }
        dc = GetDC(w);
        if (!dc) return 0;
        for (i = 0; i < 3; i++) {
            int tw = text_w(dc, g_fUISmall, tab_name(i), -1);
            if (mx >= x && mx < x + tw) { panel_show_tab(i); break; }
            x += tw + 20;
        }
        ReleaseDC(w, dc);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt;
        RECT rc;
        TRACKMOUSEEVENT t;
        int hb = -1, hr = -1;
        pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
        GetClientRect(w, &rc);

        if (PtInRect(&s_btnNew, pt)) hb = 0;
        else if (PtInRect(&s_btnKill, pt)) hb = 1;
        else if (PtInRect(&s_btnClose, pt)) hb = 2;
        else if (PtInRect(&s_btnClear, pt)) hb = 3;
        if (hb != 2 && s_tab != 2) hb = -1;      /* only close is on every tab */

        if (list_shown() && pt.y > HEADER_H && pt.x >= rc.right - LIST_W) {
            hr = (int)(pt.y - HEADER_H) / 22;
            if (hr < 0 || hr >= s_nterms) hr = -1;
        }
        if (hb != s_hoverBtn || hr != s_hoverRow) {
            s_hoverBtn = hb; s_hoverRow = hr;
            InvalidateRect(w, NULL, FALSE);
        }
        t.cbSize = sizeof t; t.dwFlags = TME_LEAVE; t.hwndTrack = w; t.dwHoverTime = 0;
        TrackMouseEvent(&t);
        return 0;
    }

    case WM_MOUSELEAVE:
        s_hoverBtn = s_hoverRow = -1;
        InvalidateRect(w, NULL, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        Term *t = curterm();
        if (t) {
            t->scroll += GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 3 : -3;
            if (t->scroll < 0) t->scroll = 0;
            if (t->scroll > t->rows) t->scroll = t->rows;
            InvalidateRect(w, NULL, FALSE);
        }
        return 0;
    }

    default:
        return DefWindowProc(w, m, wp, lp);
    }
}

void panel_register(void)
{
    WNDCLASS wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = PanelProc;
    wc.lpszClassName = "XPPanel";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
}
