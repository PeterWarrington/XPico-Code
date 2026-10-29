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

typedef struct { int used, wrote; FILE *fp; char path[MAX_PATH * 2]; } FileHandle;
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
            s_files[i].wrote = (access & 0x40000000UL) ? 1 : 0;
            strncpy(s_files[i].path, path, sizeof s_files[i].path - 1);
            s_files[i].path[sizeof s_files[i].path - 1] = 0;
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
    if (f && f->used) {
        fclose(f->fp);
        /* give anything we just wrote a Finder type, or it is unopenable */
        if (f->wrote && f->path[0]) mac_set_file_type(f->path, 'TEXT', 'ttxt');
        f->used = 0;
    }
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

/* ------------------------------------------------- MSVC string spellings */

int _stricmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int _strnicmp(const char *a, const char *b, size_t n)
{
    for (; n && *a && *b; a++, b++, n--) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
    }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

int _vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return vsnprintf(buf, n, fmt, ap);
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
HGDIOBJ gdi_stock_object(int obj);

/* NULL_BRUSH means "outline only"; the icon code selects it before stroking. */
HGDIOBJ GetStockObject(int obj) { return gdi_stock_object(obj); }

static HCURSOR s_cursor;

HCURSOR SetCursor(HCURSOR c)
{
    HCURSOR prev = s_cursor;
    s_cursor = c;
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
    else if (which == 32514) SetThemeCursor(kThemeWatchCursor);           /* IDC_WAIT */
    else SetThemeCursor(kThemeArrowCursor);
}
    return prev;
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

/* main.c saves the window's size and position from this on exit. */
BOOL GetWindowPlacement(HWND w, LPVOID pl)
{
    WINDOWPLACEMENT *p = (WINDOWPLACEMENT *)pl;
    RECT r;
    if (!p) return FALSE;
    memset(p, 0, sizeof *p);
    p->length = sizeof *p;
    /* Classic windows zoom rather than maximise, so this is always "normal"
     * and the zoomed size is simply what gets saved. */
    p->showCmd = SW_SHOWNORMAL;
    GetWindowRect(w, &r);
    p->rcNormalPosition = r;
    return TRUE;
}

BOOL SetWindowPos(HWND w, HWND after, int x, int y, int cw, int ch, UINT flags)
{
    RECT cur;
    (void)after;
    if (!w) return FALSE;

    shim_child_rect(w, &cur);
    if (flags & SWP_NOMOVE) { x = cur.left; y = cur.top; }
    if (flags & SWP_NOSIZE) { cw = cur.right - cur.left; ch = cur.bottom - cur.top; }
    MoveWindow(w, x, y, cw, ch, TRUE);

    /* The command palette is created hidden and shown through this flag;
     * ignoring it meant the palette was positioned but never made visible. */
    if (flags & SWP_SHOWWINDOW) ShowWindow(w, SW_SHOW);
    return TRUE;
}

/* ------------------------------------------------------------ shell */

/* The Explorer "Reveal in Finder" and the website/donate links. */
BOOL ShellExecute(HWND w, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, int show)
{
    (void)w; (void)verb; (void)params; (void)dir; (void)show;
    if (!file) return FALSE;
    /* Internet Config opens a URL in the user's browser. LSOpenCFURLRef would
     * be the modern call, but it is not exported by this CarbonLib. */
    if (strncmp(file, "http", 4) == 0) {
        ICInstance inst;
        long len = (long)strlen(file);
        long selStart = 0, selEnd = len;
        OSStatus err;
        if (ICStart(&inst, 'XPCD') != noErr) return FALSE;
        err = ICLaunchURL(inst, (ConstStr255Param)"\p", (Ptr)file, len, &selStart, &selEnd);
        ICStop(inst);
        return err == noErr ? TRUE : FALSE;
    }
    return FALSE;
}

/* The Explorer's delete command. pFrom is a double-NUL terminated list of
 * paths; the editor only ever deletes one at a time. */
int SHFileOperation(LPVOID p)
{
    SHFILEOPSTRUCT *op = (SHFILEOPSTRUCT *)p;
    if (!op || op->wFunc != FO_DELETE || !op->pFrom) return -1;
    return mac_delete_path(op->pFrom) ? 0 : -1;
}

DWORD SHGetFolderPath(HWND w, int folder, HANDLE tok, DWORD flags, LPSTR path)
{
    (void)w; (void)folder; (void)tok; (void)flags;
    /* The settings file lives next to the application on OS 9. */
    if (path) path[0] = 0;
    return 0;
}



/* The About box goes through this; a stub meant the menu item did nothing. */
int MessageBoxIndirect(LPVOID params)
{
    MSGBOXPARAMS *mb = (MSGBOXPARAMS *)params;
    if (!mb) return IDCANCEL;
    return MessageBox(mb->hwndOwner, mb->lpszText, mb->lpszCaption, mb->dwStyle);
}

/* An HDROP here is the path list mac-carbon/sys.c built from an 'odoc' Apple Event:
 * count first, then that many NUL-terminated paths. */
UINT DragQueryFile(HDROP d, UINT i, LPSTR buf, UINT n)
{
    const char *p = (const char *)d;
    UINT count, k;
    if (!p) return 0;
    count = (UINT)(unsigned char)p[0];
    p++;
    if (i == 0xFFFFFFFFUL) return count;      /* Win32 asks for the count this way */
    if (i >= count) return 0;
    for (k = 0; k < i; k++) p += strlen(p) + 1;
    if (buf && n) {
        strncpy(buf, p, n - 1);
        buf[n - 1] = 0;
    }
    return (UINT)strlen(p);
}

void DragFinish(HDROP d) { mac_drop_release(d); }

/* ------------------------------------------- directory change notification */

/* Mac OS 9 has no change notification, so a watch records the folder's
 * modification date and WaitForSingleObject reports whether it has moved.
 * The File Manager touches a folder's date on any create, rename or delete
 * inside it, which is what the explorer wants to know. */
HANDLE FindFirstChangeNotification(LPCSTR path, BOOL subtree, DWORD filter)
{
    void *w;
    (void)subtree; (void)filter;
    w = mac_watch_open(path);
    return w ? (HANDLE)w : INVALID_HANDLE_VALUE;
}

BOOL FindNextChangeNotification(HANDLE h) { (void)h; return TRUE; }

BOOL FindCloseChangeNotification(HANDLE h)
{
    if (h != INVALID_HANDLE_VALUE) mac_watch_close(h);
    return TRUE;
}

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
/* The explorer polls its directory watch through this. */
DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    (void)ms;
    if (h == INVALID_HANDLE_VALUE || !h) return 0xFFFFFFFFUL;
    return mac_watch_changed(h) ? WAIT_OBJECT_0 : 0x00000102UL /* WAIT_TIMEOUT */;
}
BOOL  GetExitCodeProcess(HANDLE h, DWORD *code)   { (void)h; if (code) *code = 0; return FALSE; }
BOOL  GenerateConsoleCtrlEvent(DWORD ev, DWORD g) { (void)ev; (void)g; return FALSE; }
