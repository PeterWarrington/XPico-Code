/* The remainder of the Win32 surface: file I/O, memory, cursors, the shell
 * and the system metrics.
 *
 * Nothing here is stubbed out for want of a system call -- unlike the Carbon
 * port, this build is running on Unix, so the process, path and environment
 * calls all mean what they say. What is missing from win32.h instead is the
 * pipe and thread family: those are referenced only by terminal.c, which this
 * build replaces with panel_cocoa.c and term_pty.c. */
#import <Cocoa/Cocoa.h>
#include "win32.h"
#include <string.h>
#include <strings.h>      /* strcasecmp: not pulled in by <string.h> on 10.4 */
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

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

    if (access & GENERIC_WRITE)
        mode = (create == CREATE_ALWAYS || create == CREATE_NEW) ? "wb" : "r+b";
    else
        mode = "rb";

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
    fseek(f->fp, dist, method == FILE_CURRENT ? SEEK_CUR :
                       method == FILE_END ? SEEK_END : SEEK_SET);
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
        f->used = 0;
    }
    return TRUE;
}

/* Used only to read the modification time for the on-disk change check. */
BOOL GetFileAttributesEx(LPCSTR path, int level, LPVOID info)
{
    WIN32_FILE_ATTRIBUTE_DATA *out = (WIN32_FILE_ATTRIBUTE_DATA *)info;
    struct stat st;
    (void)level;
    if (!path || !out || stat(path, &st) != 0) return FALSE;
    memset(out, 0, sizeof *out);
    out->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    /* the same raw-seconds scale FindFirstFile reports, which is all
     * CompareFileTime needs */
    out->ftLastWriteTime.dwLowDateTime = (DWORD)st.st_mtime;
    out->ftCreationTime = out->ftLastWriteTime;
    out->ftLastAccessTime = out->ftLastWriteTime;
    out->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}

/* ------------------------------------------------------------ memory */

HANDLE GlobalAlloc(UINT flags, DWORD n)
{
    void *p;
    (void)flags;
    p = calloc(1, n ? n : 1);
    return (HANDLE)p;
}

LPVOID GlobalLock(HANDLE h)   { return (LPVOID)h; }
BOOL   GlobalUnlock(HANDLE h) { (void)h; return TRUE; }
BOOL   GlobalFree(HANDLE h)   { free(h); return TRUE; }

/* ------------------------------------------------------------ environment */

/* A bundled application is launched with no arguments -- a file double-clicked
 * in the Finder arrives as an 'odoc' event instead, which the delegate turns
 * into WM_DROPFILES -- so this is the program name and nothing else, which is
 * exactly what main.c's parser expects to skip over. */
static char s_cmdline[MAX_PATH] = "XPicoCode";

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
    if (!name) return FALSE;
    return setenv(name, val ? val : "", 1) == 0 ? TRUE : FALSE;
}

BOOL SetCurrentDirectory(LPCSTR path)
{
    return (path && chdir(path) == 0) ? TRUE : FALSE;
}

DWORD GetTempPath(DWORD n, LPSTR buf)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp/";
    strncpy(buf, tmp, n - 1);
    buf[n - 1] = 0;
    return (DWORD)strlen(buf);
}

/* Locate a tool, which the Run commands use to decide what they can offer.
 * Unlike the Carbon build there is a real PATH here to search. */
DWORD SearchPath(LPCSTR dir, LPCSTR name, LPCSTR ext, DWORD n, LPSTR buf, LPSTR *part)
{
    char cand[MAX_PATH * 2];
    const char *path, *p;
    (void)ext;
    if (!name) return 0;

    if (dir && *dir) {
        snprintf(cand, sizeof cand, "%s/%s", dir, name);
        if (access(cand, X_OK) == 0) goto found;
    }
    path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin:/usr/local/bin";
    for (p = path; *p; ) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len && len < sizeof cand - 2) {
            memcpy(cand, p, len);
            cand[len] = 0;
            snprintf(cand + len, sizeof cand - len, "/%s", name);
            if (access(cand, X_OK) == 0) goto found;
        }
        if (!colon) break;
        p = colon + 1;
    }
    return 0;

found:
    strncpy(buf, cand, n - 1);
    buf[n - 1] = 0;
    if (part) {
        char *slash = strrchr(buf, '/');
        *part = slash ? slash + 1 : buf;
    }
    return (DWORD)strlen(buf);
}

void GetLocalTime(LPVOID st)
{
    SYSTEMTIME *s = (SYSTEMTIME *)st;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!s || !t) return;
    s->wYear = (WORD)(t->tm_year + 1900);
    s->wMonth = (WORD)(t->tm_mon + 1);
    s->wDayOfWeek = (WORD)t->tm_wday;
    s->wDay = (WORD)t->tm_mday;
    s->wHour = (WORD)t->tm_hour;
    s->wMinute = (WORD)t->tm_min;
    s->wSecond = (WORD)t->tm_sec;
    s->wMilliseconds = 0;
}

