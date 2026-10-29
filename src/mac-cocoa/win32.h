/* Win32 compatibility shim for Mac OS X 10.4 / Cocoa.
 *
 * The same idea as src/mac-carbon, aimed at a different system: XPico Code is
 * written against Win32, so this header declares the subset the editor uses
 * (~150 calls) and the shim's own sources implement it -- here on top of
 * Cocoa, Quartz and POSIX rather than QuickDraw and the Carbon Event Manager.
 * src/xpcode.h includes this rather than <windows.h> when MAC_COCOA is set.
 *
 * What this build has that the Carbon one does not: a real terminal. Mac OS X
 * has fork(), so src/mac-cocoa/term_pty.c runs the shell in a pty inside the
 * editor's own process and the xpcode-termd helper is not needed.
 *
 * The window model is unchanged from the Carbon port: one NSWindow with one
 * NSView, and the app's six child HWNDs are lightweight rectangles drawn into
 * that view, each with its own clipped, origin-shifted HDC. */
#ifndef MAC_COCOA_WIN32_H
#define MAC_COCOA_WIN32_H

/* Cocoa.h is deliberately NOT included here. The app code includes only this
 * header, so it never sees the Objective-C namespace; the shim's .m files
 * include Cocoa.h themselves, before this header.
 *
 * The renames below are names that also exist in the frameworks with different
 * signatures. Most of them come in through <ApplicationServices/...>, which
 * AppKit includes and which still declares QuickDraw on 10.4: SetRect,
 * PtInRect, LineTo, SetCursor and the Polygon type are all QuickDraw's. The
 * Window and Menu Manager names are not reachable from Cocoa, but they are
 * renamed too so that a file which does pull in Carbon still compiles.
 *
 * BOOL is the one that cannot be renamed away quietly: <objc/objc.h> defines
 * it as signed char, and a second typedef is an error rather than a shadowed
 * name. Making it a macro keeps one definition of the type across every
 * translation unit -- C and Objective-C alike -- so the ABI stays consistent.
 * The cost is that BOOL means int inside the shim's .m files too, which is why
 * the Objective-C methods there that override AppKit ones are written as
 * "signed char": that is what AppKit declares them to return. */
#define BOOL            w32_BOOL
#define ShowWindow      w32_ShowWindow
#define MoveWindow      w32_MoveWindow
#define SetRect         w32_SetRect
#define SetRectEmpty    w32_SetRectEmpty
#define IsRectEmpty     w32_IsRectEmpty
#define PtInRect        w32_PtInRect
#define AppendMenu      w32_AppendMenu
#define LineTo          w32_LineTo
#define IsWindowVisible w32_IsWindowVisible
#define Polygon         w32_Polygon
#define SetCursor       w32_SetCursor

#include <stddef.h>
#include <stdarg.h>

/* ---- scalar types ---- */
typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef unsigned long       DWORD;
typedef long                LONG;
typedef unsigned int        UINT;
typedef const char         *LPCSTR;
typedef char               *LPSTR;
typedef void               *LPVOID;
typedef long                LPARAM;
typedef unsigned long       WPARAM;
typedef long                LRESULT;
typedef unsigned long       COLORREF;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define WINAPI
#define CALLBACK
#define MAX_PATH 512            /* a POSIX path, not an HFS one */

/* ---- handles ---- */
typedef struct Win  *HWND;
typedef struct MDC  *HDC;
typedef struct MFont *HFONT;
typedef struct MBitmap *HBITMAP;
typedef struct MMenu *HMENU;
typedef void *HINSTANCE, *HANDLE, *HICON, *HCURSOR, *HBRUSH, *HPEN, *HGDIOBJ, *HDROP;

#define NULL_HANDLE ((HANDLE)0)
#define INVALID_HANDLE_VALUE ((HANDLE)-1)

