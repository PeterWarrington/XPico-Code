/* Files, settings, clipboard, menus and dialogs for the Win32 shim.
 *
 * Paths need no translation in this build: xpcode.h sets PATH_SEP to '/' for
 * the Cocoa target, so the editor builds POSIX paths and they go straight to
 * the system calls. (The Carbon port has to convert at every File Manager
 * boundary, because HFS paths are colon-separated and have no drive letter.)
 *
 * The .ini reader and writer are the Carbon port's, unchanged: the format is
 * simple enough to parse with stdio, and stdio is stdio. */
#import <Cocoa/Cocoa.h>
#include "win32.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>

/* ------------------------------------------------------------ encoding */

/* Mac OS X is UTF-8 throughout -- file contents, file names and the
 * pasteboard -- so that is how the shim reads the editor's bytes, and what it
 * writes back. (The Carbon port uses Mac OS Roman, which is what classic Mac
 * OS and QuickDraw actually are.)
 *
 * The editor itself is byte-oriented: one byte is one character for its caret,
 * column and selection arithmetic. So a multi-byte character displays
 * correctly but counts as several columns, and the arrow keys step through it a
 * byte at a time. Making that whole-character would mean changing editor.c,
 * which is not this port's business.
 *
 * Bytes that are not valid UTF-8 are read as Mac OS Roman rather than dropped:
 * a file saved by a classic Mac application still shows its text.
 *
 * n < 0 means the bytes are NUL-terminated. */
NSString *shim_string(const char *s, int n)
{
    NSString *out;
    NSData *d;
    if (!s) return @"";
    if (n < 0) n = (int)strlen(s);
    d = [NSData dataWithBytes:s length:(unsigned)n];
    out = [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] autorelease];
    if (!out)
        out = [[[NSString alloc] initWithData:d encoding:NSMacOSRomanStringEncoding] autorelease];
    return out ? out : @"";
}

/* The other direction: the bytes the editor should store for a string. */
NSData *shim_bytes(NSString *s)
{
    if (!s) return nil;
    return [s dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:YES];
}

/* ------------------------------------------------------- system version */

/* Always true: this is a Mac OS X binary. It stays because the app's
 * TERMINAL_AVAILABLE() and the shared #ifdef MAC_OS paths ask. */
int mac_is_osx(void) { return 1; }

void mac_system_version_string(char *buf, int n)
{
    NSDictionary *sv = [NSDictionary dictionaryWithContentsOfFile:
                           @"/System/Library/CoreServices/SystemVersion.plist"];
    NSString *ver = sv ? [sv objectForKey:@"ProductVersion"] : nil;
    snprintf(buf, (size_t)n, "%s", ver ? [ver UTF8String] : "?");
}

/* ------------------------------------------------------------ languages */

/* Files without an extension are common enough on the Mac that the editor
 * asks for a second opinion. The HFS type comes first, then a look at the
 * first line. Returns one of the LANG_* values, or -1 to leave the caller's
 * own guess alone. The LANG_* numbers are passed in rather than included, so
 * this file stays free of the app's headers. */
