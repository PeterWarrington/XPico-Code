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

static int fsspec_to_path(const FSSpec *spec, char *out, int n);
static int path_to_fsspec(const char *path, FSSpec *spec);

/* Classic Mac files often have no extension at all, so when the editor cannot
 * tell the language from the name, fall back to the Finder type and then to a
 * look at the first line. Returns one of the LANG_* values, or -1 to leave the
 * caller's own guess alone. The LANG_* numbers are passed in rather than
 * included, so this file stays free of the app's headers. */
int mac_lang_sniff(const char *path, int lang_plain, int lang_c, int lang_py,
                   int lang_js, int lang_html, int lang_md)
{
    FSSpec spec;
    FInfo fi;
    FILE *fp;
    char line[256];
    size_t k;

    if (path_to_fsspec(path, &spec) && FSpGetFInfo(&spec, &fi) == noErr) {
        switch (fi.fdType) {
        case 'TEXT': break;                 /* plain text: look inside */
        case 'HTML': return lang_html;
        default:     return -1;             /* not a text file we know */
        }
    }

    fp = fopen(path, "rb");
    if (!fp) return -1;
    k = fread(line, 1, sizeof line - 1, fp);
    fclose(fp);
    line[k] = 0;

    /* a hashbang names the interpreter outright */
    if (line[0] == '#' && line[1] == '!') {
        if (strstr(line, "python")) return lang_py;
        if (strstr(line, "node")) return lang_js;
        return lang_plain;
    }
    if (strstr(line, "<!DOCTYPE") || strstr(line, "<html") || strstr(line, "<?xml")) return lang_html;
    if (strstr(line, "#include") || strstr(line, "#ifndef") || strstr(line, "/*")) return lang_c;
    if (strstr(line, "# ") == line) return lang_md;
    return -1;
}

/* Classic Mac OS only consults an application's BNDL -- and so only shows its
 * custom icon -- when the file's bundle bit is set in its Finder info. Nothing
 * in the build chain sets it (Rez writes the type and creator, and hfsutils'
 * hattrib -b blesses folders, which is a different flag), so the app sets it
 * on itself at startup. It is a one-off: once set it stays set.
 *
 * The Finder caches icons in the desktop database, so a volume that has
 * already been seen may need the desktop rebuilt before the new icon shows. */
void mac_bless_self(void)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec spec;
    FInfo fi;

    GetCurrentProcess(&psn);
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processAppSpec = &spec;
    if (GetProcessInformation(&psn, &info) != noErr) return;
    if (FSpGetFInfo(&spec, &fi) != noErr) return;
    if (fi.fdFlags & kHasBundle) return;          /* already done */

    fi.fdFlags |= kHasBundle;
    FSpSetFInfo(&spec, &fi);                      /* read-only volume: harmless */
}

/* "Reveal in Finder": send the Finder an 'odoc' for the item's parent folder
 * and a selection event for the item itself. ShellExecute cannot stand in for
 * this -- there is no shell to run, which is why the menu item did nothing. */
/* Locate a running application by creator signature. */
static int mac_find_process(OSType signature, ProcessSerialNumber *out)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    Str255 name;
    FSSpec spec;

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        memset(&info, 0, sizeof info);
        info.processInfoLength = sizeof info;
        info.processName = name;
        info.processAppSpec = &spec;
        if (GetProcessInformation(&psn, &info) != noErr) continue;
        if (info.processSignature == signature) { *out = psn; return 1; }
    }
    return 0;
}