/* ---- geometry ---- */
typedef struct { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct { LONG x, y; } POINT, *LPPOINT;
typedef struct { LONG cx, cy; } SIZE, *LPSIZE;

/* ---- colour ---- */
#define RGB(r, g, b) ((COLORREF)(((DWORD)(BYTE)(r)) | ((DWORD)(BYTE)(g) << 8) | ((DWORD)(BYTE)(b) << 16)))
#define GetRValue(c) ((BYTE)((c) & 0xFF))
#define GetGValue(c) ((BYTE)(((c) >> 8) & 0xFF))
#define GetBValue(c) ((BYTE)(((c) >> 16) & 0xFF))

#define LOWORD(l) ((WORD)((DWORD)(l) & 0xFFFF))
#define HIWORD(l) ((WORD)(((DWORD)(l) >> 16) & 0xFFFF))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))

/* ---- messages ---- */
#define WM_CREATE        0x0001
#define WM_DESTROY       0x0002
#define WM_SIZE          0x0005
#define WM_SETFOCUS      0x0007
#define WM_KILLFOCUS     0x0008
#define WM_PAINT         0x000F
#define WM_CLOSE         0x0010
#define WM_QUIT          0x0012
#define WM_ERASEBKGND    0x0014
#define WM_SETCURSOR     0x0020
#define WM_GETDLGCODE    0x0087
#define WM_SETICON       0x0080
#define WM_KEYDOWN       0x0100
#define WM_KEYUP         0x0101
#define WM_CHAR          0x0102
#define WM_SYSKEYDOWN    0x0104
#define WM_COMMAND       0x0111
#define WM_TIMER         0x0113
#define WM_CTLCOLOREDIT  0x0133
#define WM_MOUSEMOVE     0x0200
#define WM_LBUTTONDOWN   0x0201
#define WM_LBUTTONUP     0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN   0x0204
#define WM_MBUTTONDOWN   0x0207
#define WM_MOUSEWHEEL    0x020A
#define WM_MOUSELEAVE    0x02A3
#define WM_ACTIVATEAPP   0x001C
#define WM_DROPFILES     0x0233
#define WM_APP           0x8000

/* control notifications and styles that the three EDIT fields rely on */
#define EN_CHANGE        0x0300
#define ES_AUTOHSCROLL   0x0080
#define WS_CHILD         0x40000000L
#define WS_VISIBLE       0x10000000L
#define WS_CLIPSIBLINGS  0x04000000L
#define WS_CLIPCHILDREN  0x02000000L
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define WS_EX_ACCEPTFILES   0x00000010L
#define DLGC_WANTARROWS  0x0001
#define DLGC_WANTCHARS   0x0080

/* virtual keys: the editor tests these directly */
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B

/* ---- structs ---- */
typedef struct { UINT message; WPARAM wParam; LPARAM lParam; HWND hwnd; } MSG, *LPMSG;
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct {
    UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground;
    LPCSTR lpszMenuName, lpszClassName;
} WNDCLASS;
typedef struct { HDC hdc; BOOL fErase; RECT rcPaint; } PAINTSTRUCT;
typedef struct { LONG tmHeight, tmAscent, tmDescent, tmAveCharWidth, tmMaxCharWidth; } TEXTMETRIC;
typedef struct { DWORD cbSize, dwFlags; HWND hwndTrack; DWORD dwHoverTime; } TRACKMOUSEEVENT;

#define TME_LEAVE 0x00000002
#define ETO_OPAQUE 0x0002
#define TRANSPARENT 1
#define OPAQUE 2
#define SRCCOPY 0x00CC0020L

/* ---- window management (mac-cocoa/wnd.m) ---- */
BOOL    RegisterClass(const WNDCLASS *wc);
HWND    CreateWindowEx(DWORD exstyle, LPCSTR cls, LPCSTR name, DWORD style,
                       int x, int y, int w, int h, HWND parent, HMENU menu,
                       HINSTANCE inst, LPVOID param);
#define CreateWindow(c, n, s, x, y, w, h, p, m, i, l) \
        CreateWindowEx(0, c, n, s, x, y, w, h, p, m, i, l)
void    DestroyWindow(HWND w);
BOOL    IsWindow(HWND w);
BOOL    IsWindowVisible(HWND w);
void    ShowWindow(HWND w, int cmd);
void    MoveWindow(HWND w, int x, int y, int cw, int ch, BOOL repaint);
void    GetClientRect(HWND w, LPRECT r);
void    GetWindowRect(HWND w, LPRECT r);
void    InvalidateRect(HWND w, const RECT *r, BOOL erase);
void    UpdateWindow(HWND w);
LRESULT SendMessage(HWND w, UINT m, WPARAM wp, LPARAM lp);
BOOL    PostMessage(HWND w, UINT m, WPARAM wp, LPARAM lp);
LRESULT DefWindowProc(HWND w, UINT m, WPARAM wp, LPARAM lp);
HWND    SetFocus(HWND w);
HWND    GetFocus(void);
void    SetCapture(HWND w);
void    ReleaseCapture(void);
BOOL    TrackMouseEvent(TRACKMOUSEEVENT *e);
BOOL    GetMessage(LPMSG m, HWND w, UINT lo, UINT hi);
BOOL    TranslateMessage(const MSG *m);
LRESULT DispatchMessage(const MSG *m);
void    PostQuitMessage(int code);
BOOL    SetTimer(HWND w, UINT id, UINT ms, LPVOID proc);
void    ClientToScreen(HWND w, LPPOINT p);
void    ScreenToClient(HWND w, LPPOINT p);
void    GetCursorPos(LPPOINT p);
short   GetKeyState(int vk);
void    SetWindowText(HWND w, LPCSTR s);
int     GetWindowText(HWND w, LPSTR buf, int n);

/* ---- drawing (mac-cocoa/gdi.m) ---- */
HDC     BeginPaint(HWND w, PAINTSTRUCT *ps);
void    EndPaint(HWND w, const PAINTSTRUCT *ps);
HDC     GetDC(HWND w);
void    ReleaseDC(HWND w, HDC dc);
HDC     CreateCompatibleDC(HDC dc);
void    DeleteDC(HDC dc);
HBITMAP CreateCompatibleBitmap(HDC dc, int w, int h);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj);
BOOL    DeleteObject(HGDIOBJ obj);
BOOL    BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);
void    SetBkColor(HDC dc, COLORREF c);
void    SetTextColor(HDC dc, COLORREF c);
void    SetBkMode(HDC dc, int mode);
BOOL    ExtTextOut(HDC dc, int x, int y, UINT flags, const RECT *r, LPCSTR s, UINT n, const int *dx);
BOOL    TextOut(HDC dc, int x, int y, LPCSTR s, int n);
BOOL    GetTextExtentPoint32(HDC dc, LPCSTR s, int n, LPSIZE sz);
BOOL    GetTextMetrics(HDC dc, TEXTMETRIC *tm);
HFONT   CreateFont(int h, int w, int esc, int orient, int weight, DWORD italic,
                   DWORD underline, DWORD strikeout, DWORD charset, DWORD outprec,
                   DWORD clipprec, DWORD quality, DWORD pitch, LPCSTR face);
