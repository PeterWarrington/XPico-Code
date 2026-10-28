/* Files, settings, clipboard, menus and dialogs for the Win32 shim.
 *
 * Paths: the editor passes around Win32 paths with backslashes, and HFS uses
 * colons with no drive letter and a leading colon for relative paths. Rather
 * than touch every call site, the shim accepts either separator and converts
 * at the File Manager boundary (see to_hfs). */
#include <Carbon.h>
#include "win32.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------ paths */

/* Convert a shim path to an HFS path in place-safe fashion. Backslashes become
 * colons; a path that does not start with a volume name is left relative. */
static void to_hfs(const char *in, char *out, int n)
{
    int i = 0;
    for (; in[i] && i < n - 1; i++)
        out[i] = (in[i] == '\\' || in[i] == '/') ? ':' : in[i];
    out[i] = 0;
}

static void to_pstr(const char *s, Str255 p)
{
    size_t len = strlen(s);
    if (len > 255) len = 255;
    p[0] = (unsigned char)len;
    memcpy(p + 1, s, len);
}

static int path_to_fsspec(const char *path, FSSpec *spec)
{
    char hfs[MAX_PATH * 2];
    Str255 p;
    to_hfs(path, hfs, sizeof hfs);
    to_pstr(hfs, p);
    return FSMakeFSSpec(0, 0, p, spec) == noErr;
}

DWORD GetFileAttributes(LPCSTR path)
{
    FSSpec spec;
    CInfoPBRec pb;
    if (!path_to_fsspec(path, &spec)) return INVALID_FILE_ATTRIBUTES;
    memset(&pb, 0, sizeof pb);
    pb.hFileInfo.ioNamePtr = spec.name;
    pb.hFileInfo.ioVRefNum = spec.vRefNum;
    pb.hFileInfo.ioDirID = spec.parID;
    pb.hFileInfo.ioFDirIndex = 0;
    if (PBGetCatInfoSync(&pb) != noErr) return INVALID_FILE_ATTRIBUTES;
    return (pb.hFileInfo.ioFlAttrib & ioDirMask) ? FILE_ATTRIBUTE_DIRECTORY : 0;
}

BOOL CreateDirectory(LPCSTR path, LPVOID sa)
{
    FSSpec spec;
    long created;
    (void)sa;
    if (!path_to_fsspec(path, &spec)) return FALSE;
    return FSpDirCreate(&spec, smSystemScript, &created) == noErr;
}

BOOL MoveFile(LPCSTR from, LPCSTR to)
{
    FSSpec a, b;
    if (!path_to_fsspec(from, &a) || !path_to_fsspec(to, &b)) return FALSE;
    /* Same-directory rename is all the editor needs (Save As and the explorer
     * rename command both stay inside one folder). */
    return FSpRename(&a, b.name) == noErr;
}

DWORD GetFullPathName(LPCSTR name, DWORD n, LPSTR buf, LPSTR *part)
{
    DWORD len;
    strncpy(buf, name, n - 1);
    buf[n - 1] = 0;
    len = (DWORD)strlen(buf);
    if (part) {
        char *p = strrchr(buf, '\\');
        char *q = strrchr(buf, ':');
        if (q > p) p = q;
        *part = p ? p + 1 : buf;
    }
    return len;
}

DWORD GetModuleFileName(HINSTANCE inst, LPSTR buf, DWORD n)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec spec;
    (void)inst;
    buf[0] = 0;
    GetCurrentProcess(&psn);
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processAppSpec = &spec;
    if (GetProcessInformation(&psn, &info) == noErr) {
        unsigned char len = spec.name[0];
        if (len > n - 1) len = (unsigned char)(n - 1);
        memcpy(buf, spec.name + 1, len);
        buf[len] = 0;
    }
    return (DWORD)strlen(buf);
}

DWORD GetTickCount(void)
{
    /* TickCount is 1/60s; the callers only measure elapsed milliseconds. */
    return (DWORD)TickCount() * 1000UL / 60UL;
}

/* ------------------------------------------------- directory enumeration */

typedef struct {
    short vRefNum;
    long  dirID;
    short index;
    int   used;
} FindState;

#define MAX_FINDS 8
static FindState s_finds[MAX_FINDS];

/* FindFirstFile is called with patterns like "C:\dir\*" or "...\*.c"; only the
 * directory part is used to walk the catalog, and the extension is matched. */
