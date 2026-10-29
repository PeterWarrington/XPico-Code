/* GDI over QuickDraw for the Win32 shim.
 *
 * Every panel in XPico Code paints the same way: get an offscreen DC, fill and
 * draw text into it, BitBlt it to the screen. That maps onto QuickDraw GWorlds
 * cleanly, so the double buffering survives the port unchanged.
 *
 * Child HWNDs share the top-level window's GrafPort, so a window DC carries an
 * origin offset and a clip rect; every primitive adds the offset rather than
 * using SetOrigin, which would otherwise have to be unwound for offscreen DCs. */
#include <Carbon.h>
#include "win32.h"
#include <string.h>
#include <stdlib.h>

#define MAX_DCS   16
#define MAX_FONTS 16
#define MAX_BMPS  16

struct MDC {
    int       used;
    CGrafPtr  port;        /* destination port: window port or GWorld */
    GWorldPtr gw;          /* set when the DC is backed by a bitmap */
    int       ox, oy;      /* origin of this DC inside the port */
    RECT      clip;        /* in port coordinates */
    COLORREF  bk, fg;
    int       bkmode;
    HFONT     font;
    struct MPen   *penObj;
    struct MBrush *brushObj;
    POINT     pen;
    int       savedOx, savedOy;
    RECT      savedClip;
    HDC       fallbackOf;  /* used when an offscreen bitmap cannot be made */
};

struct MFont { int used; short family; short size; Style face; int cw, ch, ascent; };
/* draw_icon_raw() draws every icon with a coloured pen and brush, so these have
 * to be real objects: returning NULL left all shapes in the DC's default black,
 * which is invisible on the dark theme and, in the supersampled icon path,
 * black-on-black -- which is why no icon ever appeared. */
struct MPen   { int used; COLORREF c; int width; };
struct MBrush { int used; COLORREF c; int hollow; };
struct MBitmap { int used; GWorldPtr gw; int w, h; };

#define MAX_PENS   8
#define MAX_BRUSHES 8
static struct MPen    s_pens[MAX_PENS];
static struct MBrush  s_brushes[MAX_BRUSHES];
static struct MBrush  s_nullBrush = { 1, 0, 1 };

static struct MDC     s_dcs[MAX_DCS];
static struct MFont   s_fonts[MAX_FONTS];
static struct MBitmap s_bmps[MAX_BMPS];
static WindowRef      s_rootWR;
static CGrafPtr       s_rootPort;

/* caret: QuickDraw has none, so it is drawn by inverting a rect on a timer */
static HWND  s_caretOwner;
static int   s_caretX, s_caretY, s_caretW, s_caretH, s_caretOn, s_caretShown;
static DWORD s_caretBase;   /* when the current blink cycle started */

int gdi_have_port(void) { return s_rootPort != NULL; }

static void remeasure_fonts(void);

void gdi_init(WindowRef wr)
{
    s_rootWR = wr;
    s_rootPort = GetWindowPort(wr);
    /* fonts built before the window existed carry estimated metrics */
    remeasure_fonts();
}

/* ------------------------------------------------------------ colour helper */

static void to_rgb(COLORREF c, RGBColor *out)
{
    /* COLORREF is 0x00BBGGRR; RGBColor components are 16-bit. */
    out->red   = (unsigned short)(GetRValue(c) * 257);
    out->green = (unsigned short)(GetGValue(c) * 257);
    out->blue  = (unsigned short)(GetBValue(c) * 257);
}

/* The port to measure text against, or NULL before the window exists.
 *
 * This used to fall back to a 1x1 offscreen GWorld so that main.c could build
 * its fonts before the window was created. That was a bad trade: making an
 * offscreen world current switches the GDevice too, and the editor then drew
 * against an offscreen device for the rest of the session. CreateFont now
 * falls back to size-derived metrics instead, and gdi_init re-measures every
 * font once the real window port exists. */
static CGrafPtr measure_port(void) { return s_rootPort; }

static void remeasure_fonts(void)
{
    CGrafPtr savePort;
    GDHandle saveDev;
    int i;
    if (!s_rootPort) return;

    GetGWorld(&savePort, &saveDev);
    SetGWorld(s_rootPort, GetMainDevice());
    for (i = 0; i < MAX_FONTS; i++) {
        FontInfo fi;
        struct MFont *f = &s_fonts[i];
        if (!f->used) continue;
        TextFont(f->family); TextSize(f->size); TextFace(f->face);
        GetFontInfo(&fi);
        if (fi.ascent + fi.descent > 0) {
            f->ch = fi.ascent + fi.descent + fi.leading;
            f->ascent = fi.ascent;
            if (CharWidth('0') > 0) f->cw = CharWidth('0');
        }
    }
    SetGWorld(savePort, saveDev);
}

static void set_fore(COLORREF c) { RGBColor r; to_rgb(c, &r); RGBForeColor(&r); }
static void set_back(COLORREF c) { RGBColor r; to_rgb(c, &r); RGBBackColor(&r); }