int mac_lang_sniff(const char *path, int lang_plain, int lang_c, int lang_py,
                   int lang_js, int lang_html, int lang_md)
{
    FILE *fp;
    char line[256];
    size_t k;
    NSDictionary *attrs = [[NSFileManager defaultManager]
                              fileAttributesAtPath:shim_string(path, -1)
                                      traverseLink:YES];
    if (attrs) {
        unsigned long type = [[attrs objectForKey:NSFileHFSTypeCode] unsignedLongValue];
        if (type == 'HTML') return lang_html;
        if (type && type != 'TEXT' && type != 0) return -1;   /* not text we know */
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

/* ------------------------------------------------------------ the Finder */

int mac_reveal_in_finder(const char *path)
{
    NSString *p = shim_string(path, -1);
    if (![p length]) return 0;
    return [[NSWorkspace sharedWorkspace] selectFile:p inFileViewerRootedAtPath:@""] ? 1 : 0;
}

/* --------------------------------------------------- dropped documents */

/* Files dropped on the window or the Dock icon, and files opened from the
 * Finder, all arrive through the delegate in mac-cocoa/wnd.m. They are packed
 * into the shape DragQueryFile expects -- a count byte, then that many
 * NUL-terminated paths -- and handed to the app as WM_DROPFILES, which is the
 * path main.c already has for drag and drop. */
static char s_drop[4096];

void mac_drop_release(void *d) { (void)d; s_drop[0] = 0; }

/* Nothing to install: AppKit routes 'odoc' through the application delegate,
 * and the view registers for dragged filenames when it is created. */
void mac_install_drop_handler(void) { }

void mac_drop_files(const char *const *paths, int n)
{
    char *out = s_drop + 1;
    size_t left = sizeof s_drop - 2;
    int packed = 0, i;
    HWND root;

    for (i = 0; i < n; i++) {
        size_t len;
        if (!paths[i]) continue;
        len = strlen(paths[i]) + 1;
        if (len > left) break;
        memcpy(out, paths[i], len);
        out += len;
        left -= len;
        packed++;
    }
    if (!packed) return;
    s_drop[0] = (char)packed;
    *out = 0;
    root = shim_root_window();
    if (root) SendMessage(root, WM_DROPFILES, (WPARAM)s_drop, 0);
}

/* ------------------------------------------------------------ files */

DWORD GetFileAttributes(LPCSTR path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
}

BOOL CreateDirectory(LPCSTR path, LPVOID sa)
{
    (void)sa;
    return (path && mkdir(path, 0755) == 0) ? TRUE : FALSE;
}

BOOL MoveFile(LPCSTR from, LPCSTR to)
{
    if (!from || !to) return FALSE;
    if (rename(from, to) == 0) return TRUE;
    /* rename cannot cross volumes: copy the bytes, then remove the original */
    {
        FILE *in = fopen(from, "rb"), *out;
        char buf[8192];
        size_t k;
        if (!in) return FALSE;
        out = fopen(to, "wb");
        if (!out) { fclose(in); return FALSE; }
        while ((k = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, k, out) != k) { fclose(in); fclose(out); return FALSE; }
        fclose(in);
        fclose(out);
        unlink(from);
        return TRUE;
    }
}

int mac_delete_path(const char *path)
{
    DWORD attr;
    if (!path || !*path) return 0;
    attr = GetFileAttributes(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                char child[MAX_PATH * 2];
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                snprintf(child, sizeof child, "%s/%s", path, e->d_name);
                mac_delete_path(child);
            }
            closedir(d);
        }
        return rmdir(path) == 0;
    }
    return unlink(path) == 0;
}

DWORD GetFullPathName(LPCSTR name, DWORD n, LPSTR buf, LPSTR *part)
{
    char resolved[MAX_PATH * 2];
    if (name && realpath(name, resolved)) {
        strncpy(buf, resolved, n - 1);
        buf[n - 1] = 0;
    } else {
        strncpy(buf, name ? name : "", n - 1);
        buf[n - 1] = 0;
    }
    if (part) {
        char *p = strrchr(buf, '/');
        *part = p ? p + 1 : buf;
    }
    return (DWORD)strlen(buf);
}

/* The full path of the executable inside the bundle. main.c strips the last
 * component to get a folder to look for tools and settings in, so a bare name
 * would leave it empty. */
DWORD GetModuleFileName(HINSTANCE inst, LPSTR buf, DWORD n)
{
    NSString *exe = [[NSBundle mainBundle] executablePath];
    (void)inst;
    buf[0] = 0;
    if (exe) {
        strncpy(buf, [exe fileSystemRepresentation], n - 1);
        buf[n - 1] = 0;
    }
    return (DWORD)strlen(buf);
}

DWORD GetTickCount(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (DWORD)((DWORD)tv.tv_sec * 1000UL + (DWORD)(tv.tv_usec / 1000));
}

/* ------------------------------------------------- directory enumeration */

typedef struct {
    int   used;
    DIR  *dir;
    char  path[MAX_PATH];
} FindState;

#define MAX_FINDS 8
static FindState s_finds[MAX_FINDS];

/* FindFirstFile is called with a pattern like "/dir/" plus a star, or one
 * ending in ".c"; only the directory part is used to walk it, and the app
 * filters the names itself. */