void    MoveToEx(HDC dc, int x, int y, LPPOINT old);
void    LineTo(HDC dc, int x, int y);
HPEN    CreatePen(int style, int width, COLORREF c);
HBRUSH  CreateSolidBrush(COLORREF c);
void    Rectangle(HDC dc, int l, int t, int r, int b);
void    Ellipse(HDC dc, int l, int t, int r, int b);
void    Polygon(HDC dc, const POINT *pts, int n);
void    Arc(HDC dc, int l, int t, int r, int b, int xs, int ys, int xe, int ye);
int     SaveDC(HDC dc);
BOOL    RestoreDC(HDC dc, int state);
int     IntersectClipRect(HDC dc, int l, int t, int r, int b);

/* caret: Quartz has none, so mac-cocoa/gdi.m blinks one itself */
BOOL    CreateCaret(HWND w, HBITMAP b, int cw, int ch);
BOOL    ShowCaret(HWND w);
BOOL    DestroyCaret(void);
BOOL    SetCaretPos(int x, int y);

/* ---- rects ---- */
/* Functions, not macros: QuickDraw's SetRect takes the same number of
 * arguments, so a macro here would silently rewrite Toolbox calls. */
void    SetRect(LPRECT r, int l, int t, int rt, int b);
void    SetRectEmpty(LPRECT r);
BOOL    IsRectEmpty(const RECT *r);
BOOL    PtInRect(const RECT *r, POINT p);