/* Make dc current: select the port, reset the pen, and clip to the DC's rect.
 *
 * An offscreen DC must go through SetGWorld, not SetPort: SetPort leaves the
 * current GDevice pointing at the screen, so QuickDraw colour-matches against
 * the wrong device and solid fills come out dithered. Its pixels also have to
 * be locked while anything draws into them. */
static int activate(HDC dc)
{
    Rect cl;
    if (!dc || !dc->used || !dc->port) return 0;

    if (dc->gw) {
        LockPixels(GetGWorldPixMap(dc->gw));
        SetGWorld(dc->gw, NULL);        /* NULL device is correct for a GWorld */
    } else {
        /* A window port needs the screen's GDevice named explicitly. Passing
         * NULL here is only valid for a GWorld; on a window port it left the
         * current device pointing at whatever offscreen world was last drawn
         * into, so everything this process drew afterwards -- including the
         * Dialog Manager's own alerts -- went nowhere. */
        SetGWorld((CGrafPtr)dc->port, GetMainDevice());
    }

    /* A fresh port is solid black/patCopy, but the app reuses DCs, so reset. */
    PenNormal();
    TextMode(srcOr);

    cl.left = (short)dc->clip.left; cl.top = (short)dc->clip.top;
    cl.right = (short)dc->clip.right; cl.bottom = (short)dc->clip.bottom;
    ClipRect(&cl);
    return 1;
}

static HDC alloc_dc(void)
{
    int i;
    for (i = 0; i < MAX_DCS; i++)
        if (!s_dcs[i].used) {
            memset(&s_dcs[i], 0, sizeof s_dcs[i]);
            s_dcs[i].used = 1;
            s_dcs[i].bkmode = OPAQUE;
            s_dcs[i].fg = RGB(0, 0, 0);
            s_dcs[i].bk = RGB(255, 255, 255);
            s_dcs[i].penObj = NULL;
            s_dcs[i].brushObj = NULL;
            return &s_dcs[i];
        }
    return NULL;
}

/* ------------------------------------------------------------ window paint */

HDC gdi_begin_window_paint(HWND w, const RECT *screenRC)
{
    HDC dc;
    (void)w;
    if (!s_rootPort) return NULL;
    dc = alloc_dc();
    if (!dc) return NULL;
    dc->port = s_rootPort;
    dc->ox = screenRC->left; dc->oy = screenRC->top;
    dc->clip = *screenRC;
    return dc;
}

void gdi_end_window_paint(HDC dc)
{
    if (dc && dc->used) dc->used = 0;
}

/* BeginPaint/EndPaint are what the window procs call; the DC for the current
 * WM_PAINT was already built by gdi_begin_window_paint, so hand that one back. */
static HDC s_paintDC;

void gdi_set_paint_dc(HDC dc) { s_paintDC = dc; }

HDC BeginPaint(HWND w, PAINTSTRUCT *ps)
{
    HDC dc = s_paintDC;
    ps->hdc = dc;
    ps->fErase = FALSE;
    GetClientRect(w, &ps->rcPaint);
    return dc;
}

void EndPaint(HWND w, const PAINTSTRUCT *ps) { (void)w; (void)ps; }

HDC GetDC(HWND w)
{
    HDC dc = alloc_dc();
    if (!dc) return NULL;
    /* may be NULL this early; the metrics calls cope, and activate() rejects it */
    dc->port = measure_port();
    dc->ox = 0; dc->oy = 0;
    if (w) GetClientRect(w, &dc->clip);
    /* GetDC(NULL) is only ever used to measure text, so a nominal clip is
     * enough; it must be non-empty or activate() would reject the DC. */
    if (!w || IsRectEmpty(&dc->clip)) SetRect(&dc->clip, 0, 0, 1, 1);
    return dc;
}

void ReleaseDC(HWND w, HDC dc) { (void)w; if (dc && dc->used) dc->used = 0; }

/* ------------------------------------------------------- offscreen DCs */

/* The DC starts unbacked and is attached by SelectObject(bitmap). If the
 * bitmap cannot be allocated, SelectObject leaves this aliasing the source DC
 * so drawing still reaches the screen -- without double buffering, but visible.
 * Losing the whole frame is far worse than a little flicker. */
HDC CreateCompatibleDC(HDC src)
{
    HDC dc = alloc_dc();
    if (!dc) return NULL;
    dc->port = NULL;
    dc->fallbackOf = src;
    return dc;
}

void DeleteDC(HDC dc) { if (dc && dc->used) dc->used = 0; }