/* Send the Finder one event about one item. */
static OSErr finder_event(AEEventClass cls, AEEventID id, const FSSpec *spec)
{
    AEDesc target, elem;
    AEDescList list;
    AppleEvent ae, reply;
    OSType finder = 'MACS';
    OSErr err;

    if (AECreateDesc(typeApplSignature, &finder, sizeof finder, &target) != noErr)
        return paramErr;
    err = AECreateAppleEvent(cls, id, &target, kAutoGenerateReturnID,
                             kAnyTransactionID, &ae);
    AEDisposeDesc(&target);
    if (err != noErr) return err;

    if (AECreateList(NULL, 0, false, &list) != noErr) { AEDisposeDesc(&ae); return paramErr; }
    if (AECreateDesc(typeFSS, spec, sizeof *spec, &elem) == noErr) {
        AEPutDesc(&list, 0, &elem);
        AEDisposeDesc(&elem);
    }
    AEPutParamDesc(&ae, keyDirectObject, &list);
    AEDisposeDesc(&list);

    /* The Finder has to be allowed to come forward, and we wait for the reply
     * so a failure is actually reported instead of vanishing. */
    err = AESend(&ae, &reply, kAEWaitReply | kAECanInteract | kAECanSwitchLayer,
                 kAENormalPriority, 60 * 10, NULL, NULL);
    if (err == noErr) AEDisposeDesc(&reply);
    AEDisposeDesc(&ae);
    return err;
}

int mac_reveal_in_finder(const char *path)
{
    char parent[MAX_PATH * 2];
    FSSpec itemSpec, dirSpec;
    char *lastSep;
    int haveItem, haveDir = 0;

    haveItem = path_to_fsspec(path, &itemSpec);

    /* "Reveal" means showing the item inside its enclosing folder, so open
     * that folder first: an 'odoc' on a folder is what the Finder understands,
     * and selecting an item in a window that is not open does nothing. */
    strncpy(parent, path, sizeof parent - 1);
    parent[sizeof parent - 1] = 0;
    lastSep = strrchr(parent, ':');
    if (lastSep && lastSep != parent) {
        *lastSep = 0;
        haveDir = path_to_fsspec(parent, &dirSpec);
    }

    if (haveDir && finder_event(kCoreEventClass, kAEOpenDocuments, &dirSpec) != noErr)
        haveDir = 0;

    /* Then select the item itself. Not every system build answers this, so a
     * failure here still leaves the folder open, which is the useful part. */
    if (haveItem) finder_event(kAEMiscStandards, kAESelect, &itemSpec);

    /* bring the Finder forward so the window is actually visible */
    if (haveDir || haveItem) {
        ProcessSerialNumber psn;
        OSType sig = 'MACS';
        if (mac_find_process(sig, &psn)) SetFrontProcess(&psn);
    }
    return haveDir || haveItem;
}

/* --------------------------------------------------- dropped documents */

/* Files dropped on the application icon, or opened from the Finder, arrive as
 * an 'odoc' Apple Event. They are packed into the HDROP shape DragQueryFile
 * expects and handed to the app as WM_DROPFILES, which is the path main.c
 * already has for drag and drop. */
static char s_drop[2048];

void mac_drop_release(void *d) { (void)d; s_drop[0] = 0; }

static pascal OSErr handle_odoc(const AppleEvent *ae, AppleEvent *reply, long ref)
{
    AEDescList docs;
    long count = 0, i;
    char *out = s_drop + 1;
    size_t left = sizeof s_drop - 2;
    int packed = 0;
    (void)reply; (void)ref;

    if (AEGetParamDesc(ae, keyDirectObject, typeAEList, &docs) != noErr) return errAEEventNotHandled;
    AECountItems(&docs, &count);

    for (i = 1; i <= count; i++) {
        FSSpec spec;
        AEKeyword kw;
        DescType type;
        Size size;
        char path[MAX_PATH * 2];
        size_t len;
        if (AEGetNthPtr(&docs, i, typeFSS, &kw, &type, &spec, sizeof spec, &size) != noErr) continue;
        if (!fsspec_to_path(&spec, path, (int)sizeof path)) continue;
        len = strlen(path) + 1;
        if (len > left) break;
        memcpy(out, path, len);
        out += len;
        left -= len;
        packed++;
    }
    AEDisposeDesc(&docs);

    if (packed) {
        HWND root;
        s_drop[0] = (char)packed;
        *out = 0;
        root = shim_root_window();
        if (root) SendMessage(root, WM_DROPFILES, (WPARAM)s_drop, 0);
    }
    return noErr;
}