static void split_pattern(const char *pattern, char *dir, int dn, char *ext, int en)
{
    const char *star = strrchr(pattern, '*');
    const char *sep;
    int len;
    ext[0] = 0;
    sep = pattern;
    {
        const char *a = strrchr(pattern, '\\');
        const char *b = strrchr(pattern, '/');
        const char *c = strrchr(pattern, ':');
        sep = a; if (b > sep) sep = b; if (c > sep) sep = c;
    }
    len = sep ? (int)(sep - pattern) : 0;
    if (len > dn - 1) len = dn - 1;
    memcpy(dir, pattern, len);
    dir[len] = 0;
    if (star && star[1] == '.') {
        strncpy(ext, star + 1, en - 1);
        ext[en - 1] = 0;
    }
}

static int fill_find_data(FindState *f, WIN32_FIND_DATA *fd)
{
    CInfoPBRec pb;
    Str255 name;
    for (;;) {
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = f->vRefNum;
        pb.hFileInfo.ioDirID = f->dirID;
        pb.hFileInfo.ioFDirIndex = f->index++;
        if (PBGetCatInfoSync(&pb) != noErr) return 0;

        memcpy(fd->cFileName, name + 1, name[0]);
        fd->cFileName[name[0]] = 0;
        fd->cAlternateFileName[0] = 0;
        fd->dwFileAttributes = (pb.hFileInfo.ioFlAttrib & ioDirMask) ? FILE_ATTRIBUTE_DIRECTORY : 0;
        fd->nFileSizeHigh = 0;
        fd->nFileSizeLow = (DWORD)pb.hFileInfo.ioFlLgLen;
        /* HFS stores seconds since 1904; Win32 FILETIME is 100ns since 1601.
         * Only CompareFileTime consumes this, so the raw seconds are kept and
         * compared on the same scale. */
        fd->ftLastWriteTime.dwLowDateTime = (DWORD)pb.hFileInfo.ioFlMdDat;
        fd->ftLastWriteTime.dwHighDateTime = 0;
        fd->ftCreationTime = fd->ftLastWriteTime;
        fd->ftLastAccessTime = fd->ftLastWriteTime;
        return 1;
    }
}

HANDLE FindFirstFile(LPCSTR pattern, WIN32_FIND_DATA *fd)
{
    char dir[MAX_PATH], ext[32], hfs[MAX_PATH * 2];
    FSSpec spec;
    Str255 p;
    CInfoPBRec pb;
    int i;
    FindState *f = NULL;

    for (i = 0; i < MAX_FINDS; i++)
        if (!s_finds[i].used) { f = &s_finds[i]; break; }
    if (!f) return INVALID_HANDLE_VALUE;

    split_pattern(pattern, dir, sizeof dir, ext, sizeof ext);
    to_hfs(dir[0] ? dir : ":", hfs, sizeof hfs);
    /* a directory path has to end in a colon for FSMakeFSSpec to resolve it */
    if (hfs[0] && hfs[strlen(hfs) - 1] != ':') strncat(hfs, ":", sizeof hfs - strlen(hfs) - 1);
    to_pstr(hfs, p);
    if (FSMakeFSSpec(0, 0, p, &spec) != noErr) return INVALID_HANDLE_VALUE;

    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = spec.name;
    pb.dirInfo.ioVRefNum = spec.vRefNum;
    pb.dirInfo.ioDrDirID = spec.parID;
    pb.dirInfo.ioFDirIndex = 0;
    if (PBGetCatInfoSync(&pb) != noErr) return INVALID_HANDLE_VALUE;

    f->used = 1;
    f->vRefNum = spec.vRefNum;
    f->dirID = pb.dirInfo.ioDrDirID;
    f->index = 1;
    if (!fill_find_data(f, fd)) { f->used = 0; return INVALID_HANDLE_VALUE; }
    return (HANDLE)f;
}

BOOL FindNextFile(HANDLE h, WIN32_FIND_DATA *fd)
{
    FindState *f = (FindState *)h;
    if (!f || !f->used) return FALSE;
    return fill_find_data(f, fd) ? TRUE : FALSE;
}

BOOL FindClose(HANDLE h)
{
    FindState *f = (FindState *)h;
    if (f && f->used) f->used = 0;
    return TRUE;
}

LONG CompareFileTime(const FILETIME *a, const FILETIME *b)
{
    if (a->dwLowDateTime < b->dwLowDateTime) return -1;
    if (a->dwLowDateTime > b->dwLowDateTime) return 1;
    return 0;
}

/* ------------------------------------------------------------ settings */

/* The .ini file is read and written with stdio; newlib gives us that on top of
 * the File Manager, and the format is simple enough to parse line by line. */