HBITMAP CreateCompatibleBitmap(HDC dc, int w, int h)
{
    int i;
    (void)dc;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    for (i = 0; i < MAX_BMPS; i++)
        if (!s_bmps[i].used) {
            Rect r;
            GWorldPtr gw = NULL;
            r.left = 0; r.top = 0; r.right = (short)w; r.bottom = (short)h;
            /* Depth 0 means "same as the current device", which keeps CopyBits
             * to the screen a straight copy instead of a colour-match. */
            if (NewGWorld(&gw, 0, &r, NULL, NULL, 0) != noErr) {
                if (NewGWorld(&gw, 0, &r, NULL, NULL, useTempMem) != noErr) return NULL;
            }
            /* Held locked for the lifetime of the bitmap: the app draws into
             * these between BitBlts and QuickDraw needs the base address fixed. */
            LockPixels(GetGWorldPixMap(gw));
            s_bmps[i].used = 1; s_bmps[i].gw = gw; s_bmps[i].w = w; s_bmps[i].h = h;
            return (HBITMAP)&s_bmps[i];
        }
    return NULL;
}

HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj)
{
    int i;
    if (!dc || !dc->used) return NULL;
    if (!obj) {
        /* CreateCompatibleBitmap failed: alias the window DC so the panel
         * still draws, straight to the screen. */
        if (!dc->port && dc->fallbackOf && dc->fallbackOf->used) {
            dc->port = dc->fallbackOf->port;
            dc->gw = dc->fallbackOf->gw;
            dc->ox = dc->fallbackOf->ox;
            dc->oy = dc->fallbackOf->oy;
            dc->clip = dc->fallbackOf->clip;
        }
        return NULL;
    }

    for (i = 0; i < MAX_FONTS; i++)
        if (obj == (HGDIOBJ)&s_fonts[i] && s_fonts[i].used) {
            HFONT old = dc->font;
            dc->font = (HFONT)obj;
            return (HGDIOBJ)old;
        }
    for (i = 0; i < MAX_PENS; i++)
        if (obj == (HGDIOBJ)&s_pens[i] && s_pens[i].used) {
            struct MPen *old = dc->penObj;
            dc->penObj = &s_pens[i];
            return (HGDIOBJ)old;
        }
    if (obj == (HGDIOBJ)&s_nullBrush) {
        struct MBrush *old = dc->brushObj;
        dc->brushObj = &s_nullBrush;
        return (HGDIOBJ)old;
    }
    for (i = 0; i < MAX_BRUSHES; i++)
        if (obj == (HGDIOBJ)&s_brushes[i] && s_brushes[i].used) {
            struct MBrush *old = dc->brushObj;
            dc->brushObj = &s_brushes[i];
            return (HGDIOBJ)old;
        }
    for (i = 0; i < MAX_BMPS; i++)
        if (obj == (HGDIOBJ)&s_bmps[i] && s_bmps[i].used) {
            dc->gw = s_bmps[i].gw;
            dc->port = (CGrafPtr)s_bmps[i].gw;
            dc->ox = dc->oy = 0;
            SetRect(&dc->clip, 0, 0, s_bmps[i].w, s_bmps[i].h);
            return NULL;
        }
    return NULL;   /* pens and brushes: the source only uses them via helpers */
}

BOOL DeleteObject(HGDIOBJ obj)
{
    int i;
    if (!obj) return FALSE;
    for (i = 0; i < MAX_BMPS; i++)
        if (obj == (HGDIOBJ)&s_bmps[i] && s_bmps[i].used) {
            int j;
            /* any DC still pointing at this GWorld must let go of it first */
            for (j = 0; j < MAX_DCS; j++)
                if (s_dcs[j].used && s_dcs[j].gw == s_bmps[i].gw) {
                    s_dcs[j].gw = NULL;
                    s_dcs[j].port = NULL;
                }
            UnlockPixels(GetGWorldPixMap(s_bmps[i].gw));
            DisposeGWorld(s_bmps[i].gw);
            s_bmps[i].used = 0;
            return TRUE;
        }
    for (i = 0; i < MAX_FONTS; i++)
        if (obj == (HGDIOBJ)&s_fonts[i] && s_fonts[i].used) { s_fonts[i].used = 0; return TRUE; }
    for (i = 0; i < MAX_PENS; i++)
        if (obj == (HGDIOBJ)&s_pens[i] && s_pens[i].used) {
            int j;
            for (j = 0; j < MAX_DCS; j++)
                if (s_dcs[j].used && s_dcs[j].penObj == &s_pens[i]) s_dcs[j].penObj = NULL;
            s_pens[i].used = 0;
            return TRUE;
        }
    for (i = 0; i < MAX_BRUSHES; i++)
        if (obj == (HGDIOBJ)&s_brushes[i] && s_brushes[i].used) {
            int j;
            for (j = 0; j < MAX_DCS; j++)
                if (s_dcs[j].used && s_dcs[j].brushObj == &s_brushes[i]) s_dcs[j].brushObj = NULL;
            s_brushes[i].used = 0;
            return TRUE;
        }
    return TRUE;
}

BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    Rect sr, dr;
    const BitMap *sb, *db;
    (void)rop;
    if (!dst || !src || !dst->port || !src->port) return FALSE;
    /* src never got its own bitmap and is aliasing dst: already on screen */
    if (src->port == dst->port && !src->gw) return TRUE;

    sr.left = (short)(src->ox + sx); sr.top = (short)(src->oy + sy);
    sr.right = (short)(sr.left + w); sr.bottom = (short)(sr.top + h);
    dr.left = (short)(dst->ox + x); dr.top = (short)(dst->oy + y);
    dr.right = (short)(dr.left + w); dr.bottom = (short)(dr.top + h);

    if (!activate(dst)) return FALSE;
    set_fore(RGB(0, 0, 0));
    set_back(RGB(255, 255, 255));

    /* Offscreen bitmaps are locked for their whole lifetime, so there is no
     * lock/unlock pair here: unlocking would pull the base address out from
     * under the next drawing call. */
    sb = GetPortBitMapForCopyBits(src->port);
    db = GetPortBitMapForCopyBits(dst->port);
    CopyBits(sb, db, &sr, &dr, srcCopy, NULL);
    return TRUE;
}

/* ------------------------------------------------------------ attributes */

void SetBkColor(HDC dc, COLORREF c)   { if (dc) dc->bk = c; }
void SetTextColor(HDC dc, COLORREF c) { if (dc) dc->fg = c; }
void SetBkMode(HDC dc, int mode)      { if (dc) dc->bkmode = mode; }

int SaveDC(HDC dc)
{
    if (!dc) return 0;
    dc->savedOx = dc->ox; dc->savedOy = dc->oy; dc->savedClip = dc->clip;
    return 1;
}

BOOL RestoreDC(HDC dc, int state)
{
    (void)state;
    if (!dc) return FALSE;
    dc->ox = dc->savedOx; dc->oy = dc->savedOy; dc->clip = dc->savedClip;
    return TRUE;
}

int IntersectClipRect(HDC dc, int l, int t, int r, int b)
{
    if (!dc) return 0;
    l += dc->ox; t += dc->oy; r += dc->ox; b += dc->oy;
    if (l > dc->clip.left) dc->clip.left = l;
    if (t > dc->clip.top) dc->clip.top = t;
    if (r < dc->clip.right) dc->clip.right = r;
    if (b < dc->clip.bottom) dc->clip.bottom = b;
    return 1;
}

HGDIOBJ gdi_stock_object(int obj)
{
    if (obj == NULL_BRUSH) return (HGDIOBJ)&s_nullBrush;
    return NULL;
}

static COLORREF pen_colour(HDC dc) { return dc->penObj ? dc->penObj->c : dc->fg; }
static int pen_width(HDC dc) { return dc->penObj ? dc->penObj->width : 1; }
static void apply_pen(HDC dc)
{
    int w = pen_width(dc);
    PenSize((short)w, (short)w);
    set_fore(pen_colour(dc));
}
/* non-zero when the shape should be filled, with the colour in *out */
static int brush_fill(HDC dc, COLORREF *out)
{
    if (!dc->brushObj || dc->brushObj->hollow) return 0;
    *out = dc->brushObj->c;
    return 1;
}

/* ------------------------------------------------------------ fonts */

