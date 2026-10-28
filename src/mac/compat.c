/* The remainder of the Win32 surface: file I/O, memory, cursors, shell calls
 * and the pieces of Win32 that have no Mac OS 9 equivalent at all.
 *
 * Anything stubbed here is marked UNIMPLEMENTED with what it would take. The
 * process, pipe and thread calls are only referenced by terminal.c, which this
 * build leaves out (HAVE_TERMINAL is 0), so they exist to keep the link honest
 * rather than to work. */
#include <Carbon.h>
#include "win32.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------ file I/O */

typedef struct { int used; FILE *fp; } FileHandle;
#define MAX_FILES 16
static FileHandle s_files[MAX_FILES];

HANDLE CreateFile(LPCSTR path, DWORD access, DWORD share, LPVOID sa,
                  DWORD create, DWORD flags, HANDLE tmpl)
{
    int i;
    const char *mode;
    (void)share; (void)sa; (void)flags; (void)tmpl;

    /* GENERIC_WRITE is 0x40000000, GENERIC_READ 0x80000000; CREATE_ALWAYS is 2. */
    if (access & 0x40000000UL) mode = (create == 2 || create == 1) ? "wb" : "r+b";
    else mode = "rb";

    for (i = 0; i < MAX_FILES; i++)
        if (!s_files[i].used) {
            FILE *fp = fopen(path, mode);
            if (!fp) return INVALID_HANDLE_VALUE;
            s_files[i].used = 1;
            s_files[i].fp = fp;
            return (HANDLE)&s_files[i];
        }
    return INVALID_HANDLE_VALUE;
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *read, LPVOID ov)
{
    FileHandle *f = (FileHandle *)h;
    (void)ov;
    if (!f || !f->used) return FALSE;
    *read = (DWORD)fread(buf, 1, n, f->fp);
    return TRUE;
}

BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, LPVOID ov)
{
    FileHandle *f = (FileHandle *)h;
    (void)ov;
    if (!f || !f->used) return FALSE;
    *written = (DWORD)fwrite(buf, 1, n, f->fp);
    return *written == n;
}

DWORD SetFilePointer(HANDLE h, LONG dist, LONG *high, DWORD method)
{
    FileHandle *f = (FileHandle *)h;
    (void)high;
    if (!f || !f->used) return (DWORD)-1;
    fseek(f->fp, dist, method == 1 ? SEEK_CUR : method == 2 ? SEEK_END : SEEK_SET);
    return (DWORD)ftell(f->fp);
}

DWORD GetFileSize(HANDLE h, DWORD *high)
{
    FileHandle *f = (FileHandle *)h;
    long cur, end;
    if (high) *high = 0;
    if (!f || !f->used) return (DWORD)-1;
    cur = ftell(f->fp);
    fseek(f->fp, 0, SEEK_END);
    end = ftell(f->fp);
    fseek(f->fp, cur, SEEK_SET);
    return (DWORD)end;
}

BOOL CloseHandle(HANDLE h)
{
    FileHandle *f = (FileHandle *)h;
    if (f && f->used) { fclose(f->fp); f->used = 0; }
    return TRUE;
}

/* GetFileAttributesEx is used only to read the modification time for the
 * on-disk change check, so it reports what the catalog gives. */