void mac_install_drop_handler(void)
{
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments,
                          NewAEEventHandlerUPP(handle_odoc), 0, false);
}

/* ------------------------------------------------------- system version */

/* gestaltSystemVersion is BCD: 0x0922 is Mac OS 9.2.2, 0x1000 and up is Mac OS
 * X. Later systems cap this selector at 0x1039, which is still >= 0x1000, so
 * the test holds for every system this binary can run on. */
int mac_is_osx(void)
{
    long ver = 0;
    if (Gestalt(gestaltSystemVersion, &ver) != noErr) return 0;
    return ver >= 0x1000;
}

/* "9.2.2" or "10.4" for display. */
void mac_system_version_string(char *buf, int n)
{
    long ver = 0;
    if (Gestalt(gestaltSystemVersion, &ver) != noErr) { snprintf(buf, (size_t)n, "?"); return; }
    snprintf(buf, (size_t)n, "%d.%d.%d",
             (int)((ver >> 8) & 0xFF) / 16 * 10 + (int)((ver >> 8) & 0x0F),
             (int)((ver >> 4) & 0x0F),
             (int)(ver & 0x0F));
}

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

static int fsspec_to_path(const FSSpec *spec, char *out, int n);

/* fnfErr counts as success: FSMakeFSSpec fills in a perfectly usable spec for
 * an item that does not exist yet, which is exactly what creating a file or a
 * folder needs. Treating it as failure meant FSpDirCreate was never reached
 * and "New Folder" silently did nothing. Callers that require the item to
 * exist fail at their next call anyway. */
static int path_to_fsspec(const char *path, FSSpec *spec)
{
    char hfs[MAX_PATH * 2];
    Str255 p;
    OSErr err;
    to_hfs(path, hfs, sizeof hfs);
    to_pstr(hfs, p);
    err = FSMakeFSSpec(0, 0, p, spec);
    return err == noErr || err == fnfErr;
}

/* Classic Mac OS identifies a file by its type and creator rather than its
 * extension, so a file written with plain stdio gets no type at all and the
 * Finder shows it as a blank document that nothing will open. Tag text files
 * as TEXT, owned by SimpleText so they stay double-clickable. */
int mac_set_file_type(const char *path, unsigned long type, unsigned long creator)
{
    FSSpec spec;
    FInfo fi;
    if (!path_to_fsspec(path, &spec)) return 0;
    if (FSpGetFInfo(&spec, &fi) != noErr) return 0;
    fi.fdType = (OSType)type;
    fi.fdCreator = (OSType)creator;
    return FSpSetFInfo(&spec, &fi) == noErr;
}

/* Delete a file or an empty-able directory. FSpDelete refuses a non-empty
 * folder, so its contents are removed first. */
/* Mac OS 9 has no change notification, so a watch records the folder's
 * modification date and the poll reports whether it has moved. The Finder
 * touches the parent folder on any create, rename or delete inside it. */
typedef struct { int used; short vRefNum; long dirID; unsigned long mod; } Watch;
#define MAX_WATCHES 4
static Watch s_watches[MAX_WATCHES];

static unsigned long dir_mod_date(short vRefNum, long dirID)
{
    CInfoPBRec pb;
    Str255 name;
    memset(&pb, 0, sizeof pb);
    name[0] = 0;
    pb.dirInfo.ioNamePtr = name;
    pb.dirInfo.ioVRefNum = vRefNum;
    pb.dirInfo.ioDrDirID = dirID;
    pb.dirInfo.ioFDirIndex = -1;
    if (PBGetCatInfoSync(&pb) != noErr) return 0;
    return pb.dirInfo.ioDrMdDat;
}