HFONT CreateFont(int h, int w, int esc, int orient, int weight, DWORD italic,
                 DWORD underline, DWORD strikeout, DWORD charset, DWORD outprec,
                 DWORD clipprec, DWORD quality, DWORD pitch, LPCSTR face)
{
    int i;
    (void)w; (void)esc; (void)orient; (void)charset;
    (void)outprec; (void)clipprec; (void)quality; (void)strikeout;

    for (i = 0; i < MAX_FONTS; i++)
        if (!s_fonts[i].used) {
            struct MFont *f = &s_fonts[i];
            Str255 pname;
            short fam = 0;
            FontInfo fi;

            f->used = 1;
            f->size = (short)(h < 0 ? -h : h);
            if (f->size <= 0) f->size = 12;
            f->face = 0;
            if (weight >= 700) f->face |= bold;
            if (italic) f->face |= italic;
            if (underline) f->face |= underline;

            /* Map the requested face onto something that exists on Mac OS 9:
             * Monaco for monospaced text, Geneva otherwise.
             *
             * FIXED_PITCH decides it, not the face name. The editor asks for
             * "Lucida Console", which matches none of the obvious monospace
             * names, so a name-based guess silently handed the editor a
             * proportional font -- text was proportionally spaced while the
             * caret and column arithmetic still assumed a fixed cell, so the
             * cursor drifted further right the longer the line. */
            {
                const char *mac = "Geneva";
                if ((pitch & 0x03) == FIXED_PITCH) {
                    mac = "Monaco";
                } else if (face) {
                    if (strstr(face, "Consolas") || strstr(face, "Courier") ||
                        strstr(face, "Console") || strstr(face, "Mono") ||
                        strstr(face, "Fixed") || strstr(face, "Terminal"))
                        mac = "Monaco";
                }
                pname[0] = (unsigned char)strlen(mac);
                memcpy(pname + 1, mac, pname[0]);
                GetFNum(pname, &fam);
            }
            f->family = fam;

            /* Measure against a real port, then put back exactly what was
             * current. GetGWorld/SetGWorld, not GetPort/SetPort: the measuring
             * port may be a GWorld, and SetPort would leave the screen's
             * GDevice current while drawing into it. Restoring the wrong port
             * here left every later draw going to the 1x1 scratch world. */
            {
                CGrafPtr savePort;
                GDHandle saveDev;
                CGrafPtr mp = measure_port();
                GetGWorld(&savePort, &saveDev);
                if (mp) SetGWorld(mp, mp == s_rootPort ? saveDev : NULL);
                TextFont(f->family); TextSize(f->size); TextFace(f->face);
                GetFontInfo(&fi);
                f->ch = fi.ascent + fi.descent + fi.leading;
                f->ascent = fi.ascent;
                f->cw = CharWidth('0');
                SetGWorld(savePort, saveDev);
            }

            /* Never leave a font with a zero cell. main.c derives the editor's
             * whole glyph grid from these, and a zero width means it can lay
             * nothing out and the window stays blank. If the measurement did
             * not take -- no usable port this early in startup -- fall back to
             * proportions that are about right for Monaco and Geneva. */
            if (f->ch <= 0)     f->ch = f->size + f->size / 4 + 1;
            if (f->ascent <= 0) f->ascent = f->size;
            if (f->cw <= 0)     f->cw = (f->size * 3 + 4) / 5;

            return (HFONT)f;
        }
    return NULL;
}

static void apply_font(HDC dc)
{
    if (dc->font) {
        TextFont(dc->font->family);
        TextSize(dc->font->size);
        TextFace(dc->font->face);
    }
}

/* Answered from the font's own cached metrics, which CreateFont guarantees are
 * non-zero. main.c calls this through GetDC(NULL) before the window exists and
 * uses the result without checking it, so it must always return something the
 * editor can lay out with. */
BOOL GetTextMetrics(HDC dc, TEXTMETRIC *tm)
{
    FontInfo fi;
    memset(tm, 0, sizeof *tm);
    if (!dc || !dc->font) return FALSE;

    tm->tmAscent = dc->font->ascent;
    tm->tmHeight = dc->font->ch;
    tm->tmDescent = dc->font->ch - dc->font->ascent;
    tm->tmAveCharWidth = dc->font->cw;
    tm->tmMaxCharWidth = dc->font->cw;

    /* refine from the live port when there is one */
    if (activate(dc)) {
        apply_font(dc);
        GetFontInfo(&fi);
        if (fi.ascent + fi.descent > 0) {
            tm->tmAscent = fi.ascent;
            tm->tmDescent = fi.descent;
            tm->tmHeight = fi.ascent + fi.descent + fi.leading;
            if (CharWidth('0') > 0) tm->tmAveCharWidth = CharWidth('0');
            if (fi.widMax > 0) tm->tmMaxCharWidth = fi.widMax;
        }
    }
    return TRUE;
}

BOOL GetTextExtentPoint32(HDC dc, LPCSTR s, int n, LPSIZE sz)
{
    FontInfo fi;
    sz->cx = sz->cy = 0;
    if (!dc) return FALSE;
    if (n < 0) n = (int)strlen(s);

    /* an estimate that works with no port, refined below when there is one */
    if (dc->font) { sz->cx = n * dc->font->cw; sz->cy = dc->font->ch; }

    if (!activate(dc)) return TRUE;
    apply_font(dc);
    GetFontInfo(&fi);
    sz->cx = TextWidth(s, 0, n);
    sz->cy = fi.ascent + fi.descent + fi.leading;
    return TRUE;
}

/* ------------------------------------------------------------ text & fills */

BOOL ExtTextOut(HDC dc, int x, int y, UINT flags, const RECT *r, LPCSTR s, UINT n, const int *dx)
{
    (void)dx;
    if (!activate(dc)) return FALSE;

    /* fill() and fill_rc() in main.c paint solid rectangles through this call
     * with ETO_OPAQUE and no text, which is the shim's most common operation. */
    if ((flags & ETO_OPAQUE) && r) {
        Rect mr;
        mr.left = (short)(dc->ox + r->left); mr.top = (short)(dc->oy + r->top);
        mr.right = (short)(dc->ox + r->right); mr.bottom = (short)(dc->oy + r->bottom);
        set_fore(dc->bk);
        PaintRect(&mr);
    }
    if (s && n) {
        apply_font(dc);
        if (dc->bkmode == OPAQUE) { set_back(dc->bk); TextMode(srcCopy); }
        else TextMode(srcOr);
        set_fore(dc->fg);
        MoveTo((short)(dc->ox + x), (short)(dc->oy + y + (dc->font ? dc->font->ascent : 10)));
        DrawText(s, 0, (short)n);
    }
    return TRUE;
}

