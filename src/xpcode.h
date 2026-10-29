/* XPico Code - a VS Code style editor for Windows XP, written in plain Win32 C.
 * Built with Tiny C Compiler: see build.bat */
#ifndef XPCODE_H
#define XPCODE_H

#ifdef MAC_OS
/* Mac OS 9 / Carbon build: the Win32 API the editor is written against is
 * provided by the shim in src/mac-carbon. Mac OS 9 has no shell, pipes or processes,
 * so the integrated terminal is left out of this build. */
#include "mac-carbon/win32.h"
#define HAVE_TERMINAL 0
/* Runtime, not compile time: the same Carbon binary runs on Mac OS 9, which has
 * no shell at all, and on Mac OS X, where the xpcode-termd helper provides one.
 * Everything that depends on running a command is hidden when there is none. */
#define TERMINAL_AVAILABLE() mac_is_osx()
#define FILE_MANAGER_NAME "Finder"
/* How the editor describes itself in the welcome screen and the About box.
 * The Windows build is the plain-Win32 one; this build reaches the same Win32
 * surface through the shim in src/mac, so saying "Win32" to the user would be
 * true of the source and meaningless on the machine. */
#define PLATFORM_BLURB "for legacy MacOS"
#define TEXT_ENCODING_NAME "Mac OS Roman"
/* Classic Mac OS terminates lines with a bare CR. Files are written that way
 * so other Mac applications can read them; read_file normalises all three
 * conventions to LF on the way in. */
#define LINE_ENDING      "\r"
#define LINE_ENDING_LEN  1
#define LINE_ENDING_NAME "CR"
#define BUILD_SCRIPT  "build.sh"
#define BUILD_COMMAND "cd '%s' && sh build.sh"
#define TERMINAL_BLURB "A real Unix shell, right inside the editor."
#define RUN_BLURB "F5 compiles C with cc and runs Python."
#define RUN_RECIPES mac_run_recipes
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#else
#define _WIN32_WINNT 0x0501
#define WINVER 0x0501
#define _WIN32_IE 0x0600
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#define HAVE_TERMINAL 1
#define TERMINAL_AVAILABLE() 1
#define FILE_MANAGER_NAME "Windows Explorer"
#define PLATFORM_BLURB "in plain Win32"
#define TEXT_ENCODING_NAME "Windows 1252"
#define LINE_ENDING      "\r\n"
#define LINE_ENDING_LEN  2
#define LINE_ENDING_NAME "CRLF"
#define BUILD_SCRIPT  "build.bat"
#define BUILD_COMMAND "cd /d \"%s\" && call build.bat"
#define TERMINAL_BLURB "A real cmd.exe, right inside the editor."
#define RUN_BLURB "F5 compiles C with TCC, runs Python and batch."
#define RUN_RECIPES win_run_recipes
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* Path separator. Classic Mac OS uses ':' and has no drive letters, so the
 * editor's own path building and splitting has to follow the host convention
 * or file names come out as whole paths in the tabs and explorer. */
#ifdef MAC_OS
#define PATH_SEP   ':'
#define PATH_SEPS  ":"
#define IS_PATH_SEP(c) ((c) == ':')
#else
#define PATH_SEP   '\\'
#define PATH_SEPS  "\\"
#define IS_PATH_SEP(c) ((c) == '\\' || (c) == '/')
#endif

#define APP_NAME "XPico Code"
#define APP_VERSION "1.1.1"
#define IDI_APP 1        /* icon resource embedded by tools/embed_resources.py */
#define HEX(c) RGB(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255)

