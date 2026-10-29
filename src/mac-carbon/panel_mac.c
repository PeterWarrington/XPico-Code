/* The bottom panel for the Mac build: Problems, Output and a Terminal.
 *
 * This replaces terminal.c, which drives cmd.exe over Win32 pipes. Here the
 * shell lives in the xpcode-termd helper and is reached over TCP (mac-carbon/term_ot.c),
 * so this file is the view: a scrollback buffer, a small ANSI parser and the
 * keyboard handling. On Mac OS 9 the connection fails and the Terminal tab
 * explains why.
 *
 * Built with the app sources: it uses xpcode.h's drawing helpers, not the
 * Toolbox. */
#include "../xpcode.h"
#include <string.h>

#define OUT_MAX   400
#define TERM_ROWS 600
#define TERM_COLS 200
#define LINE_MAX  256

/* ---- output log ---- */
static char s_out[OUT_MAX][LINE_MAX];
static int  s_outCount, s_outFirst;

/* ---- terminal screen: a scrollback of fixed-width rows ---- */
static char s_scr[TERM_ROWS][TERM_COLS + 1];
static int  s_rows = 1;            /* rows in use */
static int  s_cx, s_cy;            /* cursor, cy indexes s_scr */
static int  s_scroll;              /* rows scrolled back from the bottom */
static int  s_tab, s_connected, s_exited;

/* mac-carbon/term_ot.c */
int  term_transport_open(void);
void term_transport_close(void);
void term_transport_send(const char *data, int n);
void term_transport_resize(int cols, int rows);
void term_transport_cwd(const char *path);
void term_transport_kill(void);
void term_transport_poll(void);
const char *term_transport_error(void);
int  term_transport_state(void);

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
    if (!TERMINAL_AVAILABLE()) return;
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

static void term_newline(void)
{
    s_cx = 0;
    s_cy++;
    if (s_cy >= TERM_ROWS) {
        /* drop the oldest row and keep the cursor on the last one */
        memmove(s_scr[0], s_scr[1], (size_t)(TERM_ROWS - 1) * (TERM_COLS + 1));
        s_cy = TERM_ROWS - 1;
        s_scr[s_cy][0] = 0;
    }
    if (s_cy >= s_rows) s_rows = s_cy + 1;
    s_scr[s_cy][0] = 0;
}

static void term_putc(char c)
{
    int len;
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { s_cx = 0; return; }
    if (c == '\b') { if (s_cx > 0) s_cx--; return; }
    if (c == '\t') {
        int next = (s_cx + 8) & ~7;
        while (s_cx < next && s_cx < TERM_COLS) term_putc(' ');
        return;
    }
    if ((unsigned char)c < 32) return;
    if (s_cx >= TERM_COLS) term_newline();

    len = (int)strlen(s_scr[s_cy]);
    while (len < s_cx) s_scr[s_cy][len++] = ' ';
    s_scr[s_cy][s_cx] = c;
    if (s_cx + 1 > len) s_scr[s_cy][s_cx + 1] = 0;
    s_cx++;
}

/* A deliberately small ANSI handler: enough for a login shell's prompt and
 * colour codes, which are discarded rather than rendered. */
void term_on_data(const char *data, int n)
{
    static int inEsc, inCsi;
    int i;
    for (i = 0; i < n; i++) {
        char c = data[i];
        if (inEsc) {
            if (!inCsi) {
                if (c == '[') { inCsi = 1; continue; }
                inEsc = 0;                       /* a two-character sequence */
                continue;
            }
            /* CSI runs until a final byte in 0x40-0x7E */
            if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) {
                if (c == 'J' || c == 'K') {
                    s_scr[s_cy][s_cx] = 0;       /* erase to end of line */
                }
                inEsc = inCsi = 0;
            }
            continue;
        }
        if (c == 0x1B) { inEsc = 1; inCsi = 0; continue; }
        term_putc(c);
    }
    s_scroll = 0;
    if (g_hwndPanel) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