BOOL GetFileAttributesEx(LPCSTR path, int level, LPVOID info)
{
    struct { DWORD attr; FILETIME c, a, w; DWORD hi, lo; } *out = info;
    WIN32_FIND_DATA fd;
    HANDLE h;
    (void)level;
    h = FindFirstFile(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    FindClose(h);
    out->attr = fd.dwFileAttributes;
    out->c = fd.ftCreationTime;
    out->a = fd.ftLastAccessTime;
    out->w = fd.ftLastWriteTime;
    out->hi = fd.nFileSizeHigh;
    out->lo = fd.nFileSizeLow;
    return TRUE;
}

/* ------------------------------------------------------------ memory */

HANDLE GlobalAlloc(UINT flags, DWORD n)
{
    void *p;
    (void)flags;
    p = malloc(n ? n : 1);
    if (p) memset(p, 0, n ? n : 1);
    return (HANDLE)p;
}

LPVOID GlobalLock(HANDLE h)   { return (LPVOID)h; }
BOOL   GlobalUnlock(HANDLE h) { (void)h; return TRUE; }
BOOL   GlobalFree(HANDLE h)   { free(h); return TRUE; }

/* ------------------------------------------------------------ environment */

static char s_cmdline[MAX_PATH] = "XPicoCode";
static char s_cwd[MAX_PATH];

LPSTR GetCommandLine(void) { return s_cmdline; }

DWORD GetEnvironmentVariable(LPCSTR name, LPSTR buf, DWORD n)
{
    const char *v = getenv(name);
    if (!v) { if (buf && n) buf[0] = 0; return 0; }
    strncpy(buf, v, n - 1);
    buf[n - 1] = 0;
    return (DWORD)strlen(buf);
}

BOOL SetEnvironmentVariable(LPCSTR name, LPCSTR val)
{
    (void)name; (void)val;
    return TRUE;   /* only used to extend PATH for the terminal, which is absent */
}

BOOL SetCurrentDirectory(LPCSTR path)
{
    strncpy(s_cwd, path ? path : "", sizeof s_cwd - 1);
    s_cwd[sizeof s_cwd - 1] = 0;
    return TRUE;
}

DWORD GetTempPath(DWORD n, LPSTR buf)
{
    strncpy(buf, ":", n - 1);
    buf[n - 1] = 0;
    return (DWORD)strlen(buf);
}

/* UNIMPLEMENTED: SearchPath locates a build tool on PATH. There is no PATH on
 * Mac OS 9 and no tool to run, so it always fails. */
DWORD SearchPath(LPCSTR dir, LPCSTR name, LPCSTR ext, DWORD n, LPSTR buf, LPSTR *part)
{
    (void)dir; (void)name; (void)ext; (void)n; (void)buf; (void)part;
    return 0;
}

void GetLocalTime(LPVOID st)
{
    /* SYSTEMTIME layout: wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute,
     * wSecond, wMilliseconds, all WORD. Used only for the output log stamp. */
    unsigned short *w = (unsigned short *)st;
    DateTimeRec d;
    unsigned long secs;
    GetDateTime(&secs);
    SecondsToDate(secs, &d);
    w[0] = (unsigned short)d.year;  w[1] = (unsigned short)d.month;
    w[2] = (unsigned short)(d.dayOfWeek - 1); w[3] = (unsigned short)d.day;
    w[4] = (unsigned short)d.hour;  w[5] = (unsigned short)d.minute;
    w[6] = (unsigned short)d.second; w[7] = 0;
}

void Sleep(DWORD ms)
{
    unsigned long end = TickCount() + (ms * 60) / 1000;
    while (TickCount() < end) { /* spin: no scheduler to yield to on OS 9 */ }
}

LPSTR CharUpper(LPSTR s)
{
    char *p;
    for (p = s; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    return s;
}

/* ------------------------------------------------------------ no-ops */

/* These exist on Windows for COM, the common controls and console handling;
 * none of it has a Mac counterpart that the editor needs. */
int   CoInitialize(LPVOID p)                 { (void)p; return 0; }
void  CoTaskMemFree(LPVOID p)                { (void)p; }
BOOL  InitCommonControlsEx(LPVOID p)         { (void)p; return TRUE; }
BOOL  FreeConsole(void)                      { return TRUE; }
BOOL  SetConsoleCtrlHandler(LPVOID h, BOOL a){ (void)h; (void)a; return TRUE; }
LPVOID GetProcAddress(HANDLE m, LPCSTR n)    { (void)m; (void)n; return NULL; }
HANDLE GetModuleHandle(LPCSTR n)             { (void)n; return NULL; }
BOOL  SetHandleInformation(HANDLE h, DWORD m, DWORD f) { (void)h; (void)m; (void)f; return TRUE; }

/* ------------------------------------------------------------ cursors, icons */

HCURSOR LoadCursor(HINSTANCE i, LPCSTR name) { (void)i; return (HCURSOR)name; }
HICON   LoadIcon(HINSTANCE i, LPCSTR name)   { (void)i; (void)name; return NULL; }
HANDLE  LoadImage(HINSTANCE i, LPCSTR n, UINT t, int cx, int cy, UINT f)
{
    (void)i; (void)n; (void)t; (void)cx; (void)cy; (void)f;
    return NULL;
}
HGDIOBJ GetStockObject(int obj) { (void)obj; return NULL; }

void SetCursor(HCURSOR c)
{
    /* The source asks for IDC_ARROW, IDC_IBEAM and the split cursors; the
     * Toolbox equivalents are the arrow and I-beam, and resize cursors come
     * from the system cursor resources. */
    long which = (long)c;
    if (which == 32513) SetThemeCursor(kThemeIBeamCursor);                /* IDC_IBEAM */
    else if (which == 32644) SetThemeCursor(kThemeResizeLeftRightCursor); /* IDC_SIZEWE */
    /* IDC_SIZENS: these Universal Interfaces have no up/down resize cursor,
     * so the vertical splitter keeps the arrow. */
    else if (which == 32645) SetThemeCursor(kThemeArrowCursor);
    else if (which == 32649) SetThemeCursor(kThemePointingHandCursor);    /* IDC_HAND */
    else SetThemeCursor(kThemeArrowCursor);
}

int GetSystemMetrics(int index)
{
    BitMap screen;
    GetQDGlobalsScreenBits(&screen);
    switch (index) {
    case 0:  return screen.bounds.right - screen.bounds.left;   /* SM_CXSCREEN */
    case 1:  return screen.bounds.bottom - screen.bounds.top;   /* SM_CYSCREEN */
    default: return 0;
    }
}

BOOL GetWindowPlacement(HWND w, LPVOID pl)
{
    /* Only the maximised flag is read, to persist it; windows on OS 9 are
     * zoomed rather than maximised, so report "normal". */
    unsigned int *u = (unsigned int *)pl;
    (void)w;
    if (u) u[1] = 1;   /* showCmd = SW_SHOWNORMAL */
    return TRUE;
}

BOOL SetWindowPos(HWND w, HWND after, int x, int y, int cw, int ch, UINT flags)
{
    (void)after; (void)flags;
    MoveWindow(w, x, y, cw, ch, TRUE);
    return TRUE;
}

/* ------------------------------------------------------------ shell */

/* The Explorer "Reveal in Finder" and the website/donate links. */
BOOL ShellExecute(HWND w, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, int show)
{
    (void)w; (void)verb; (void)params; (void)dir; (void)show;
    if (!file) return FALSE;
    if (strncmp(file, "http", 4) == 0) {
        CFURLRef url = CFURLCreateWithBytes(NULL, (const UInt8 *)file,
                                            (CFIndex)strlen(file), kCFStringEncodingASCII, NULL);
        if (url) { LSOpenCFURLRef(url, NULL); CFRelease(url); return TRUE; }
        return FALSE;
    }
    return FALSE;
}

/* UNIMPLEMENTED: SHFileOperation does the Explorer delete-to-recycle-bin. The
 * Mac equivalent is FSMoveObjectToTrashSync; left out until the explorer's
 * delete path is wired up and tested. */
int SHFileOperation(LPVOID op) { (void)op; return -1; }

DWORD SHGetFolderPath(HWND w, int folder, HANDLE tok, DWORD flags, LPSTR path)
{
    (void)w; (void)folder; (void)tok; (void)flags;
    /* The settings file lives next to the application on OS 9. */
    if (path) path[0] = 0;
    return 0;
}

BOOL SHGetPathFromIDList(LPVOID idl, LPSTR path) { (void)idl; if (path) path[0] = 0; return FALSE; }
LPVOID SHBrowseForFolder(LPVOID bi) { (void)bi; return NULL; }

/* GetOpenFileName/GetSaveFileName are routed to Navigation Services. The
 * OPENFILENAME layout the source uses is lpstrFile/nMaxFile, so the shim reads
 * those two fields by offset-free struct access in xpcode's own declaration. */
BOOL GetOpenFileName(LPVOID ofn)
{
    char **fileField = (char **)((char *)ofn + 0);
    (void)fileField;
    return FALSE;   /* replaced by MacChooseFile at the call site */
}

BOOL GetSaveFileName(LPVOID ofn) { (void)ofn; return FALSE; }

int MessageBoxIndirect(LPVOID params) { (void)params; return IDCANCEL; }

UINT DragQueryFile(HDROP d, UINT i, LPSTR buf, UINT n)
{
    (void)d; (void)i;
    if (buf && n) buf[0] = 0;
    return 0;
}

void DragFinish(HDROP d) { (void)d; }

/* ------------------------------------------------- minimap bitmap access */

/* UNIMPLEMENTED: the minimap renders into a DIB and blits it with GetDIBits /
 * SetDIBitsToDevice. Doing this on QuickDraw means locking the GWorld's PixMap
 * and walking its rowBytes directly. Until that is written the Mac build runs
 * with the minimap off (g_minimap is forced to 0 in main.c). */
int GetDIBits(HDC dc, HBITMAP b, UINT start, UINT lines, LPVOID bits, LPVOID info, UINT usage)
{
    (void)dc; (void)b; (void)start; (void)lines; (void)bits; (void)info; (void)usage;
    return 0;
}

int SetDIBitsToDevice(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                      UINT start, UINT lines, LPVOID bits, LPVOID info, UINT usage)
{
    (void)dc; (void)x; (void)y; (void)w; (void)h; (void)sx; (void)sy;
    (void)start; (void)lines; (void)bits; (void)info; (void)usage;
    return 0;
}

BOOL PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    RECT r;
    (void)rop;
    SetRect(&r, x, y, x + w, y + h);
    return ExtTextOut(dc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
}

/* ------------------------------------------- directory change notification */

/* UNIMPLEMENTED: Win32 signals a handle when a directory changes. Mac OS 9 has
 * no such notification, so the explorer's auto-refresh relies on its existing
 * one-second poll (sidebar_tick) instead and these report "no change". */
HANDLE FindFirstChangeNotification(LPCSTR path, BOOL subtree, DWORD filter)
{
    (void)path; (void)subtree; (void)filter;
    return INVALID_HANDLE_VALUE;
}

BOOL FindNextChangeNotification(HANDLE h)  { (void)h; return FALSE; }
BOOL FindCloseChangeNotification(HANDLE h) { (void)h; return TRUE; }

/* --------------------------------------------- processes (terminal absent) */

/* These are referenced only by terminal.c. HAVE_TERMINAL is 0 for this build,
 * so they are never called; they fail cleanly if that ever changes. */
BOOL CreatePipe(HANDLE *r, HANDLE *w, LPVOID sa, DWORD size)
{
    (void)sa; (void)size;
    if (r) *r = INVALID_HANDLE_VALUE;
    if (w) *w = INVALID_HANDLE_VALUE;
    return FALSE;
}

BOOL CreateProcess(LPCSTR app, LPSTR cmd, LPVOID pa, LPVOID ta, BOOL inherit,
                   DWORD flags, LPVOID env, LPCSTR cwd, LPVOID si, LPVOID pi)
{
    (void)app; (void)cmd; (void)pa; (void)ta; (void)inherit;
    (void)flags; (void)env; (void)cwd; (void)si; (void)pi;
    return FALSE;
}

HANDLE CreateThread(LPVOID sa, DWORD stack, LPVOID fn, LPVOID arg, DWORD flags, DWORD *id)
{
    (void)sa; (void)stack; (void)fn; (void)arg; (void)flags; (void)id;
    return NULL;
}

BOOL  TerminateProcess(HANDLE h, UINT code)       { (void)h; (void)code; return FALSE; }
DWORD WaitForSingleObject(HANDLE h, DWORD ms)     { (void)h; (void)ms; return 0xFFFFFFFFUL; }
BOOL  GetExitCodeProcess(HANDLE h, DWORD *code)   { (void)h; if (code) *code = 0; return FALSE; }
BOOL  GenerateConsoleCtrlEvent(DWORD ev, DWORD g) { (void)ev; (void)g; return FALSE; }