void Sleep(DWORD ms) { usleep((unsigned int)ms * 1000); }

/* ------------------------------------------------- MSVC string spellings */

int _stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
int _strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }

char *_strdup(const char *s) { return strdup(s); }

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

/* LoadCursor hands back the IDC_ value itself, so SetCursor can tell which
 * cursor was asked for. */
HCURSOR LoadCursor(HINSTANCE i, LPCSTR name) { (void)i; return (HCURSOR)name; }

/* The application's icon comes from the bundle, not from a runtime call. */
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

/* Applied every time, not only when it changes: AppKit resets the cursor on
 * its own schedule -- crossing the window frame, a tracking area, a cursor
 * rect -- so a shim that skipped the redundant calls would find the cursor
 * quietly reverting to the arrow and never putting its own back. */
HCURSOR SetCursor(HCURSOR c)
{
    HCURSOR prev = s_cursor;
    long which = (long)c;
    s_cursor = c;
    switch (which) {
    case 32513: [[NSCursor IBeamCursor] set]; break;              /* IDC_IBEAM */
    case 32644: [[NSCursor resizeLeftRightCursor] set]; break;    /* IDC_SIZEWE */
    case 32645: [[NSCursor resizeUpDownCursor] set]; break;       /* IDC_SIZENS */
    case 32649: [[NSCursor pointingHandCursor] set]; break;       /* IDC_HAND */
    default:    [[NSCursor arrowCursor] set]; break;
    }
    return prev;
}

int GetSystemMetrics(int index)
{
    NSRect frame = [[NSScreen mainScreen] frame];
    switch (index) {
    case SM_CXSCREEN: return (int)frame.size.width;
    case SM_CYSCREEN: return (int)frame.size.height;
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
    /* A zoomed Mac window is still a normal window, so the zoomed size is
     * simply what gets saved. */
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
     * ignoring it would leave the palette positioned but invisible. */
    if (flags & SWP_SHOWWINDOW) ShowWindow(w, SW_SHOW);
    return TRUE;
}

/* ------------------------------------------------------------ shell */

/* The website and donate links, and anything else the app hands to the
 * system to open. */
BOOL ShellExecute(HWND w, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, int show)
{
    NSString *s;
    (void)w; (void)verb; (void)params; (void)dir; (void)show;
    if (!file) return FALSE;
    s = [NSString stringWithUTF8String:file];
    if (!s) return FALSE;
    if (strncmp(file, "http", 4) == 0)
        return [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:s]] ? TRUE : FALSE;
    return [[NSWorkspace sharedWorkspace] openFile:s] ? TRUE : FALSE;
}

/* The explorer's delete command. pFrom is a double-NUL terminated list of
 * paths; the editor only ever deletes one at a time.
 *
 * This deletes rather than moving to the Trash: FSMoveObjectToTrashSync and
 * NSWorkspaceRecycleOperation both postdate 10.4, so FOF_ALLOWUNDO cannot be
 * honoured. The explorer asks for confirmation first. */
int SHFileOperation(LPVOID p)
{
    SHFILEOPSTRUCT *op = (SHFILEOPSTRUCT *)p;
    if (!op || op->wFunc != FO_DELETE || !op->pFrom) return -1;
    return mac_delete_path(op->pFrom) ? 0 : -1;
}

/* Where the settings file lives. */
DWORD SHGetFolderPath(HWND w, int folder, HANDLE tok, DWORD flags, LPSTR path)
{
    const char *home = getenv("HOME");
    (void)w; (void)folder; (void)tok; (void)flags;
    if (!path) return (DWORD)-1;
    if (!home || !*home) { path[0] = 0; return (DWORD)-1; }
    snprintf(path, MAX_PATH, "%s/Library/Application Support", home);
    return 0;
}

/* The About box goes through this. */
int MessageBoxIndirect(LPVOID params)
{
    MSGBOXPARAMS *mb = (MSGBOXPARAMS *)params;
    if (!mb) return IDCANCEL;
    return MessageBox(mb->hwndOwner, mb->lpszText, mb->lpszCaption, mb->dwStyle);
}

/* An HDROP here is the path list mac-cocoa/sys.m packed from a drag or an
 * 'odoc': a count byte, then that many NUL-terminated paths. */
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

/* The explorer arms a watch on its root folder and polls it from the editor's
 * timer; mac-cocoa/sys.m keeps the folder's modification time. */
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

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    (void)ms;
    if (h == INVALID_HANDLE_VALUE || !h) return 0xFFFFFFFFUL;
    return mac_watch_changed(h) ? WAIT_OBJECT_0 : 0x00000102UL /* WAIT_TIMEOUT */;
}