void *mac_watch_open(const char *path)
{
    FSSpec spec;
    CInfoPBRec pb;
    int i;
    if (!path_to_fsspec(path, &spec)) return NULL;

    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = spec.name;
    pb.dirInfo.ioVRefNum = spec.vRefNum;
    pb.dirInfo.ioDrDirID = spec.parID;
    pb.dirInfo.ioFDirIndex = 0;
    if (PBGetCatInfoSync(&pb) != noErr) return NULL;

    for (i = 0; i < MAX_WATCHES; i++)
        if (!s_watches[i].used) {
            s_watches[i].used = 1;
            s_watches[i].vRefNum = spec.vRefNum;
            s_watches[i].dirID = pb.dirInfo.ioDrDirID;
            s_watches[i].mod = dir_mod_date(spec.vRefNum, pb.dirInfo.ioDrDirID);
            return &s_watches[i];
        }
    return NULL;
}

int mac_watch_changed(void *h)
{
    Watch *w = (Watch *)h;
    unsigned long now;
    if (!w || !w->used) return 0;
    now = dir_mod_date(w->vRefNum, w->dirID);
    if (now != w->mod) { w->mod = now; return 1; }
    return 0;
}

void mac_watch_close(void *h)
{
    Watch *w = (Watch *)h;
    if (w && w->used) w->used = 0;
}

int mac_delete_path(const char *path)
{
    FSSpec spec;
    DWORD attr = GetFileAttributes(path);

    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        char pattern[MAX_PATH * 2];
        WIN32_FIND_DATA fd;
        HANDLE h;
        snprintf(pattern, sizeof pattern, "%s:*", path);
        h = FindFirstFile(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char child[MAX_PATH * 2];
                if (fd.cFileName[0] == 0) continue;
                snprintf(child, sizeof child, "%s:%s", path, fd.cFileName);
                mac_delete_path(child);
            } while (FindNextFile(h, &fd));
            FindClose(h);
        }
    }
    if (!path_to_fsspec(path, &spec)) return 0;
    return FSpDelete(&spec) == noErr;
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

    /* Same folder: a plain rename. */
    if (a.vRefNum == b.vRefNum && a.parID == b.parID)
        return FSpRename(&a, b.name) == noErr;

    /* Different folder on the same volume: move, then rename if the leaf name
     * changed too. FSpCatMove cannot cross volumes, so that case is copied. */
    if (a.vRefNum == b.vRefNum) {
        FSSpec destDir;
        destDir.vRefNum = b.vRefNum;
        destDir.parID = fsRtParID;
        if (FSpCatMove(&a, &b) == noErr) {
            FSSpec moved = a;
            moved.parID = b.parID;
            if (!EqualString(a.name, b.name, false, true))
                return FSpRename(&moved, b.name) == noErr;
            return TRUE;
        }
        (void)destDir;
        return FALSE;
    }

    /* Across volumes: copy the bytes, then remove the original. */
    {
        FILE *in = fopen(from, "rb"), *out;
        char buf[4096];
        size_t k;
        if (!in) return FALSE;
        out = fopen(to, "wb");
        if (!out) { fclose(in); return FALSE; }
        while ((k = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, k, out) != k) { fclose(in); fclose(out); return FALSE; }
        fclose(in);
        fclose(out);
        mac_set_file_type(to, 'TEXT', 'ttxt');
        return mac_delete_path(from) ? TRUE : TRUE;
    }
}