BOOL TextOut(HDC dc, int x, int y, LPCSTR s, int n)
{
    if (!activate(dc)) return FALSE;
    apply_font(dc);
    if (dc->bkmode == OPAQUE) { set_back(dc->bk); TextMode(srcCopy); }
    else TextMode(srcOr);
    set_fore(dc->fg);
    MoveTo((short)(dc->ox + x), (short)(dc->oy + y + (dc->font ? dc->font->ascent : 10)));
    DrawText(s, 0, (short)n);
    return TRUE;
}

/* ------------------------------------------------------------ shapes */

void MoveToEx(HDC dc, int x, int y, LPPOINT old)
{
    if (!dc) return;
    if (old) *old = dc->pen;
    dc->pen.x = x; dc->pen.y = y;
}

void LineTo(HDC dc, int x, int y)
{
    if (!activate(dc)) return;
    apply_pen(dc);
    MoveTo((short)(dc->ox + dc->pen.x), (short)(dc->oy + dc->pen.y));
    /* Carbon's LineTo is shadowed by the shim's; MoveTo plus Line is equivalent. */
    Line((short)(x - dc->pen.x), (short)(y - dc->pen.y));
    dc->pen.x = x; dc->pen.y = y;
}

HPEN CreatePen(int style, int width, COLORREF c)
{
    int i;
    (void)style;
    for (i = 0; i < MAX_PENS; i++)
        if (!s_pens[i].used) {
            s_pens[i].used = 1;
            s_pens[i].c = c;
            s_pens[i].width = width > 0 ? width : 1;
            return (HPEN)&s_pens[i];
        }
    return NULL;
}

HBRUSH CreateSolidBrush(COLORREF c)
{
    int i;
    for (i = 0; i < MAX_BRUSHES; i++)
        if (!s_brushes[i].used) {
            s_brushes[i].used = 1;
            s_brushes[i].c = c;
            s_brushes[i].hollow = 0;
            return (HBRUSH)&s_brushes[i];
        }
    return NULL;
}

/* the colour and width a stroke should use */

void Rectangle(HDC dc, int l, int t, int r, int b)
{
    Rect mr;
    COLORREF fillc;
    if (!activate(dc)) return;
    mr.left = (short)(dc->ox + l); mr.top = (short)(dc->oy + t);
    mr.right = (short)(dc->ox + r); mr.bottom = (short)(dc->oy + b);
    if (brush_fill(dc, &fillc)) { set_fore(fillc); PaintRect(&mr); }
    apply_pen(dc);
    FrameRect(&mr);
}

void Ellipse(HDC dc, int l, int t, int r, int b)
{
    Rect mr;
    COLORREF fillc;
    if (!activate(dc)) return;
    mr.left = (short)(dc->ox + l); mr.top = (short)(dc->oy + t);
    mr.right = (short)(dc->ox + r); mr.bottom = (short)(dc->oy + b);
    if (brush_fill(dc, &fillc)) { set_fore(fillc); PaintOval(&mr); }
    apply_pen(dc);
    FrameOval(&mr);
}

void Polygon(HDC dc, const POINT *pts, int n)
{
    PolyHandle p;
    COLORREF fillc;
    int i;
    if (!activate(dc) || n < 2) return;
    p = OpenPoly();
    MoveTo((short)(dc->ox + pts[0].x), (short)(dc->oy + pts[0].y));
    for (i = 1; i < n; i++)
        Line((short)(pts[i].x - pts[i - 1].x), (short)(pts[i].y - pts[i - 1].y));
    Line((short)(pts[0].x - pts[n - 1].x), (short)(pts[0].y - pts[n - 1].y));
    ClosePoly();
    if (brush_fill(dc, &fillc)) { set_fore(fillc); PaintPoly(p); }
    apply_pen(dc);
    FramePoly(p);
    KillPoly(p);
}

void Arc(HDC dc, int l, int t, int r, int b, int xs, int ys, int xe, int ye)
{
    Rect mr;
    (void)xs; (void)ys; (void)xe; (void)ye;
    if (!activate(dc)) return;
    mr.left = (short)(dc->ox + l); mr.top = (short)(dc->oy + t);
    mr.right = (short)(dc->ox + r); mr.bottom = (short)(dc->oy + b);
    apply_pen(dc);
    FrameArc(&mr, 0, 360);
}

/* --------------------------------------------------------- DIB access */

/* main.c draws every icon by supersampling it into an offscreen bitmap, reading
 * the pixels back, antialiasing against the destination in software, and
 * writing the result out again. That needs real GetDIBits/SetDIBitsToDevice,
 * so these walk the GWorld's PixMap directly.
 *
 * The app asks for 32-bit top-down DIBs (biBitCount 32, negative biHeight), so
 * each pixel is a DWORD laid out 0x00RRGGBB. */