void term_on_exit(int status)
{
    char msg[64];
    sfmt(msg, sizeof msg, "[shell exited: %d]", status);
    term_on_data("\r\n", 2);
    term_on_data(msg, (int)strlen(msg));
    term_on_data("\r\n", 2);
    s_exited = 1;
    s_connected = 0;
}

/* ---------------------------------------------------------- the term_ API */

static void idle_poll(void) { term_transport_poll(); }
void shim_set_idle(void (*fn)(void));

void term_new(const char *cwd)
{
    /* Startup diagnostics. The editor's whole layout is derived from the code
     * font's cell size, and a zero there leaves the window blank; printing it
     * where it can be read on the machine itself beats guessing. */
    {
        char diag[128];
        RECT rc;
        sfmt(diag, sizeof diag, "font cell: cw=%d ch=%d  size=%d",
             g_codeCW, g_codeCH, g_codeSize);
        panel_output(diag);
        if (g_hwndEditor) {
            GetClientRect(g_hwndEditor, &rc);
            sfmt(diag, sizeof diag, "editor rect: %dx%d   main: %dx%d",
                 (int)rc.right, (int)rc.bottom, (int)0, (int)0);
            if (g_hwndMain) {
                RECT mr;
                GetClientRect(g_hwndMain, &mr);
                sfmt(diag, sizeof diag, "editor rect: %dx%d   main: %dx%d",
                     (int)rc.right, (int)rc.bottom, (int)mr.right, (int)mr.bottom);
            }
            panel_output(diag);
        }
    }

    s_exited = 0;
    /* Mac OS 9 has no Unix shell for the helper to run, so there is nothing to
     * connect to; don't spin up OpenTransport just to fail. */
    if (!mac_is_osx()) { s_connected = 0; return; }

    if (!s_connected) {
        if (!term_transport_open()) {
            s_connected = 0;
            return;                            /* the panel shows the reason */
        }
        s_connected = 1;
        shim_set_idle(idle_poll);
        s_rows = 1; s_cx = s_cy = 0; s_scr[0][0] = 0;
    }
    if (cwd && *cwd) term_transport_cwd(cwd);
}

void term_send_line(const char *cmd)
{
    if (!s_connected) { panel_show_tab(2); return; }
    term_transport_send(cmd, (int)strlen(cmd));
    term_transport_send("\n", 1);
    panel_show_tab(2);
}

void term_kill(void)
{
    if (s_connected) term_transport_kill();
}