/* ---- Dark+ palette ---- */
#define C_EDITOR_BG      HEX(0x1E1E1E)
#define C_SIDEBAR_BG     HEX(0x252526)
#define C_ACTIVITY_BG    HEX(0x333333)
#define C_TAB_INACTIVE   HEX(0x2D2D2D)
#define C_TABSTRIP_BG    HEX(0x252526)
#define C_BORDER         HEX(0x444444)
#define C_STATUS_BG      HEX(0x007ACC)
#define C_STATUS_NOFOLDER HEX(0x68217A)
#define C_TEXT           HEX(0xD4D4D4)
#define C_TEXT_BRIGHT    HEX(0xFFFFFF)
#define C_TEXT_DIM       HEX(0x858585)
#define C_TEXT_MUTED     HEX(0x969696)
#define C_SIDEBAR_TEXT   HEX(0xCCCCCC)
#define C_ACCENT         HEX(0x007ACC)
#define C_LINK           HEX(0x3794FF)
#define C_SELECTION      HEX(0x264F78)
#define C_SEL_INACTIVE   HEX(0x3A3D41)
#define C_LIST_HOVER     HEX(0x2A2D2E)
#define C_LIST_ACTIVE    HEX(0x37373D)
#define C_LIST_FOCUS     HEX(0x094771)
#define C_INPUT_BG       HEX(0x3C3C3C)
#define C_WIDGET_BG      HEX(0x252526)
#define C_ERROR          HEX(0xF14C4C)
#define C_WARNING        HEX(0xCCA700)
#define C_SUCCESS        HEX(0x89D185)

/* ---- private messages ---- */
#define WM_APP_TERMDATA  (WM_APP + 1)   /* wParam = Term*, lParam = TermChunk* */
#define WM_APP_TERMEXIT  (WM_APP + 2)   /* wParam = Term* */

/* ---- commands ---- */
enum {
    CMD_NONE = 0,
    CMD_NEW_FILE = 100, CMD_OPEN_FILE, CMD_OPEN_FOLDER, CMD_SAVE, CMD_SAVE_AS, CMD_SAVE_ALL,
    CMD_CLOSE_EDITOR, CMD_CLOSE_ALL, CMD_CLOSE_FOLDER, CMD_EXIT,
    CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_FIND, CMD_REPLACE, CMD_TOGGLE_COMMENT,
    CMD_SELECT_ALL, CMD_MOVE_LINE_UP, CMD_MOVE_LINE_DOWN, CMD_COPY_LINE_DOWN, CMD_DELETE_LINE,
    CMD_PALETTE, CMD_QUICK_OPEN, CMD_GOTO_LINE, CMD_SHOW_EXPLORER, CMD_SHOW_SEARCH,
    CMD_SHOW_RUN, CMD_TOGGLE_SIDEBAR, CMD_TOGGLE_PANEL, CMD_TOGGLE_MINIMAP,
    CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET, CMD_NEXT_EDITOR, CMD_PREV_EDITOR,
    CMD_RUN_FILE, CMD_BUILD_TASK, CMD_NEW_TERMINAL, CMD_KILL_TERMINAL, CMD_CLEAR_TERMINAL,
    CMD_FOCUS_TERMINAL, CMD_SHOW_PROBLEMS, CMD_SHOW_OUTPUT, CMD_WELCOME, CMD_SHORTCUTS, CMD_ABOUT,
    CMD_REFRESH_EXPLORER, CMD_COLLAPSE_EXPLORER, CMD_NEW_FOLDER_ITEM, CMD_FOCUS_EDITOR,
    CMD_WEBSITE, CMD_DONATE
};

/* ---- icons drawn with GDI ---- */
enum {
    ICON_FILES, ICON_SEARCH, ICON_RUN, ICON_GEAR,
    ICON_CHEVRON_RIGHT, ICON_CHEVRON_DOWN, ICON_CLOSE, ICON_PLUS, ICON_TRASH, ICON_NEW_FILE,
    ICON_NEW_FOLDER, ICON_REFRESH, ICON_COLLAPSE, ICON_ERROR, ICON_WARNING,
    ICON_CHEVRON_UP, ICON_DOT, ICON_SPLIT, ICON_ELLIPSIS, ICON_ARROW_UP, ICON_ARROW_DOWN,
    ICON_TERMINAL, ICON_PLAY
};

/* ---- languages ---- */
enum { LANG_PLAIN, LANG_C, LANG_PY, LANG_JS, LANG_BAT, LANG_INI, LANG_HTML, LANG_MD };

/* ---- globals (main.c) ---- */
extern HINSTANCE g_hinst;
extern HWND g_hwndMain, g_hwndActivity, g_hwndSidebar, g_hwndEditor, g_hwndPanel, g_hwndStatus,
            g_hwndPalette;