static struct MBitmap *find_bmp(HBITMAP b)
{
    int i;
    for (i = 0; i < MAX_BMPS; i++)
        if ((HGDIOBJ)&s_bmps[i] == (HGDIOBJ)b && s_bmps[i].used) return &s_bmps[i];
    return NULL;
}

/* read one pixel from a locked PixMap as 0x00RRGGBB */
static DWORD pix_get(PixMapHandle pm, Ptr base, int rowBytes, int depth, int x, int y)
{
    switch (depth) {
    case 32: {
        unsigned char *p = (unsigned char *)(base + y * rowBytes + x * 4);
        /* stored as ARGB on big-endian PowerPC */
        return ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | (DWORD)p[3];
    }
    case 16: {
        unsigned short v = *(unsigned short *)(base + y * rowBytes + x * 2);
        int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
        return ((DWORD)(r * 255 / 31) << 16) | ((DWORD)(g * 255 / 31) << 8) | (DWORD)(b * 255 / 31);
    }
    default: {
        /* indexed modes: ask QuickDraw rather than decode the colour table */
        RGBColor c;
        (void)pm; (void)base; (void)rowBytes;
        GetCPixel((short)x, (short)y, &c);
        return ((DWORD)(c.red >> 8) << 16) | ((DWORD)(c.green >> 8) << 8) | (DWORD)(c.blue >> 8);
    }
    }
}

int GetDIBits(HDC dc, HBITMAP b, UINT start, UINT lines, LPVOID bits, LPVOID info, UINT usage)
{
    struct MBitmap *bm = find_bmp(b);
    BITMAPINFO *bi = (BITMAPINFO *)info;
    DWORD *out = (DWORD *)bits;
    PixMapHandle pm;
    Ptr base;
    int rowBytes, depth, w, h, y, x;
    CGrafPtr savePort;
    GDHandle saveDev;
    (void)dc; (void)usage;

    if (!bm || !out || !bi) return 0;
    w = (int)bi->bmiHeader.biWidth;
    if (w <= 0 || w > bm->w) w = bm->w;
    h = (int)lines;
    if (h <= 0 || (int)start + h > bm->h) h = bm->h - (int)start;
    if (h <= 0) return 0;

    pm = GetGWorldPixMap(bm->gw);
    LockPixels(pm);
    base = GetPixBaseAddr(pm);
    rowBytes = (*pm)->rowBytes & 0x3FFF;
    depth = (*pm)->pixelSize;

    GetGWorld(&savePort, &saveDev);
    SetGWorld(bm->gw, NULL);              /* for the GetCPixel fallback */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            out[y * w + x] = pix_get(pm, base, rowBytes, depth, x, (int)start + y);
    SetGWorld(savePort, saveDev);

    return h;
}

int SetDIBitsToDevice(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                      UINT start, UINT lines, LPVOID bits, LPVOID info, UINT usage)
{
    const DWORD *in = (const DWORD *)bits;
    GWorldPtr tmp = NULL;
    PixMapHandle pm;
    Ptr base;
    Rect r;
    int rowBytes, depth, iy, ix;
    CGrafPtr savePort;
    GDHandle saveDev;
    (void)sx; (void)sy; (void)start; (void)lines; (void)info; (void)usage;

    if (!dc || !in || w == 0 || h == 0) return 0;

    r.left = 0; r.top = 0; r.right = (short)w; r.bottom = (short)h;
    if (NewGWorld(&tmp, 32, &r, NULL, NULL, 0) != noErr) {
        if (NewGWorld(&tmp, 0, &r, NULL, NULL, 0) != noErr) return 0;
    }

    pm = GetGWorldPixMap(tmp);
    LockPixels(pm);
    base = GetPixBaseAddr(pm);
    rowBytes = (*pm)->rowBytes & 0x3FFF;
    depth = (*pm)->pixelSize;

    GetGWorld(&savePort, &saveDev);
    SetGWorld(tmp, NULL);
    for (iy = 0; iy < (int)h; iy++) {
        for (ix = 0; ix < (int)w; ix++) {
            DWORD v = in[iy * (int)w + ix];
            if (depth == 32) {
                unsigned char *p = (unsigned char *)(base + iy * rowBytes + ix * 4);
                p[0] = 0;
                p[1] = (unsigned char)((v >> 16) & 0xFF);
                p[2] = (unsigned char)((v >> 8) & 0xFF);
                p[3] = (unsigned char)(v & 0xFF);
            } else {
                RGBColor c;
                c.red   = (unsigned short)(((v >> 16) & 0xFF) * 257);
                c.green = (unsigned short)(((v >> 8) & 0xFF) * 257);
                c.blue  = (unsigned short)((v & 0xFF) * 257);
                SetCPixel((short)ix, (short)iy, &c);
            }
        }
    }
    SetGWorld(savePort, saveDev);

    /* blit it where the caller asked for it */
    if (activate(dc)) {
        Rect dr;
        dr.left = (short)(dc->ox + x); dr.top = (short)(dc->oy + y);
        dr.right = (short)(dr.left + w); dr.bottom = (short)(dr.top + h);
        set_fore(RGB(0, 0, 0));
        set_back(RGB(255, 255, 255));
        CopyBits(GetPortBitMapForCopyBits((CGrafPtr)tmp),
                 GetPortBitMapForCopyBits(dc->port), &r, &dr, srcCopy, NULL);
    }

    UnlockPixels(pm);
    DisposeGWorld(tmp);
    return (int)h;
}