/* ---- system / files (mac-cocoa/sys.m) ---- */
UINT    GetPrivateProfileInt(LPCSTR sec, LPCSTR key, int def, LPCSTR file);
DWORD   GetPrivateProfileString(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR buf, DWORD n, LPCSTR file);
BOOL    WritePrivateProfileString(LPCSTR sec, LPCSTR key, LPCSTR val, LPCSTR file);
DWORD   GetFileAttributes(LPCSTR path);
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10
BOOL    CreateDirectory(LPCSTR path, LPVOID sa);
BOOL    MoveFile(LPCSTR from, LPCSTR to);
DWORD   GetFullPathName(LPCSTR name, DWORD n, LPSTR buf, LPSTR *part);
DWORD   GetModuleFileName(HINSTANCE i, LPSTR buf, DWORD n);
DWORD   GetTickCount(void);
int     MessageBox(HWND w, LPCSTR text, LPCSTR caption, UINT type);
#define MB_OK 0
#define MB_OKCANCEL 1
#define MB_YESNO 4
#define MB_YESNOCANCEL 3
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7

/* directory enumeration, mapped onto opendir/readdir */
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char  cFileName[MAX_PATH], cAlternateFileName[14];
} WIN32_FIND_DATA;
HANDLE  FindFirstFile(LPCSTR pattern, WIN32_FIND_DATA *fd);
BOOL    FindNextFile(HANDLE h, WIN32_FIND_DATA *fd);
BOOL    FindClose(HANDLE h);
LONG    CompareFileTime(const FILETIME *a, const FILETIME *b);

/* clipboard, over NSPasteboard */
BOOL    OpenClipboard(HWND w);
BOOL    CloseClipboard(void);
BOOL    EmptyClipboard(void);
HANDLE  GetClipboardData(UINT fmt);
HANDLE  SetClipboardData(UINT fmt, HANDLE mem);
#define CF_TEXT 1

/* menus, over NSMenu */
HMENU   CreateMenu(void);
HMENU   CreatePopupMenu(void);
BOOL    AppendMenu(HMENU m, UINT flags, UINT id, LPCSTR text);
BOOL    DestroyMenu(HMENU m);
BOOL    TrackPopupMenu(HMENU m, UINT flags, int x, int y, int reserved, HWND w, LPVOID r);
#define MF_STRING 0x0000
#define MF_POPUP 0x0010
#define MF_SEPARATOR 0x0800
#define MF_GRAYED 0x0001
#define MF_CHECKED 0x0008
#define TPM_LEFTALIGN 0x0000
#define TPM_RETURNCMD 0x0100

/* file dialogs, over NSOpenPanel and NSSavePanel */
BOOL    MacChooseFile(char *path, int n, BOOL save);
BOOL    MacChooseFolder(char *path, int n);

/* installs a callback run once per event-loop pass (mac-cocoa/wnd.m) */
void    shim_set_idle(void (*fn)(void));

/* ---- the shim's own helpers ---- */
void    shim_child_rect(HWND w, LPRECT r);
HWND    shim_parent(HWND w);
HWND    shim_root_window(void);
int     shim_child_id(HWND w);
/* Always 1 here: this binary only runs on Mac OS X. It stays because the app's
 * TERMINAL_AVAILABLE() and the shared #ifdef MAC_OS code paths call it. */
int     mac_is_osx(void);
int     mac_delete_path(const char *path);
int     mac_lang_sniff(const char *path, int lang_plain, int lang_c, int lang_py,
                       int lang_js, int lang_html, int lang_md);