DWORD GetFullPathName(LPCSTR name, DWORD n, LPSTR buf, LPSTR *part)
{
    DWORD len;
    FSSpec spec;

    /* Resolve through the File Manager so a partial path becomes a real
     * Volume:folder:name one; fall back to the input if it does not exist. */
    if (path_to_fsspec(name, &spec) && fsspec_to_path(&spec, buf, (int)n)) {
        len = (DWORD)strlen(buf);
        if (part) {
            char *p = strrchr(buf, ':');
            *part = p ? p + 1 : buf;
        }
        return len;
    }

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
    /* The full path, not just the name: main.c strips the last component to
     * get the application's folder, and a bare name left that empty, so the
     * settings file had nowhere to live and preferences never persisted. */
    if (GetProcessInformation(&psn, &info) == noErr)
        fsspec_to_path(&spec, buf, (int)n);
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

/* ---- the menu bar ----
 *
 * main.c builds a Win32 menu whose top level is all MF_POPUP items and hands it
 * to CreateWindowEx. Each of those submenus becomes a Toolbox menu in the Mac
 * menu bar, and MenuSelect's result is mapped back to the command id. */

#define MAX_BAR 12
static struct { short id; HMENU sub; int helpOffset; } s_bar[MAX_BAR];
static int s_nbar;

/* Win32 item text carries a mnemonic ampersand and a tab-separated accelerator
 * ("&New File\tCtrl+N"); the Mac shows neither. */
static void clean_title(const char *in, char *out, int n)
{
    int i = 0;
    for (; *in && i < n - 1; in++) {
        if (*in == '&') continue;
        if (*in == '\t') break;
        out[i++] = *in;
    }
    out[i] = 0;
}

void menubar_install(HMENU root)
{
    int i;
    if (!root || !root->used) return;

    ClearMenuBar();
    s_nbar = 0;

    for (i = 0; i < root->n && s_nbar < MAX_BAR; i++) {
        MenuRef mr;
        CFStringRef cf;
        char title[64];
        short id;
        int j;
        HMENU sub = root->sub[i];

        if (!(root->flags[i] & MF_POPUP) || !sub || !sub->used) continue;

        clean_title(root->text[i], title, sizeof title);

        /* Nothing in the Terminal menu can work without a Unix shell, so it is
         * left out entirely on Mac OS 9 rather than offering dead commands. */
        if (!mac_is_osx() && strcmp(title, "Terminal") == 0) continue;

        /* The system already owns a Help menu. Adding another gives two, so the
         * app's help items are appended to the system one instead; MenuSelect
         * reports them with the system menu's id, offset past Apple's items. */
        if (strcmp(title, "Help") == 0) {
            MenuRef help;
            MenuItemIndex first = 0;
            if (HMGetHelpMenu(&help, &first) == noErr && help) {
                for (j = 0; j < sub->n; j++) {
                    CFStringRef icf;
                    char itext[64];
                    if (sub->flags[j] & MF_SEPARATOR) {
                        AppendMenuItemTextWithCFString(help, CFSTR("-"),
                                                       kMenuItemAttrSeparator, 0, NULL);
                        continue;
                    }
                    clean_title(sub->text[j], itext, sizeof itext);
                    icf = CFStringCreateWithCString(NULL, itext, kCFStringEncodingMacRoman);
                    AppendMenuItemTextWithCFString(help, icf ? icf : CFSTR(""), 0, 0, NULL);
                    if (icf) CFRelease(icf);
                }
                s_bar[s_nbar].id = GetMenuID(help);
                s_bar[s_nbar].sub = sub;
                s_bar[s_nbar].helpOffset = (int)first;
                s_nbar++;
            }
            continue;
        }

        id = s_nextMenuID++;
        cf = CFStringCreateWithCString(NULL, title, kCFStringEncodingMacRoman);
        if (CreateNewMenu(id, 0, &mr) != noErr) { if (cf) CFRelease(cf); continue; }
        if (cf) { SetMenuTitleWithCFString(mr, cf); CFRelease(cf); }

        for (j = 0; j < sub->n; j++) {
            char itext[64];
            CFStringRef icf;
            if (sub->flags[j] & MF_SEPARATOR) {
                AppendMenuItemTextWithCFString(mr, CFSTR("-"), kMenuItemAttrSeparator, 0, NULL);
                continue;
            }
            clean_title(sub->text[j], itext, sizeof itext);
            icf = CFStringCreateWithCString(NULL, itext, kCFStringEncodingMacRoman);
            AppendMenuItemTextWithCFString(mr, icf ? icf : CFSTR(""),
                (sub->flags[j] & MF_GRAYED) ? kMenuItemAttrDisabled : 0, 0, NULL);
            if (icf) CFRelease(icf);
            if (sub->flags[j] & MF_CHECKED) CheckMenuItem(mr, (MenuItemIndex)(j + 1), true);
        }

        InsertMenu(mr, 0);
        s_bar[s_nbar].id = id;
        s_bar[s_nbar].sub = sub;
        s_bar[s_nbar].helpOffset = 0;
        s_nbar++;
    }
    DrawMenuBar();
}

/* Map MenuSelect's packed result to the app's command id, or 0. */
int menubar_command(long selection)
{
    short id = (short)((selection >> 16) & 0xFFFF);
    short item = (short)(selection & 0xFFFF);
    int i;
    if (!id || item < 1) return 0;
    for (i = 0; i < s_nbar; i++)
        if (s_bar[i].id == id) {
            HMENU sub = s_bar[i].sub;
            /* items appended to the system Help menu start after Apple's own */
            int idx = item - 1 - (s_bar[i].helpOffset ? s_bar[i].helpOffset - 1 : 0);
            if (idx < 0 || idx >= sub->n) return 0;
            return (int)sub->ids[idx];
        }
    return 0;
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

/* StandardAlert, not CreateStandardAlert: the CarbonLib import library in the
 * Universal Interfaces exports the former but not the CFString-based calls. */
int MessageBox(HWND w, LPCSTR text, LPCSTR caption, UINT type)
{
    AlertStdAlertParamRec params;
    SInt16 hit = 1;
    Str255 pcap, ptext;
    Str255 yes, no, cancel, ok;
    int kind = type & 0x0F;
    (void)w;

    to_pstr(caption ? caption : "", pcap);
    to_pstr(text ? text : "", ptext);
    {   /* the Dialog Manager breaks lines on CR; LF renders as a box */
        int k;
        for (k = 1; k <= ptext[0]; k++)
            if (ptext[k] == '\n') ptext[k] = '\r';
    }
    to_pstr("Yes", yes); to_pstr("No", no);
    to_pstr("Cancel", cancel); to_pstr("OK", ok);

    memset(&params, 0, sizeof params);
    params.movable = true;
    params.defaultButton = kAlertStdAlertOKButton;

    if (kind == MB_YESNO || kind == MB_YESNOCANCEL) {
        params.defaultText = yes;
        params.cancelText = no;
        if (kind == MB_YESNOCANCEL) params.otherText = cancel;
        params.cancelButton = kAlertStdAlertCancelButton;
    } else if (kind == MB_OKCANCEL) {
        params.defaultText = ok;
        params.cancelText = cancel;
        params.cancelButton = kAlertStdAlertCancelButton;
    } else {
        params.defaultText = ok;
    }

    StandardAlert((type & MB_ICONERROR) ? kAlertStopAlert :
                  (type & MB_ICONWARNING) ? kAlertCautionAlert : kAlertNoteAlert,
                  pcap, ptext, &params, &hit);

    if (kind == MB_YESNO || kind == MB_YESNOCANCEL) {
        if (hit == kAlertStdAlertOKButton) return IDYES;
        if (hit == kAlertStdAlertCancelButton) return IDNO;
        return IDCANCEL;
    }
    return hit == kAlertStdAlertOKButton ? IDOK : IDCANCEL;
}

/* Navigation Services stands in for GetOpenFileName/GetSaveFileName and for the
 * shell folder picker. */
/* Walk an FSSpec up to the volume root to build a full "Volume:dir:name" path.
 * The editor stores whatever comes back and passes it to the shim's own file
 * calls, so it has to be a path FSMakeFSSpec can resolve again. */
static int fsspec_to_path(const FSSpec *spec, char *out, int n)
{
    char tail[MAX_PATH * 2];
    char head[MAX_PATH * 2];
    long dirID = spec->parID;
    int isDir = 0;

    /* the leaf name */
    {
        unsigned char len = spec->name[0];
        if (len > sizeof tail - 1) len = (unsigned char)(sizeof tail - 1);
        memcpy(tail, spec->name + 1, len);
        tail[len] = 0;
    }

    /* prepend each parent directory name, ending with the volume */
    head[0] = 0;
    while (dirID != 0) {
        CInfoPBRec pb;
        Str255 name;
        char seg[MAX_PATH];
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.dirInfo.ioNamePtr = name;
        pb.dirInfo.ioVRefNum = spec->vRefNum;
        pb.dirInfo.ioDrDirID = dirID;
        pb.dirInfo.ioFDirIndex = -1;          /* look up the directory itself */
        if (PBGetCatInfoSync(&pb) != noErr) break;

        memcpy(seg, name + 1, name[0]);
        seg[name[0]] = 0;

        {   /* prepend "seg:" to head, giving up if the path grows too long */
            size_t sl = strlen(seg), hl = strlen(head);
            if (sl + 1 + hl >= sizeof head) break;
            memmove(head + sl + 1, head, hl + 1);
            memcpy(head, seg, sl);
            head[sl] = ':';
        }

        if (pb.dirInfo.ioDrDirID == fsRtDirID) break;   /* reached the volume */
        dirID = pb.dirInfo.ioDrParID;
        isDir = 1;
    }
    (void)isDir;

    snprintf(out, (size_t)n, "%s%s", head, tail);
    return out[0] != 0;
}

static int nav_reply_to_path(NavReplyRecord *reply, char *path, int n)
{
    FSSpec spec;
    AEKeyword kw;
    DescType type;
    Size size;
    if (AEGetNthPtr(&reply->selection, 1, typeFSS, &kw, &type, &spec, sizeof spec, &size) != noErr)
        return 0;
    return fsspec_to_path(&spec, path, n);
}

/* Navigation Services has to be pulled in explicitly on classic Mac OS; without
 * this the Nav calls fail and no dialog ever appears. */
static int nav_ready(void)
{
    static int loaded;
    if (!loaded) {
        if (NavLoad() != noErr) return 0;
        loaded = 1;
    }
    return 1;
}

/* Report a failed Toolbox call rather than returning silently: a save that does
 * nothing at all gives the user nothing to act on. */
static void nav_failed(const char *what, OSStatus err)
{
    char msg[160];
    snprintf(msg, sizeof msg, "%s failed (error %d).", what, (int)err);
    MessageBox(NULL, msg, "XPico Code", MB_ICONERROR);
}

BOOL MacChooseFile(char *path, int n, BOOL save)
{
    NavDialogCreationOptions opts;
    NavDialogRef dlg = NULL;
    NavReplyRecord reply;
    OSStatus err;

    if (!nav_ready()) { nav_failed("Navigation Services", -1); return FALSE; }

    err = NavGetDefaultDialogCreationOptions(&opts);
    if (err != noErr) { nav_failed("NavGetDefaultDialogCreationOptions", err); return FALSE; }
    opts.modality = kWindowModalityAppModal;
    opts.optionFlags |= kNavNoTypePopup;

    if (save) {
        /* Without a default name the Put File dialog has nothing to show in
         * its name field. */
        opts.saveFileName = CFStringCreateWithCString(NULL, "Untitled.txt",
                                                      kCFStringEncodingMacRoman);
        err = NavCreatePutFileDialog(&opts, 'TEXT', 'XPCD', NULL, NULL, &dlg);
        if (opts.saveFileName) CFRelease(opts.saveFileName);
    } else {
        err = NavCreateGetFileDialog(&opts, NULL, NULL, NULL, NULL, NULL, &dlg);
    }
    if (err != noErr || !dlg) {
        nav_failed(save ? "NavCreatePutFileDialog" : "NavCreateGetFileDialog", err);
        return FALSE;
    }

    err = NavDialogRun(dlg);
    if (err != noErr) { nav_failed("NavDialogRun", err); NavDialogDispose(dlg); return FALSE; }
    if (NavDialogGetReply(dlg, &reply) != noErr) { NavDialogDispose(dlg); return FALSE; }

    {
        int ok = 0;
        if (reply.validRecord) {
            if (!save) {
                ok = nav_reply_to_path(&reply, path, n);
            } else {
                /* A Put File reply is shaped differently from an Open one:
                 * selection holds the destination *directory*, and the name the
                 * user typed is in saveFileName. Reading it like an Open reply
                 * produced the folder path, so saving went nowhere. */
                char dir[MAX_PATH * 2];
                char name[256];
                if (nav_reply_to_path(&reply, dir, (int)sizeof dir)) {
                    name[0] = 0;
                    if (reply.saveFileName)
                        CFStringGetCString(reply.saveFileName, name, sizeof name,
                                           kCFStringEncodingMacRoman);
                    if (name[0]) {
                        size_t dl = strlen(dir);
                        if (dl && dir[dl - 1] == ':') dir[dl - 1] = 0;
                        snprintf(path, (size_t)n, "%s:%s", dir, name);
                        ok = 1;
                    }
                }
            }
        }
        NavDisposeReply(&reply);
        NavDialogDispose(dlg);
        return ok ? TRUE : FALSE;
    }
}

/* ---- the Win32 dialog entry points the app actually calls ---- */

/* GetOpenFileName hands back a double-NUL terminated buffer: with a single
 * selection that is the full path followed by an empty string, which is what
 * main.c's open_file_dialog walks. */
BOOL GetOpenFileName(LPVOID p)
{
    OPENFILENAME *ofn = (OPENFILENAME *)p;
    char path[MAX_PATH * 2];
    size_t len;

    if (!ofn || !ofn->lpstrFile || ofn->nMaxFile < 4) return FALSE;
    if (!MacChooseFile(path, (int)sizeof path, FALSE)) return FALSE;

    len = strlen(path);
    if (len + 2 > ofn->nMaxFile) len = ofn->nMaxFile - 2;
    memcpy(ofn->lpstrFile, path, len);
    ofn->lpstrFile[len] = 0;
    ofn->lpstrFile[len + 1] = 0;      /* terminates the list */
    return TRUE;
}

BOOL GetSaveFileName(LPVOID p)
{
    OPENFILENAME *ofn = (OPENFILENAME *)p;
    char path[MAX_PATH * 2];
    size_t len;

    if (!ofn || !ofn->lpstrFile || ofn->nMaxFile < 4) return FALSE;
    if (!MacChooseFile(path, (int)sizeof path, TRUE)) return FALSE;

    len = strlen(path);
    if (len + 2 > ofn->nMaxFile) len = ofn->nMaxFile - 2;
    memcpy(ofn->lpstrFile, path, len);
    ofn->lpstrFile[len] = 0;
    ofn->lpstrFile[len + 1] = 0;
    return TRUE;
}

/* The shell folder picker: the "pidl" is just the chosen path, which
 * SHGetPathFromIDList then copies out. */
static char s_browsePath[MAX_PATH * 2];

LPVOID SHBrowseForFolder(LPVOID bi)
{
    (void)bi;
    if (!MacChooseFolder(s_browsePath, (int)sizeof s_browsePath)) return NULL;
    return s_browsePath;
}

BOOL SHGetPathFromIDList(LPVOID idl, LPSTR path)
{
    if (!idl || !path) return FALSE;
    strcpy(path, (const char *)idl);
    return TRUE;
}

BOOL MacChooseFolder(char *path, int n)
{
    NavDialogCreationOptions opts;
    NavDialogRef dlg = NULL;
    NavReplyRecord reply;
    OSStatus err;

    if (!nav_ready()) { nav_failed("Navigation Services", -1); return FALSE; }

    err = NavGetDefaultDialogCreationOptions(&opts);
    if (err != noErr) { nav_failed("NavGetDefaultDialogCreationOptions", err); return FALSE; }
    opts.modality = kWindowModalityAppModal;

    err = NavCreateChooseFolderDialog(&opts, NULL, NULL, NULL, &dlg);
    if (err != noErr || !dlg) { nav_failed("NavCreateChooseFolderDialog", err); return FALSE; }

    err = NavDialogRun(dlg);
    if (err != noErr) { nav_failed("NavDialogRun", err); NavDialogDispose(dlg); return FALSE; }
    if (NavDialogGetReply(dlg, &reply) != noErr) { NavDialogDispose(dlg); return FALSE; }

    {
        int ok = reply.validRecord ? nav_reply_to_path(&reply, path, n) : 0;
        NavDisposeReply(&reply);
        NavDialogDispose(dlg);
        return ok ? TRUE : FALSE;
    }
}