static void split_pattern(const char *pattern, char *dir, int dn)
{
    const char *sep = strrchr(pattern, '/');
    int len = sep ? (int)(sep - pattern) : 0;
    if (len > dn - 1) len = dn - 1;
    memcpy(dir, pattern, (size_t)len);
    dir[len] = 0;
    if (!dir[0]) { dir[0] = sep ? '/' : '.'; dir[1] = 0; }
}

static int fill_find_data(FindState *f, WIN32_FIND_DATA *fd)
{
    struct dirent *e;
    for (;;) {
        char full[MAX_PATH * 2];
        struct stat st;
        e = readdir(f->dir);
        if (!e) return 0;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

        strncpy(fd->cFileName, e->d_name, sizeof fd->cFileName - 1);
        fd->cFileName[sizeof fd->cFileName - 1] = 0;
        fd->cAlternateFileName[0] = 0;
        snprintf(full, sizeof full, "%s/%s", f->path, e->d_name);
        if (stat(full, &st) != 0) memset(&st, 0, sizeof st);
        fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
        if (e->d_name[0] == '.') fd->dwFileAttributes |= FILE_ATTRIBUTE_HIDDEN;
        fd->nFileSizeHigh = 0;
        fd->nFileSizeLow = (DWORD)st.st_size;
        /* Only CompareFileTime consumes these, so the seconds are kept raw and
         * compared on the same scale rather than converted to a FILETIME. */
        fd->ftLastWriteTime.dwLowDateTime = (DWORD)st.st_mtime;
        fd->ftLastWriteTime.dwHighDateTime = 0;
        fd->ftCreationTime = fd->ftLastWriteTime;
        fd->ftLastAccessTime = fd->ftLastWriteTime;
        return 1;
    }
}

HANDLE FindFirstFile(LPCSTR pattern, WIN32_FIND_DATA *fd)
{
    FindState *f = NULL;
    int i;

    for (i = 0; i < MAX_FINDS; i++)
        if (!s_finds[i].used) { f = &s_finds[i]; break; }
    if (!f || !pattern) return INVALID_HANDLE_VALUE;

    split_pattern(pattern, f->path, (int)sizeof f->path);
    f->dir = opendir(f->path);
    if (!f->dir) return INVALID_HANDLE_VALUE;
    f->used = 1;
    if (!fill_find_data(f, fd)) { closedir(f->dir); f->dir = NULL; f->used = 0; return INVALID_HANDLE_VALUE; }
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
    if (f && f->used) {
        if (f->dir) closedir(f->dir);
        f->dir = NULL;
        f->used = 0;
    }
    return TRUE;
}

LONG CompareFileTime(const FILETIME *a, const FILETIME *b)
{
    if (a->dwLowDateTime < b->dwLowDateTime) return -1;
    if (a->dwLowDateTime > b->dwLowDateTime) return 1;
    return 0;
}

/* ------------------------------------------- directory change notification */

/* The explorer wants to know when a folder's contents change. kqueue would do
 * it, but the watch is polled from the editor's own timer anyway, so the
 * modification time is enough -- the kernel touches a directory's mtime on any
 * create, rename or delete inside it. */
typedef struct { int used; char path[MAX_PATH]; long mod; } Watch;
#define MAX_WATCHES 4
static Watch s_watches[MAX_WATCHES];

static long dir_mod_time(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long)st.st_mtime;
}

void *mac_watch_open(const char *path)
{
    int i;
    if (!path || !*path) return NULL;
    for (i = 0; i < MAX_WATCHES; i++)
        if (!s_watches[i].used) {
            s_watches[i].used = 1;
            strncpy(s_watches[i].path, path, sizeof s_watches[i].path - 1);
            s_watches[i].path[sizeof s_watches[i].path - 1] = 0;
            s_watches[i].mod = dir_mod_time(s_watches[i].path);
            return &s_watches[i];
        }
    return NULL;
}

int mac_watch_changed(void *h)
{
    Watch *w = (Watch *)h;
    long now;
    if (!w || !w->used) return 0;
    now = dir_mod_time(w->path);
    if (now != w->mod) { w->mod = now; return 1; }
    return 0;
}

void mac_watch_close(void *h)
{
    Watch *w = (Watch *)h;
    if (w && w->used) w->used = 0;
}