int     mac_reveal_in_finder(const char *path);
void    mac_install_drop_handler(void);
void    mac_drop_files(const char *const *paths, int n);
void    mac_drop_release(void *d);
void   *mac_watch_open(const char *path);
int     mac_watch_changed(void *h);
void    mac_watch_close(void *h);
int     mac_set_file_type(const char *path, unsigned long type, unsigned long creator);
void    mac_system_version_string(char *buf, int n);

/* ---- the rest of the surface (mac-cocoa/compat.m) ---- */
typedef long LONG_PTR;

/* cursors and icons: the IDC_/IDI_ values are the Win32 ones, so that
 * mac-cocoa/compat.m can tell which cursor was asked for. */
#define IDC_ARROW    ((LPCSTR)32512)
#define IDC_IBEAM    ((LPCSTR)32513)
#define IDC_SIZEWE   ((LPCSTR)32644)
#define IDC_SIZENS   ((LPCSTR)32645)
#define IDC_HAND     ((LPCSTR)32649)
#define IDI_APPLICATION ((LPCSTR)32512)
#define MAKEINTRESOURCE(i) ((LPCSTR)(DWORD)(i))
#define IMAGE_ICON 1
#define ICON_SMALL 0

/* ShowWindow commands */
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOW 5
#define SW_SHOWMAXIMIZED 3

/* GetSystemMetrics indices */
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1

/* window/class long offsets: only the window proc is ever replaced */
#define GWLP_WNDPROC (-4)
#define GWL_WNDPROC  (-4)

/* edit control messages the find bar and palette use */
#define EM_SETSEL 0x00B1

#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))

/* mouse button flags in a WM_MOUSEMOVE wParam */
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT   0x0004
#define MK_CONTROL 0x0008

/* generic file access, for CreateFile */
#define GENERIC_READ  0x80000000UL
#define GENERIC_WRITE 0x40000000UL
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define GMEM_MOVEABLE 2
#define INFINITE 0xFFFFFFFFUL
#define WAIT_OBJECT_0 0

typedef long INT_PTR;
typedef unsigned long UINT_PTR;
typedef HANDLE HGLOBAL;
typedef void *LPITEMIDLIST;

LONG_PTR SetWindowLongPtr(HWND w, int index, LONG_PTR v);
LRESULT  CallWindowProc(WNDPROC proc, HWND w, UINT m, WPARAM wp, LPARAM lp);

/* ---- structs the app fills in directly ---- */

typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
#define GetFileExInfoStandard 0

typedef struct {
    DWORD lStructSize;
    HWND  hwndOwner;
    HANDLE hInstance;
    LPCSTR lpstrFilter, lpstrCustomFilter;
    DWORD nMaxCustFilter, nFilterIndex;
    LPSTR  lpstrFile;
    DWORD  nMaxFile;
    LPSTR  lpstrFileTitle;
    DWORD  nMaxFileTitle;
    LPCSTR lpstrInitialDir, lpstrTitle;
    DWORD  Flags;
    WORD   nFileOffset, nFileExtension;
    LPCSTR lpstrDefExt;
    LPARAM lCustData;
    LPVOID lpfnHook;
    LPCSTR lpTemplateName;
} OPENFILENAME;

#define OFN_EXPLORER         0x00080000
#define OFN_FILEMUSTEXIST    0x00001000
#define OFN_PATHMUSTEXIST    0x00000800
#define OFN_OVERWRITEPROMPT  0x00000002
#define OFN_ALLOWMULTISELECT 0x00000200
#define OFN_HIDEREADONLY     0x00000004

typedef struct {
    HWND   hwndOwner;
    LPITEMIDLIST pidlRoot;
    LPSTR  pszDisplayName;
    LPCSTR lpszTitle;
    UINT   ulFlags;
    LPVOID lpfn;
    LPARAM lParam;
    int    iImage;
} BROWSEINFO;

#define BIF_RETURNONLYFSDIRS 0x0001
#define BIF_NEWDIALOGSTYLE   0x0040
#define BFFM_INITIALIZED     1
#define BFFM_SETSELECTION    0x0467
#define CSIDL_APPDATA        0x001A