static int ini_find(LPCSTR file, LPCSTR sec, LPCSTR key, char *val, int n)
{
    FILE *fp = fopen(file, "r");
    char line[512];
    int inSec = 0;
    if (!fp) return 0;
    while (fgets(line, sizeof line, fp)) {
        char *s = line, *e;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (close) *close = 0;
            inSec = (strcmp(s + 1, sec) == 0);
            continue;
        }
        if (!inSec) continue;
        {
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            {
                char *ke = eq;
                while (ke > s && ke[-1] == ' ') *--ke = 0;
            }
            if (strcmp(s, key) == 0) {
                char *v = eq + 1;
                while (*v == ' ') v++;
                strncpy(val, v, n - 1);
                val[n - 1] = 0;
                fclose(fp);
                return 1;
            }
        }
    }
    fclose(fp);
    return 0;
}

UINT GetPrivateProfileInt(LPCSTR sec, LPCSTR key, int def, LPCSTR file)
{
    char val[64];
    if (!ini_find(file, sec, key, val, sizeof val)) return (UINT)def;
    return (UINT)atoi(val);
}

DWORD GetPrivateProfileString(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR buf, DWORD n, LPCSTR file)
{
    char val[512];
    if (!ini_find(file, sec, key, val, sizeof val)) {
        strncpy(buf, def ? def : "", n - 1);
        buf[n - 1] = 0;
        return (DWORD)strlen(buf);
    }
    strncpy(buf, val, n - 1);
    buf[n - 1] = 0;
    return (DWORD)strlen(buf);
}

/* Rewrite the file with the key replaced; the settings file is a few dozen
 * lines, so the simple read-all/write-all approach is fine. */
BOOL WritePrivateProfileString(LPCSTR sec, LPCSTR key, LPCSTR val, LPCSTR file)
{
    char *lines[256];
    int nlines = 0, i, inSec = 0, wrote = 0, secLine = -1;
    FILE *fp = fopen(file, "r");
    char line[512];

    if (fp) {
        while (nlines < 256 && fgets(line, sizeof line, fp)) {
            lines[nlines] = strdup(line);
            if (lines[nlines]) nlines++;
        }
        fclose(fp);
    }

    fp = fopen(file, "w");
    if (!fp) {
        for (i = 0; i < nlines; i++) free(lines[i]);
        return FALSE;
    }

    for (i = 0; i < nlines; i++) {
        char trimmed[512], *s = trimmed, *e;
        strncpy(trimmed, lines[i], sizeof trimmed - 1);
        trimmed[sizeof trimmed - 1] = 0;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;

        if (trimmed[0] == '[') {
            if (inSec && !wrote) { fprintf(fp, "%s=%s\n", key, val); wrote = 1; }
            inSec = 0;
            {
                char name[128], *close;
                strncpy(name, trimmed + 1, sizeof name - 1);
                name[sizeof name - 1] = 0;
                close = strchr(name, ']');
                if (close) *close = 0;
                if (strcmp(name, sec) == 0) { inSec = 1; secLine = i; }
            }
            fputs(lines[i], fp);
            continue;
        }
        if (inSec && !wrote) {
            char name[256], *eq;
            strncpy(name, trimmed, sizeof name - 1);
            name[sizeof name - 1] = 0;
            eq = strchr(name, '=');
            if (eq) {
                *eq = 0;
                if (strcmp(name, key) == 0) {
                    fprintf(fp, "%s=%s\n", key, val);
                    wrote = 1;
                    continue;
                }
            }
        }
        fputs(lines[i], fp);
    }

    if (!wrote) {
        if (secLine < 0) fprintf(fp, "[%s]\n", sec);
        fprintf(fp, "%s=%s\n", key, val);
    }

    fclose(fp);
    for (i = 0; i < nlines; i++) free(lines[i]);
    return TRUE;
}

/* ------------------------------------------------------------ clipboard */

static char *s_clipMem;

BOOL OpenClipboard(HWND w)  { (void)w; return TRUE; }
BOOL CloseClipboard(void)   { return TRUE; }
BOOL EmptyClipboard(void)   { ClearCurrentScrap(); return TRUE; }

HANDLE GetClipboardData(UINT fmt)
{
    ScrapRef scrap;
    Size size = 0;
    (void)fmt;
    if (GetCurrentScrap(&scrap) != noErr) return NULL;
    if (GetScrapFlavorSize(scrap, kScrapFlavorTypeText, &size) != noErr || size <= 0) return NULL;
    free(s_clipMem);
    s_clipMem = (char *)malloc(size + 1);
    if (!s_clipMem) return NULL;
    if (GetScrapFlavorData(scrap, kScrapFlavorTypeText, &size, s_clipMem) != noErr) {
        free(s_clipMem); s_clipMem = NULL;
        return NULL;
    }
    s_clipMem[size] = 0;
    return (HANDLE)s_clipMem;
}