/* ------------------------------------------------------------ settings */

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
 * lines, so the simple read-all/write-all approach is fine.
 *
 * Win32 overloads the NULL arguments, and the app uses both: a NULL key means
 * "delete the whole section", which is how save_session clears last session's
 * file list before writing this one, and a NULL value means "delete this key". */
BOOL WritePrivateProfileString(LPCSTR sec, LPCSTR key, LPCSTR val, LPCSTR file)
{
    char *lines[256];
    int nlines = 0, i, inSec = 0, haveSec = 0, wrote = 0;
    FILE *fp = fopen(file, "r");
    char line[512];

    if (!sec) return FALSE;

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
        char trimmed[512], *e;
        strncpy(trimmed, lines[i], sizeof trimmed - 1);
        trimmed[sizeof trimmed - 1] = 0;
        e = trimmed + strlen(trimmed);
        while (e > trimmed && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;

        if (trimmed[0] == '[') {
            char name[128], *close;
            /* leaving the section without having found the key: add it here */
            if (inSec && key && val && !wrote) { fprintf(fp, "%s=%s\n", key, val); wrote = 1; }
            strncpy(name, trimmed + 1, sizeof name - 1);
            name[sizeof name - 1] = 0;
            close = strchr(name, ']');
            if (close) *close = 0;
            inSec = (strcmp(name, sec) == 0);
            if (inSec) haveSec = 1;
            /* a NULL key takes the section's header with it */
            if (!inSec || key) fputs(lines[i], fp);
            continue;
        }

        if (inSec) {
            char name[256], *eq, *ke;
            if (!key) continue;                      /* deleting the section */
            strncpy(name, trimmed, sizeof name - 1);
            name[sizeof name - 1] = 0;
            eq = strchr(name, '=');
            if (eq) {
                *eq = 0;
                ke = eq;
                while (ke > name && ke[-1] == ' ') *--ke = 0;
                if (strcmp(name, key) == 0) {
                    if (!val) continue;              /* deleting the key */
                    if (!wrote) { fprintf(fp, "%s=%s\n", key, val); wrote = 1; }
                    continue;                        /* and drop any duplicate */
                }
            }
        }
        fputs(lines[i], fp);
    }

    if (key && val && !wrote) {
        if (!haveSec) fprintf(fp, "[%s]\n", sec);
        fprintf(fp, "%s=%s\n", key, val);
    }

    fclose(fp);
    for (i = 0; i < nlines; i++) free(lines[i]);
    return TRUE;
}

/* ------------------------------------------------------------ clipboard */

/* The pasteboard is Unicode; the editor's side of it is UTF-8 bytes. */
static char *s_clipMem;

BOOL OpenClipboard(HWND w)  { (void)w; return TRUE; }
BOOL CloseClipboard(void)   { return TRUE; }

BOOL EmptyClipboard(void)
{
    [[NSPasteboard generalPasteboard]
        declareTypes:[NSArray arrayWithObject:NSStringPboardType] owner:nil];
    return TRUE;
}

HANDLE GetClipboardData(UINT fmt)
{
    NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSStringPboardType];
    NSData *d;
    (void)fmt;
    if (!s) return NULL;
    d = shim_bytes(s);
    if (!d) return NULL;
    free(s_clipMem);
    s_clipMem = (char *)malloc([d length] + 1);
    if (!s_clipMem) return NULL;
    memcpy(s_clipMem, [d bytes], [d length]);
    s_clipMem[[d length]] = 0;
    return (HANDLE)s_clipMem;
}

HANDLE SetClipboardData(UINT fmt, HANDLE mem)
{
    const char *text = (const char *)mem;
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSString *s;
    (void)fmt;
    if (!text) return NULL;
    s = shim_string(text, -1);
    if (!s) return NULL;
    [pb declareTypes:[NSArray arrayWithObject:NSStringPboardType] owner:nil];
    [pb setString:s forType:NSStringPboardType];
    return mem;
}

/* ------------------------------------------------------------ menus */

/* The shim keeps menus as plain item lists, as the Carbon port does. The main
 * menu bar is built once at startup; everything else is a context menu shown
 * through TrackPopupMenu. */
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