typedef struct {
    HWND  hwnd;
    UINT  wFunc;
    LPCSTR pFrom, pTo;
    WORD  fFlags;
    BOOL  fAnyOperationsAborted;
    LPVOID hNameMappings;
    LPCSTR lpszProgressTitle;
} SHFILEOPSTRUCT;

#define FO_DELETE            3
#define FOF_ALLOWUNDO        0x0040
#define FOF_NOCONFIRMATION   0x0010
#define FOF_SILENT           0x0004

typedef struct {
    DWORD  cbSize;
    HWND   hwndOwner;
    HANDLE hInstance;
    LPCSTR lpszText, lpszCaption;
    DWORD  dwStyle;
    LPCSTR lpszIcon;
    DWORD  dwContextHelpId;
    LPVOID lpfnMsgBoxCallback;
    DWORD  dwLanguageId;
} MSGBOXPARAMS;
#define MB_USERICON 0x80

typedef struct {
    UINT  length, flags, showCmd;
    POINT ptMinPosition, ptMaxPosition;
    RECT  rcNormalPosition;
} WINDOWPLACEMENT;

typedef struct {
    DWORD biSize;
    LONG  biWidth, biHeight;
    WORD  biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG  biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;

typedef struct {
    BITMAPINFOHEADER bmiHeader;
    DWORD bmiColors[1];
} BITMAPINFO;

#define BI_RGB 0
#define DIB_RGB_COLORS 0

typedef struct { DWORD dwSize, dwICC; } INITCOMMONCONTROLSEX;
#define ICC_WIN95_CLASSES 0x000000FF

/* ---- remaining constants ---- */

/* CreateFont arguments: the shim only reads the height, weight and pitch. */
#define FW_NORMAL 400
#define FW_BOLD   700
#define ANSI_CHARSET 0
#define DEFAULT_CHARSET 1
#define OUT_TT_PRECIS 4
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY 0
#define CLEARTYPE_QUALITY 5
#define ANTIALIASED_QUALITY 4
#define FIXED_PITCH 1
#define VARIABLE_PITCH 2
#define DEFAULT_PITCH 0
#define FF_DONTCARE 0

#define PS_SOLID 0
#define NULL_BRUSH 5
#define BLACKNESS 0x00000042L
#define WHITENESS 0x00FF0062L

#define CS_DBLCLKS 0x0008
#define HTCLIENT 1
#define HWND_TOP ((HWND)0)
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_SHOWWINDOW 0x0040
#define TPM_BOTTOMALIGN 0x0020
#define IDC_WAIT ((LPCSTR)32514)

#define EN_SETFOCUS  0x0100
#define EN_KILLFOCUS 0x0200
#define WM_SETFONT   0x0030

#define CREATE_NEW 1
#define FILE_ATTRIBUTE_HIDDEN 0x02
#define FILE_ATTRIBUTE_SYSTEM 0x04
#define FILE_NOTIFY_CHANGE_FILE_NAME 0x01
#define FILE_NOTIFY_CHANGE_DIR_NAME  0x02

#define SUCCEEDED(hr) ((hr) >= 0)

/* keys the editor binds that have no Mac equivalent name */
#define VK_OEM_PLUS  0xBB
#define VK_OEM_MINUS 0xBD
#define VK_OEM_1     0xBA
#define VK_OEM_2     0xBF
#define VK_OEM_3     0xC0
#define VK_OEM_4     0xDB
#define VK_OEM_5     0xDC
#define VK_OEM_6     0xDD
#define VK_OEM_7     0xDE
#define VK_OEM_8     0xDF
#define VK_ADD       0x6B
#define VK_SUBTRACT  0x6D
#define VK_DIVIDE    0x6F
#define VK_MULTIPLY  0x6A
#define VK_NUMPAD0   0x60

/* MSVC spellings the source uses; libSystem has the POSIX ones. */
int   _stricmp(const char *a, const char *b);
int   _strnicmp(const char *a, const char *b, size_t n);
char *_strdup(const char *s);
int   _vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

HANDLE  CreateFile(LPCSTR path, DWORD access, DWORD share, LPVOID sa,
                   DWORD create, DWORD flags, HANDLE tmpl);
BOOL    ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *read, LPVOID ov);
BOOL    WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, LPVOID ov);
DWORD   SetFilePointer(HANDLE h, LONG dist, LONG *high, DWORD method);
DWORD   GetFileSize(HANDLE h, DWORD *high);
BOOL    CloseHandle(HANDLE h);
BOOL    GetFileAttributesEx(LPCSTR path, int level, LPVOID info);