extern HFONT g_fUI, g_fUIBold, g_fUISmall, g_fCode, g_fTitle, g_fHeading, g_fCodeSmall;
extern int g_codeCW, g_codeCH, g_codeSize;
extern char g_root[MAX_PATH];
extern int g_sidebarVisible, g_panelVisible, g_sideView, g_minimap;
extern char g_exeDir[MAX_PATH];

/* ---- util (main.c) ---- */
void fill(HDC dc, int x, int y, int w, int h, COLORREF c);
void fill_rc(HDC dc, const RECT *r, COLORREF c);
void frame(HDC dc, int x, int y, int w, int h, COLORREF c);
void hline(HDC dc, int x, int y, int w, COLORREF c);
void vline(HDC dc, int x, int y, int h, COLORREF c);
void text_at(HDC dc, int x, int y, const char *s, int n, COLORREF c, HFONT f);
int  text_w(HDC dc, HFONT f, const char *s, int n);
void text_ellipsis(HDC dc, int x, int y, int maxw, const char *s, COLORREF c, HFONT f);
void draw_icon(HDC dc, int icon, int cx, int cy, int sz, COLORREF c);
void file_badge(HDC dc, const char *name, int isdir, int open, int x, int y);
int  sfmt(char *buf, int n, const char *f, ...);
const char *path_name(const char *p);
const char *path_ext(const char *p);
int  path_rel(const char *full, char *out, int n);
int  file_exists(const char *p);
int  dir_exists(const char *p);
int  lang_from_path(const char *p);
const char *lang_name(int lang);
void out_log(const char *f, ...);
void app_command(int cmd);
void app_layout(void);
void app_update_title(void);
void app_open_folder(const char *path);
int  app_hotkey(MSG *m);
char *read_file(const char *path, int *len);
int  fuzzy_score(const char *pattern, const char *s);
/* 1 for commands that need a shell behind them, so the menus, the command
 * palette and the welcome screen can all hide the same set on a system that
 * has none. */
int  cmd_needs_terminal(int cmd);
/* What F5 does per file type, listed in the Run side bar. NULL-terminated. */
extern const char *const RUN_RECIPES[];

/* ---- editor.c ---- */
typedef struct Doc Doc;
void  editor_register(void);
Doc  *editor_open(const char *path);
void  editor_new_untitled(void);
void  editor_goto(int line, int col, int sellen);
int   editor_save(int idx, int saveas);
void  editor_save_all(void);
int   editor_close(int idx);
int   editor_close_all(void);
int   editor_count(void);
int   editor_active_index(void);
const char *editor_doc_path(int idx);
void  editor_doc_label(int idx, char *buf, int n);
int   editor_doc_dirty(int idx);
void  editor_activate(int idx);
void  editor_command(int cmd);
void  editor_check_disk(void);
void  editor_font_changed(void);
void  editor_status(int *line, int *col, int *lang, int *hasdoc, int *nlines);
void  editor_find_show(int replace);
void  editor_focus(void);

/* ---- explorer.c (side bar) ---- */
void sidebar_register(void);
void sidebar_set_root(const char *root);
void sidebar_set_view(int view);
void sidebar_refresh(void);
void sidebar_tick(void);
void sidebar_collapse_all(void);
void sidebar_reveal(const char *path);
void sidebar_new_item(int folder);
int  sidebar_list_files(char ***out);   /* every file under root, relative paths */

/* ---- activity bar (main.c) ---- */
void activity_register(void);

/* ---- terminal.c (panel) ---- */
void panel_register(void);
void panel_show_tab(int tab);  /* 0 problems, 1 output, 2 terminal */
void panel_output(const char *line);
void term_new(const char *cwd);
void term_kill(void);
void term_clear(void);
void term_send_line(const char *cmd);
void term_focus(void);
void term_shutdown(void);
int  problems_count(int *errors, int *warnings);

/* ---- palette.c ---- */
typedef void (*InputCallback)(const char *text, void *ctx);
void palette_register(void);
void palette_open(int mode);    /* 0 files, 1 commands, 2 goto line */
void palette_prompt(const char *placeholder, const char *initial, InputCallback cb, void *ctx);
void palette_close(void);
int  palette_is_open(void);
const char *command_name(int cmd);
const char *command_key(int cmd);

/* ---- status bar (main.c) ---- */
void status_register(void);
void status_refresh(void);

#endif