BOOL AppendMenu(HMENU m, UINT flags, UINT_PTR id, LPCSTR text)
{
    if (!m || !m->used || m->n >= MAX_ITEMS) return FALSE;
    m->flags[m->n] = flags;
    if (flags & MF_POPUP) {
        m->sub[m->n] = (HMENU)id;
        m->ids[m->n] = 0;
    } else {
        m->sub[m->n] = NULL;
        m->ids[m->n] = (UINT)id;
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

/* Win32 item text carries a mnemonic ampersand and a tab-separated
 * accelerator ("&New File\tCtrl+N"); the Mac shows the title alone and puts
 * the shortcut in the item's key equivalent. */
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

/* The accelerator as a key equivalent, or an empty string when there is none
 * to show.
 *
 * Only shortcuts with Ctrl in them become key equivalents, because Ctrl is
 * what maps to Command. A key equivalent without Command would have the menu
 * swallowing plain keystrokes, so F5 and the Alt+arrow bindings are left to
 * app_hotkey -- which handles them anyway, from the view's key events. Chords
 * (Ctrl+K Ctrl+O) have no Mac equivalent at all. */
static void accel_for(const char *in, char *key, int keyn, unsigned int *mask)
{
    const char *tab = strchr(in, '\t');
    char spec[64], *tok, *last = NULL;
    key[0] = 0;
    *mask = 0;
    if (!tab) return;
    strncpy(spec, tab + 1, sizeof spec - 1);
    spec[sizeof spec - 1] = 0;
    if (strchr(spec, ' ')) return;                 /* a chord */
    if (!strstr(spec, "Ctrl")) return;

    for (tok = strtok(spec, "+"); tok; tok = strtok(NULL, "+")) {
        if (strcmp(tok, "Ctrl") == 0)       *mask |= NSCommandKeyMask;
        else if (strcmp(tok, "Shift") == 0) *mask |= NSShiftKeyMask;
        else if (strcmp(tok, "Alt") == 0)   *mask |= NSAlternateKeyMask;
        else last = tok;
    }
    (void)keyn;
    if (!last) return;
    if (strcmp(last, "Tab") == 0) { key[0] = '\t'; key[1] = 0; return; }
    /* Anything longer is a named key -- an arrow or an F-key -- and those are
     * left to app_hotkey, so there is nothing to put in the menu. */
    if (strlen(last) == 1) {
        key[0] = (char)tolower((unsigned char)last[0]);
        key[1] = 0;
    }
}

/* ---- the menu bar ---- */

/* The command the user picked, while a context menu is tracking. */
static int s_popupPick;
static int s_inPopup;

@interface XPMenuTarget : NSObject
@end

@implementation XPMenuTarget
- (void)menuPick:(id)sender
{
    int cmd = (int)[sender tag];
    if (s_inPopup) { s_popupPick = cmd; return; }
    {
        HWND root = shim_root_window();
        if (root) SendMessage(root, WM_COMMAND, (WPARAM)cmd, 0);
    }
}
@end

static XPMenuTarget *s_menuTarget;

/* Turn one shim menu into an NSMenu. */
static NSMenu *build_nsmenu(HMENU m, const char *title)
{
    NSMenu *menu = [[NSMenu alloc] initWithTitle:shim_string(title, -1)];
    int j;
    /* The app decides what is enabled when it builds the menu, so AppKit must
     * not second-guess it by validating against the responder chain. */
    [menu setAutoenablesItems:NO];
    for (j = 0; j < m->n; j++) {
        char itext[64], key[8];
        unsigned int mask = 0;
        NSMenuItem *item;
        if (m->flags[j] & MF_SEPARATOR) {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }
        clean_title(m->text[j], itext, sizeof itext);
        accel_for(m->text[j], key, (int)sizeof key, &mask);
        item = (NSMenuItem *)[menu addItemWithTitle:shim_string(itext, -1)
                                             action:@selector(menuPick:)
                                      keyEquivalent:[NSString stringWithUTF8String:key]];
        [item setKeyEquivalentModifierMask:mask];
        [item setTarget:s_menuTarget];
        [item setTag:(int)m->ids[j]];
        [item setEnabled:(m->flags[j] & MF_GRAYED) ? 0 : 1];
        if (m->flags[j] & MF_CHECKED) [item setState:NSOnState];
    }
    return menu;
}

/* main.c builds a Win32 menu whose top level is all MF_POPUP items and hands
 * it to CreateWindowEx. Each of those submenus becomes a menu in the Mac menu
 * bar, with the application menu -- which Win32 has no equivalent of --
 * synthesised in front of them. */
void menubar_install(HMENU root)
{
    NSMenu *bar;
    int i;
    if (!root || !root->used) return;
    if (!s_menuTarget) s_menuTarget = [[XPMenuTarget alloc] init];

    bar = [[NSMenu alloc] initWithTitle:@""];

    /* The application menu. Win32 has no equivalent, so its two live items
     * are found by name in the menus the app built: About, which the Mac puts
     * here rather than under Help, and Exit, which becomes Quit. */
    {
        int about = 0, quit = 0;
        for (i = 0; i < root->n; i++) {
            HMENU sub = root->sub[i];
            int j;
            if (!(root->flags[i] & MF_POPUP) || !sub || !sub->used) continue;
            for (j = 0; j < sub->n; j++) {
                char t[64];
                clean_title(sub->text[j], t, sizeof t);
                if (strcmp(t, "About") == 0) about = (int)sub->ids[j];
                else if (strcmp(t, "Exit") == 0) quit = (int)sub->ids[j];
            }
        }

        /* AppKit takes the first menu in the bar for this and titles it after
         * the bundle, whatever it is called here. */
        {
            NSMenu *app = [[NSMenu alloc] initWithTitle:@"XPico Code"];
            NSMenuItem *slot = (NSMenuItem *)[bar addItemWithTitle:@"XPico Code"
                                                            action:NULL keyEquivalent:@""];
            NSMenuItem *item;
            [app setAutoenablesItems:NO];

            item = (NSMenuItem *)[app addItemWithTitle:@"About XPico Code"
                                               action:@selector(menuPick:) keyEquivalent:@""];
            [item setTarget:s_menuTarget];
            [item setTag:about];

            [app addItem:[NSMenuItem separatorItem]];
            [app addItemWithTitle:@"Hide XPico Code" action:@selector(hide:) keyEquivalent:@"h"];
            [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:)
                    keyEquivalent:@""];
            [app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:)
                    keyEquivalent:@""];
            [app addItem:[NSMenuItem separatorItem]];

            /* Quit goes through the app's own command so that the session is
             * saved; -terminate: would take the process out from under it. */
            item = (NSMenuItem *)[app addItemWithTitle:@"Quit XPico Code"
                                               action:@selector(menuPick:) keyEquivalent:@"q"];
            [item setTarget:s_menuTarget];
            [item setTag:quit];

            [slot setSubmenu:app];

            /* Tell AppKit this is the application menu.
             *
             * A current system takes the first menu in the bar for that by
             * itself, which is why this is invisible on one. 10.4 does not:
             * it synthesises its own application menu from the bundle name
             * and leaves this one sitting beside it as an ordinary menu, so
             * the bar comes up with "XPico Code" twice.
             *
             * -setAppleMenu: is how a nib-less application said so in 2005.
             * It has never been in a public header, so it is sent by name --
             * which also means a system that has dropped it simply does not
             * answer, and the first-menu behaviour takes over. */
            if ([NSApp respondsToSelector:@selector(setAppleMenu:)])
                [NSApp performSelector:@selector(setAppleMenu:) withObject:app];

            [app release];
        }
    }

    for (i = 0; i < root->n; i++) {
        HMENU sub = root->sub[i];
        char title[64];
        NSMenu *menu;
        NSMenuItem *slot;

        if (!(root->flags[i] & MF_POPUP) || !sub || !sub->used) continue;
        clean_title(root->text[i], title, sizeof title);
        menu = build_nsmenu(sub, title);
        slot = (NSMenuItem *)[bar addItemWithTitle:shim_string(title, -1) action:NULL keyEquivalent:@""];
        [slot setSubmenu:menu];
        [menu release];
    }

    [NSApp setMainMenu:bar];
    [bar release];
}