HANDLE  GlobalAlloc(UINT flags, DWORD n);
LPVOID  GlobalLock(HANDLE h);
BOOL    GlobalUnlock(HANDLE h);
BOOL    GlobalFree(HANDLE h);

LPSTR   GetCommandLine(void);
DWORD   GetEnvironmentVariable(LPCSTR name, LPSTR buf, DWORD n);
BOOL    SetEnvironmentVariable(LPCSTR name, LPCSTR val);
BOOL    SetCurrentDirectory(LPCSTR path);
DWORD   GetTempPath(DWORD n, LPSTR buf);
DWORD   SearchPath(LPCSTR dir, LPCSTR name, LPCSTR ext, DWORD n, LPSTR buf, LPSTR *part);
void    GetLocalTime(LPVOID st);
void    Sleep(DWORD ms);
LPSTR   CharUpper(LPSTR s);

int     CoInitialize(LPVOID p);
void    CoTaskMemFree(LPVOID p);
BOOL    InitCommonControlsEx(LPVOID p);
BOOL    FreeConsole(void);
BOOL    SetConsoleCtrlHandler(LPVOID h, BOOL add);
LPVOID  GetProcAddress(HANDLE m, LPCSTR n);
HANDLE  GetModuleHandle(LPCSTR n);
BOOL    SetHandleInformation(HANDLE h, DWORD mask, DWORD flags);

HCURSOR LoadCursor(HINSTANCE i, LPCSTR name);
HICON   LoadIcon(HINSTANCE i, LPCSTR name);
HANDLE  LoadImage(HINSTANCE i, LPCSTR n, UINT type, int cx, int cy, UINT flags);
HGDIOBJ GetStockObject(int obj);
HCURSOR SetCursor(HCURSOR c);   /* returns the previous cursor, as Win32 does */
int     GetSystemMetrics(int index);
BOOL    GetWindowPlacement(HWND w, LPVOID pl);
BOOL    SetWindowPos(HWND w, HWND after, int x, int y, int cw, int ch, UINT flags);

BOOL    ShellExecute(HWND w, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, int show);
int     SHFileOperation(LPVOID op);
DWORD   SHGetFolderPath(HWND w, int folder, HANDLE tok, DWORD flags, LPSTR path);
BOOL    SHGetPathFromIDList(LPVOID idl, LPSTR path);
LPVOID  SHBrowseForFolder(LPVOID bi);
BOOL    GetOpenFileName(LPVOID ofn);
BOOL    GetSaveFileName(LPVOID ofn);
int     MessageBoxIndirect(LPVOID params);
UINT    DragQueryFile(HDROP d, UINT i, LPSTR buf, UINT n);
void    DragFinish(HDROP d);

int     GetDIBits(HDC dc, HBITMAP b, UINT start, UINT lines, LPVOID bits, LPVOID info, UINT usage);
int     SetDIBitsToDevice(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                          UINT start, UINT lines, LPVOID bits, LPVOID info, UINT usage);
BOOL    PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop);

HANDLE  FindFirstChangeNotification(LPCSTR path, BOOL subtree, DWORD filter);
BOOL    FindNextChangeNotification(HANDLE h);
BOOL    FindCloseChangeNotification(HANDLE h);

DWORD   WaitForSingleObject(HANDLE h, DWORD ms);

#endif /* MAC_COCOA_WIN32_H */