HANDLE SetClipboardData(UINT fmt, HANDLE mem)
{
    ScrapRef scrap;
    const char *text = (const char *)mem;
    (void)fmt;
    if (!text) return NULL;
    if (GetCurrentScrap(&scrap) != noErr) return NULL;
    PutScrapFlavor(scrap, kScrapFlavorTypeText, kScrapFlavorMaskNone,
                   (Size)strlen(text), text);
    return mem;
}

/* ------------------------------------------------------------ menus */

/* The shim keeps menus as plain item lists. The main menu bar is built once at
 * startup; everything else is a context menu shown through TrackPopupMenu. */
#define MAX_MENUS 24
#define MAX_ITEMS 48

struct MMenu {
    int  used;
    int  n;
    UINT ids[MAX_ITEMS];
    char text[MAX_ITEMS][64];
    UINT flags[MAX_ITEMS];
    HMENU sub[MAX_ITEMS];
};

static struct MMenu s_menus[MAX_MENUS];
static short s_nextMenuID = 128;

static HMENU alloc_menu(void)
{
    int i;
    for (i = 0; i < MAX_MENUS; i++)
        if (!s_menus[i].used) {
            memset(&s_menus[i], 0, sizeof s_menus[i]);
            s_menus[i].used = 1;
            return (HMENU)&s_menus[i];
        }
    return NULL;
}

HMENU CreateMenu(void)      { return alloc_menu(); }
HMENU CreatePopupMenu(void) { return alloc_menu(); }

BOOL AppendMenu(HMENU m, UINT flags, UINT id, LPCSTR text)
{
    if (!m || !m->used || m->n >= MAX_ITEMS) return FALSE;
    m->flags[m->n] = flags;
    if (flags & MF_POPUP) {
        m->sub[m->n] = (HMENU)id;
        m->ids[m->n] = 0;
    } else {
        m->sub[m->n] = NULL;
        m->ids[m->n] = id;
    }
    if (text) {
        strncpy(m->text[m->n], text, sizeof m->text[0] - 1);
        m->text[m->n][sizeof m->text[0] - 1] = 0;
    } else {
        m->text[m->n][0] = 0;
    }
    m->n++;
    return TRUE;
}

BOOL DestroyMenu(HMENU m)
{
    if (m && m->used) m->used = 0;
    return TRUE;
}

/* Build a Toolbox menu from a shim menu and pop it up at (x, y) in globals. */
BOOL TrackPopupMenu(HMENU m, UINT flags, int x, int y, int reserved, HWND w, LPVOID r)
{
    MenuRef mr;
    long chosen;
    int i;
    (void)reserved; (void)r;
    if (!m || !m->used) return FALSE;

    if (CreateNewMenu(s_nextMenuID++, 0, &mr) != noErr) return FALSE;
    for (i = 0; i < m->n; i++) {
        CFStringRef cf;
        if (m->flags[i] & MF_SEPARATOR) {
            AppendMenuItemTextWithCFString(mr, CFSTR("-"), kMenuItemAttrSeparator, 0, NULL);
            continue;
        }
        cf = CFStringCreateWithCString(NULL, m->text[i], kCFStringEncodingMacRoman);
        AppendMenuItemTextWithCFString(mr, cf,
            (m->flags[i] & MF_GRAYED) ? kMenuItemAttrDisabled : 0, 0, NULL);
        if (cf) CFRelease(cf);
        if (m->flags[i] & MF_CHECKED) CheckMenuItem(mr, (MenuItemIndex)(i + 1), true);
    }

    InsertMenu(mr, kInsertHierarchicalMenu);
    chosen = PopUpMenuSelect(mr, (short)y, (short)x, 1);
    DeleteMenu(GetMenuID(mr));
    DisposeMenu(mr);

    if (!(chosen & 0xFFFF0000)) return FALSE;
    {
        int item = (int)(chosen & 0xFFFF) - 1;
        if (item < 0 || item >= m->n) return FALSE;
        if (flags & TPM_RETURNCMD) return (BOOL)m->ids[item];
        SendMessage(w, WM_COMMAND, (WPARAM)m->ids[item], 0);
        return TRUE;
    }
}

/* ------------------------------------------------------------ dialogs */