void term_clear(void)
{
    if (s_tab == 2) { s_rows = 1; s_cx = s_cy = 0; s_scr[0][0] = 0; s_scroll = 0; }
    else { s_outCount = s_outFirst = 0; }
    if (g_hwndPanel) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

void term_focus(void)
{
    panel_show_tab(2);
    if (g_hwndPanel) SetFocus(g_hwndPanel);
}

void term_shutdown(void)
{
    if (s_connected) { term_transport_kill(); term_transport_close(); }
    s_connected = 0;
}

/* ---------------------------------------------------------------- painting */

static int body_top(void) { return 31; }

static void paint_terminal(HDC dc, const RECT *rc)
{
    int rows, y, i, first;

    if (!s_connected) {
        int line = body_top() + 10;
        int step = g_codeCH + 4;

        if (!mac_is_osx()) {
            char ver[32], msg[96];
            mac_system_version_string(ver, sizeof ver);
            sfmt(msg, sizeof msg, "The terminal is not supported on Mac OS %s.", ver);
            text_at(dc, 12, line, msg, -1, C_TEXT, g_fUI);
            text_at(dc, 12, line + step,
                    "Mac OS 9 has no Unix shell to run commands in.",
                    -1, C_TEXT_DIM, g_fUI);
            text_at(dc, 12, line + 2 * step,
                    "Everything else in the editor works; only this tab is unavailable.",
                    -1, C_TEXT_DIM, g_fUI);
            return;
        }

        /* On Mac OS X the shell exists, so the only reason to be here is that
         * the helper is not running. */
        text_at(dc, 12, line, "The terminal helper is not running.", -1, C_TEXT, g_fUI);
        text_at(dc, 12, line + step, term_transport_error(), -1, C_TEXT_DIM, g_fUI);
        text_at(dc, 12, line + 2 * step,
                "Start xpcode-termd, then choose Terminal > New Terminal.",
                -1, C_TEXT_DIM, g_fUI);
        return;
    }

    rows = (rc->bottom - body_top() - 4) / (g_codeCH > 0 ? g_codeCH : 12);
    first = s_rows - rows - s_scroll;
    if (first < 0) first = 0;

    y = body_top() + 2;
    for (i = first; i < s_rows && i < first + rows; i++) {
        text_at(dc, 8, y, s_scr[i], -1, C_TEXT, g_fCode);
        /* block cursor on the active row */
        if (i == s_cy && !s_exited) {
            int cx = 8 + text_w(dc, g_fCode, s_scr[i], s_cx);
            fill(dc, cx, y, 2, g_codeCH, C_TEXT_BRIGHT);
        }
        y += g_codeCH;
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
    hline(dc, 0, 30, rc.right, C_BORDER);

    if (s_tab == 0) {
        text_at(dc, 12, body_top() + 10, "No problems have been detected.",
                -1, C_TEXT_DIM, g_fUI);
    } else if (s_tab == 1) {
        int rows = (rc.bottom - body_top()) / (g_codeCH > 0 ? g_codeCH : 12);
        int y = body_top() + 2;
        for (i = s_outCount - rows; i < s_outCount; i++) {
            if (i < 0) { y += g_codeCH; continue; }
            text_at(dc, 12, y, s_out[(s_outFirst + i) % OUT_MAX], -1, C_TEXT, g_fCode);
            y += g_codeCH;
        }
    } else {
        paint_terminal(dc, &rc);
    }
    EndPaint(w, &ps);
}

/* ---------------------------------------------------------------- input */

static void send_key(WPARAM vk)
{
    /* the few keys a shell needs that are not plain characters */
    switch (vk) {
    case VK_UP:    term_transport_send("\033[A", 3); break;
    case VK_DOWN:  term_transport_send("\033[B", 3); break;
    case VK_RIGHT: term_transport_send("\033[C", 3); break;
    case VK_LEFT:  term_transport_send("\033[D", 3); break;
    case VK_HOME:  term_transport_send("\033[H", 3); break;
    case VK_END:   term_transport_send("\033[F", 3); break;
    case VK_DELETE:term_transport_send("\033[3~", 4); break;
    case VK_BACK:  term_transport_send("\177", 1); break;
    case VK_RETURN:term_transport_send("\r", 1); break;
    case VK_TAB:   term_transport_send("\t", 1); break;
    case VK_ESCAPE:term_transport_send("\033", 1); break;
    default: break;
    }
}

static LRESULT CALLBACK PanelProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: panel_paint(w); return 0;
    case WM_ERASEBKGND: return 1;

    case WM_SIZE:
        if (s_connected) {
            RECT rc;
            GetClientRect(w, &rc);
            term_transport_resize(g_codeCW > 0 ? (rc.right - 16) / g_codeCW : 80,
                                  g_codeCH > 0 ? (rc.bottom - body_top()) / g_codeCH : 24);
        }
        InvalidateRect(w, NULL, FALSE);
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(w, NULL, FALSE);
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_KEYDOWN:
        if (s_tab == 2 && s_connected) send_key(wp);
        return 0;

    case WM_CHAR:
        if (s_tab == 2 && s_connected) {
            char c = (char)wp;
            if (c != '\r' && c != '\t' && c != 8 && (unsigned char)c >= 32)
                term_transport_send(&c, 1);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp), i, x = 12;
        HDC dc;
        SetFocus(w);
        if (my > 30) return 0;
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

    case WM_MOUSEWHEEL: {
        s_scroll += GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 3 : -3;
        if (s_scroll < 0) s_scroll = 0;
        if (s_scroll > s_rows) s_scroll = s_rows;
        InvalidateRect(w, NULL, FALSE);
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