/* Pop a context menu up at (x, y) in screen coordinates and, with
 * TPM_RETURNCMD, hand back what the user picked.
 *
 * -popUpContextMenu: tracks the menu modally and fires the item's action
 * before it returns, so the pick can simply be recorded in a static. */
BOOL TrackPopupMenu(HMENU m, UINT flags, int x, int y, int reserved, HWND w, LPVOID r)
{
    NSMenu *menu;
    NSWindow *win;
    NSEvent *fake;
    NSPoint where;
    float screenH;
    (void)reserved; (void)r;

    if (!m || !m->used || !shim_root_window()) return FALSE;
    if (!s_menuTarget) s_menuTarget = [[XPMenuTarget alloc] init];

    win = [NSApp mainWindow] ? [NSApp mainWindow] : [[NSApp windows] objectAtIndex:0];
    if (!win) return FALSE;
    screenH = [([win screen] ? [win screen] : [NSScreen mainScreen]) frame].size.height;

    /* screen coordinates, top left origin -> window coordinates, bottom left */
    where.x = (float)x - [win frame].origin.x;
    where.y = (screenH - (float)y) - [win frame].origin.y;

    menu = build_nsmenu(m, "");
    fake = [NSEvent mouseEventWithType:NSLeftMouseDown
                              location:where
                         modifierFlags:0
                             timestamp:0
                          windowNumber:[win windowNumber]
                               context:nil
                           eventNumber:0
                            clickCount:1
                              pressure:1.0f];
    s_popupPick = 0;
    s_inPopup = 1;
    [NSMenu popUpContextMenu:menu withEvent:fake forView:[win contentView]];
    s_inPopup = 0;
    [menu release];

    if (!s_popupPick) return FALSE;
    if (flags & TPM_RETURNCMD) return (BOOL)s_popupPick;
    SendMessage(w, WM_COMMAND, (WPARAM)s_popupPick, 0);
    return TRUE;
}