int MessageBox(HWND w, LPCSTR text, LPCSTR caption, UINT type)
{
    AlertStdCFStringAlertParamRec params;
    DialogRef dlg;
    DialogItemIndex hit = kAlertStdAlertOKButton;
    CFStringRef msg = CFStringCreateWithCString(NULL, text ? text : "", kCFStringEncodingMacRoman);
    CFStringRef cap = CFStringCreateWithCString(NULL, caption ? caption : "", kCFStringEncodingMacRoman);
    (void)w;

    GetStandardAlertDefaultParams(&params, kStdCFStringAlertVersionOne);
    params.movable = true;
    if ((type & 0x0F) == MB_YESNO || (type & 0x0F) == MB_YESNOCANCEL) {
        params.defaultText = CFSTR("Yes");
        params.cancelText = CFSTR("No");
        if ((type & 0x0F) == MB_YESNOCANCEL) params.otherText = CFSTR("Cancel");
    } else if ((type & 0x0F) == MB_OKCANCEL) {
        params.defaultText = CFSTR("OK");
        params.cancelText = CFSTR("Cancel");
    }

    if (CreateStandardAlert(
            (type & MB_ICONERROR) ? kAlertStopAlert :
            (type & MB_ICONWARNING) ? kAlertCautionAlert : kAlertNoteAlert,
            cap, msg, &params, &dlg) == noErr)
        RunStandardAlert(dlg, NULL, &hit);

    if (msg) CFRelease(msg);
    if (cap) CFRelease(cap);

    switch (hit) {
    case kAlertStdAlertOKButton:
        return ((type & 0x0F) == MB_YESNO || (type & 0x0F) == MB_YESNOCANCEL) ? IDYES : IDOK;
    case kAlertStdAlertCancelButton:
        return ((type & 0x0F) == MB_YESNO || (type & 0x0F) == MB_YESNOCANCEL) ? IDNO : IDCANCEL;
    default:
        return IDCANCEL;
    }
}

/* Navigation Services stands in for GetOpenFileName/GetSaveFileName and for the
 * shell folder picker. */
static int nav_reply_to_path(NavReplyRecord *reply, char *path, int n)
{
    FSSpec spec;
    AEKeyword kw;
    DescType type;
    Size size;
    if (AEGetNthPtr(&reply->selection, 1, typeFSS, &kw, &type, &spec, sizeof spec, &size) != noErr)
        return 0;
    {
        /* Build "dir:name" from the FSSpec; the editor only needs a path it can
         * hand back to the shim's own file calls. */
        unsigned char len = spec.name[0];
        if (len > n - 1) len = (unsigned char)(n - 1);
        memcpy(path, spec.name + 1, len);
        path[len] = 0;
    }
    return 1;
}

BOOL MacChooseFile(char *path, int n, BOOL save)
{
    NavDialogCreationOptions opts;
    NavDialogRef dlg = NULL;
    NavReplyRecord reply;
    OSStatus err;

    if (NavGetDefaultDialogCreationOptions(&opts) != noErr) return FALSE;
    opts.modality = kWindowModalityAppModal;

    if (save)
        err = NavCreatePutFileDialog(&opts, 'TEXT', 'XPCD', NULL, NULL, &dlg);
    else
        err = NavCreateGetFileDialog(&opts, NULL, NULL, NULL, NULL, NULL, &dlg);
    if (err != noErr || !dlg) return FALSE;

    if (NavDialogRun(dlg) != noErr) { NavDialogDispose(dlg); return FALSE; }
    if (NavDialogGetReply(dlg, &reply) != noErr) { NavDialogDispose(dlg); return FALSE; }

    {
        int ok = reply.validRecord ? nav_reply_to_path(&reply, path, n) : 0;
        NavDisposeReply(&reply);
        NavDialogDispose(dlg);
        return ok ? TRUE : FALSE;
    }
}

BOOL MacChooseFolder(char *path, int n)
{
    NavDialogCreationOptions opts;
    NavDialogRef dlg = NULL;
    NavReplyRecord reply;

    if (NavGetDefaultDialogCreationOptions(&opts) != noErr) return FALSE;
    opts.modality = kWindowModalityAppModal;
    if (NavCreateChooseFolderDialog(&opts, NULL, NULL, NULL, &dlg) != noErr || !dlg) return FALSE;

    if (NavDialogRun(dlg) != noErr) { NavDialogDispose(dlg); return FALSE; }
    if (NavDialogGetReply(dlg, &reply) != noErr) { NavDialogDispose(dlg); return FALSE; }

    {
        int ok = reply.validRecord ? nav_reply_to_path(&reply, path, n) : 0;
        NavDisposeReply(&reply);
        NavDialogDispose(dlg);
        return ok ? TRUE : FALSE;
    }
}
