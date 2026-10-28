/* Win32 compatibility shim for Mac OS 9 / Carbon.
 *
 * XPico Code is written against Win32. Rather than rewrite the workbench, this
 * header declares the subset of Win32 that the editor actually uses (~150 calls)
 * and the mac/*.c files implement it on top of QuickDraw and the Carbon Event
 * Manager. src/xpcode.h includes this instead of <windows.h> when MAC_OS is set.
 *
 * What is deliberately missing: processes, pipes and threads. Mac OS 9 has no
 * shell to drive, so terminal.c is compiled out (see HAVE_TERMINAL in xpcode.h)
 * and the Problems panel is fed only by its API, not by a child process. */
#ifndef MAC_WIN32_H
#define MAC_WIN32_H

/* Carbon.h is deliberately NOT included here. The app code includes only this
 * header, so it never sees the Toolbox namespace; the mac/*.c files include
 * Carbon.h themselves, before this header.
 *
 * Seven Win32 names also exist in Carbon with different signatures. They are
 * renamed here so that app code compiles to distinct symbols and CarbonLib's
 * own exports stay reachable inside the shim. Carbon's versions must not be
 * called from a file that includes this header: use the Toolbox equivalent
 * (ShowHide, SetWindowBounds, AppendMenuItemTextWithCFString, qd_line_to, ...). */
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
#define MAX_PATH 256            /* HFS paths are shorter than NTFS ones */

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

/* ---- window management (mac/wnd.c) ---- */
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

/* ---- drawing (mac/gdi.c) ---- */
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

/* caret: QuickDraw has none, so mac/gdi.c blinks one itself */
BOOL    CreateCaret(HWND w, HBITMAP b, int cw, int ch);
BOOL    ShowCaret(HWND w);
BOOL    DestroyCaret(void);
BOOL    SetCaretPos(int x, int y);

/* ---- rects ---- */
/* Functions, not macros: Carbon's SetRect takes the same number of arguments,
 * so a macro here would silently rewrite Toolbox calls inside the shim. */
void    SetRect(LPRECT r, int l, int t, int rt, int b);
void    SetRectEmpty(LPRECT r);
BOOL    IsRectEmpty(const RECT *r);
BOOL    PtInRect(const RECT *r, POINT p);

/* ---- system / files (mac/sys.c) ---- */
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

/* directory enumeration, mapped onto the File Manager */
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

/* clipboard, over the Carbon scrap */
BOOL    OpenClipboard(HWND w);
BOOL    CloseClipboard(void);
BOOL    EmptyClipboard(void);
HANDLE  GetClipboardData(UINT fmt);
HANDLE  SetClipboardData(UINT fmt, HANDLE mem);
#define CF_TEXT 1

/* menus, over the Carbon Menu Manager */
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

/* file dialogs, over Navigation Services */
BOOL    MacChooseFile(char *path, int n, BOOL save);
BOOL    MacChooseFolder(char *path, int n);

/* ---- the rest of the surface (mac/compat.c) ---- */
typedef long LONG_PTR;

/* cursors and icons: the IDC_/IDI_ values are the Win32 ones, so that
 * mac/compat.c can tell which cursor was asked for. */
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

LONG_PTR SetWindowLongPtr(HWND w, int index, LONG_PTR v);
LRESULT  CallWindowProc(WNDPROC proc, HWND w, UINT m, WPARAM wp, LPARAM lp);

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
void    SetCursor(HCURSOR c);
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

BOOL    CreatePipe(HANDLE *r, HANDLE *w, LPVOID sa, DWORD size);
BOOL    CreateProcess(LPCSTR app, LPSTR cmd, LPVOID pa, LPVOID ta, BOOL inherit,
                      DWORD flags, LPVOID env, LPCSTR cwd, LPVOID si, LPVOID pi);
HANDLE  CreateThread(LPVOID sa, DWORD stack, LPVOID fn, LPVOID arg, DWORD flags, DWORD *id);
BOOL    TerminateProcess(HANDLE h, UINT code);
DWORD   WaitForSingleObject(HANDLE h, DWORD ms);
BOOL    GetExitCodeProcess(HANDLE h, DWORD *code);
BOOL    GenerateConsoleCtrlEvent(DWORD ev, DWORD group);

#endif /* MAC_WIN32_H */