/* ------------------------------------------------------------ dialogs */

int MessageBox(HWND w, LPCSTR text, LPCSTR caption, UINT type)
{
    int kind = type & 0x0F;
    NSString *msg = shim_string(text, -1);
    NSString *cap = shim_string(caption, -1);
    int hit;
    (void)w;

    if (kind == MB_YESNO || kind == MB_YESNOCANCEL)
        hit = (int)NSRunAlertPanel(cap, @"%@", @"Yes", @"No",
                                   kind == MB_YESNOCANCEL ? @"Cancel" : nil, msg);
    else if (kind == MB_OKCANCEL)
        hit = (int)NSRunAlertPanel(cap, @"%@", @"OK", @"Cancel", nil, msg);
    else
        hit = (int)NSRunAlertPanel(cap, @"%@", @"OK", nil, nil, msg);

    if (kind == MB_YESNO || kind == MB_YESNOCANCEL) {
        if (hit == NSAlertDefaultReturn) return IDYES;
        if (hit == NSAlertAlternateReturn) return IDNO;
        return IDCANCEL;
    }
    return hit == NSAlertDefaultReturn ? IDOK : IDCANCEL;
}

BOOL MacChooseFile(char *path, int n, BOOL save)
{
    int ok;
    if (save) {
        NSSavePanel *panel = [NSSavePanel savePanel];
        ok = (int)[panel runModalForDirectory:nil file:@"Untitled.txt"];
        if (ok != NSOKButton) return FALSE;
        strncpy(path, [[panel filename] fileSystemRepresentation], (size_t)n - 1);
    } else {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        ok = (int)[panel runModalForDirectory:nil file:nil types:nil];
        if (ok != NSOKButton) return FALSE;
        strncpy(path, [[panel filename] fileSystemRepresentation], (size_t)n - 1);
    }
    path[n - 1] = 0;
    return path[0] ? TRUE : FALSE;
}

BOOL MacChooseFolder(char *path, int n)
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setCanChooseFiles:NO];
    [panel setCanChooseDirectories:YES];
    [panel setAllowsMultipleSelection:NO];
    if ((int)[panel runModalForDirectory:nil file:nil types:nil] != NSOKButton) return FALSE;
    strncpy(path, [[panel filename] fileSystemRepresentation], (size_t)n - 1);
    path[n - 1] = 0;
    return path[0] ? TRUE : FALSE;
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