/* PatBlt is used with BLACKNESS to clear the supersampling buffer, so the
 * pattern operations have to mean what they say rather than paint bk. */
BOOL PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    Rect r;
    if (!activate(dc)) return FALSE;
    r.left = (short)(dc->ox + x); r.top = (short)(dc->oy + y);
    r.right = (short)(r.left + w); r.bottom = (short)(r.top + h);
    if (rop == BLACKNESS)      set_fore(RGB(0, 0, 0));
    else if (rop == WHITENESS) set_fore(RGB(255, 255, 255));
    else                       set_fore(dc->bk);
    PaintRect(&r);
    return TRUE;
}

/* ------------------------------------------------------------ caret */

static void invalidate_caret(void);

BOOL CreateCaret(HWND w, HBITMAP b, int cw, int ch)
{
    (void)b;
    s_caretOwner = w; s_caretW = cw ? cw : 2; s_caretH = ch;
    s_caretOn = 0; s_caretShown = 0;
    return TRUE;
}

BOOL ShowCaret(HWND w)
{
    (void)w;
    s_caretShown = 1;
    s_caretBase = TickCount();
    s_caretOn = 1;
    invalidate_caret();
    return TRUE;
}

BOOL DestroyCaret(void)
{
    if (s_caretShown && s_caretOn) invalidate_caret();
    s_caretOwner = NULL; s_caretShown = 0; s_caretOn = 0;
    return TRUE;
}

BOOL SetCaretPos(int x, int y)
{
    if (x == s_caretX && y == s_caretY) return TRUE;
    /* erase it where it was, then restart the blink so it is visible at once
     * in its new place rather than waiting out the current half-cycle */
    if (s_caretShown && s_caretOn) invalidate_caret();
    s_caretX = x; s_caretY = y;
    s_caretBase = TickCount();
    s_caretOn = 1;
    if (s_caretShown) invalidate_caret();
    return TRUE;
}

void gdi_caret_owner_gone(HWND w) { if (s_caretOwner == w) DestroyCaret(); }

/* Invalidate just the caret, not the whole panel: this runs twice a second and
 * the editor redraws through a full-size offscreen buffer. */
static void invalidate_caret(void)
{
    RECT r;
    if (!s_caretOwner) return;
    SetRect(&r, s_caretX - 1, s_caretY - 1,
            s_caretX + s_caretW + 1, s_caretY + (s_caretH > 0 ? s_caretH : 12) + 1);
    InvalidateRect(s_caretOwner, &r, FALSE);
}

/* The visible phase, derived from the clock rather than kept as a flag that
 * gets toggled. The editor calls SetCaretPos on every redraw, and anything that
 * reset a toggle from there would pin the caret on and stop it blinking. */
#define CARET_BLINK_TICKS 30        /* half a second at 60 ticks/sec */

static int caret_phase(void)
{
    return ((TickCount() - s_caretBase) / CARET_BLINK_TICKS) % 2 == 0;
}

void gdi_caret_tick(void)
{
    int on;
    if (!s_caretShown || !s_caretOwner || !s_rootPort) return;
    on = caret_phase();
    if (on == s_caretOn) return;    /* nothing to redraw yet */
    s_caretOn = on;
    invalidate_caret();
}

/* Draw the caret over a window that has just painted.
 *
 * Win32 has the system draw the caret on the application's behalf, so the
 * editor never draws one itself -- it only calls CreateCaret/SetCaretPos and
 * expects a blinking bar to appear. Nothing in the shim was doing that, so
 * there was no text cursor at all. mac-carbon/wnd.c calls this after each window's
 * WM_PAINT, which puts it on top of the panel's own blitted buffer. */
void gdi_paint_caret(HWND w, HDC dc)
{
    Rect r;
    if (!s_caretShown || !s_caretOn || !s_caretOwner || s_caretOwner != w) return;
    if (!activate(dc)) return;
    r.left = (short)(dc->ox + s_caretX);
    r.top = (short)(dc->oy + s_caretY);
    r.right = (short)(r.left + (s_caretW > 0 ? s_caretW : 2));
    r.bottom = (short)(r.top + (s_caretH > 0 ? s_caretH : 12));
    /* inverting shows up against any background the editor painted */
    InvertRect(&r);
}
