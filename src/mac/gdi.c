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
    POINT     pen;
    int       savedOx, savedOy;
    RECT      savedClip;
};

struct MFont { int used; short family; short size; Style face; int cw, ch, ascent; };
struct MBitmap { int used; GWorldPtr gw; int w, h; };

static struct MDC     s_dcs[MAX_DCS];
static struct MFont   s_fonts[MAX_FONTS];
static struct MBitmap s_bmps[MAX_BMPS];
static WindowRef      s_rootWR;
static CGrafPtr       s_rootPort;

/* caret: QuickDraw has none, so it is drawn by inverting a rect on a timer */
static HWND  s_caretOwner;
static int   s_caretX, s_caretY, s_caretW, s_caretH, s_caretOn, s_caretShown;
static DWORD s_caretNext;

void gdi_init(WindowRef wr)
{
    s_rootWR = wr;
    s_rootPort = GetWindowPort(wr);
}

/* ------------------------------------------------------------ colour helper */

static void to_rgb(COLORREF c, RGBColor *out)
{
    /* COLORREF is 0x00BBGGRR; RGBColor components are 16-bit. */
    out->red   = (unsigned short)(GetRValue(c) * 257);
    out->green = (unsigned short)(GetGValue(c) * 257);
    out->blue  = (unsigned short)(GetBValue(c) * 257);
}

static void set_fore(COLORREF c) { RGBColor r; to_rgb(c, &r); RGBForeColor(&r); }
static void set_back(COLORREF c) { RGBColor r; to_rgb(c, &r); RGBBackColor(&r); }

/* Make dc current: set the port, and clip to the DC's rect. */
static int activate(HDC dc)
{
    Rect cl;
    if (!dc || !dc->used || !dc->port) return 0;
    SetPort((GrafPtr)dc->port);
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
    HDC dc;
    RECT r;
    if (!s_rootPort) return NULL;
    dc = alloc_dc();
    if (!dc) return NULL;
    dc->port = s_rootPort;
    GetWindowRect(w, &r);
    dc->ox = 0; dc->oy = 0;
    GetClientRect(w, &dc->clip);
    return dc;
}

void ReleaseDC(HWND w, HDC dc) { (void)w; if (dc && dc->used) dc->used = 0; }

/* ------------------------------------------------------- offscreen DCs */

HDC CreateCompatibleDC(HDC src)
{
    HDC dc = alloc_dc();
    (void)src;
    if (!dc) return NULL;
    dc->port = NULL;          /* attached by SelectObject(bitmap) */
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
            if (NewGWorld(&gw, 32, &r, NULL, NULL, 0) != noErr) {
                if (NewGWorld(&gw, 16, &r, NULL, NULL, 0) != noErr) return NULL;
            }
            s_bmps[i].used = 1; s_bmps[i].gw = gw; s_bmps[i].w = w; s_bmps[i].h = h;
            return (HBITMAP)&s_bmps[i];
        }
    return NULL;
}

HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj)
{
    int i;
    if (!dc || !dc->used || !obj) return NULL;

    for (i = 0; i < MAX_FONTS; i++)
        if (obj == (HGDIOBJ)&s_fonts[i] && s_fonts[i].used) {
            HFONT old = dc->font;
            dc->font = (HFONT)obj;
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
            DisposeGWorld(s_bmps[i].gw);
            s_bmps[i].used = 0;
            return TRUE;
        }
    for (i = 0; i < MAX_FONTS; i++)
        if (obj == (HGDIOBJ)&s_fonts[i] && s_fonts[i].used) { s_fonts[i].used = 0; return TRUE; }
    return TRUE;
}

BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    Rect sr, dr;
    const BitMap *sb, *db;
    (void)rop;
    if (!dst || !src || !dst->port || !src->port) return FALSE;

    sr.left = (short)(src->ox + sx); sr.top = (short)(src->oy + sy);
    sr.right = (short)(sr.left + w); sr.bottom = (short)(sr.top + h);
    dr.left = (short)(dst->ox + x); dr.top = (short)(dst->oy + y);
    dr.right = (short)(dr.left + w); dr.bottom = (short)(dr.top + h);

    if (!activate(dst)) return FALSE;
    set_fore(RGB(0, 0, 0));
    set_back(RGB(255, 255, 255));

    sb = GetPortBitMapForCopyBits(src->port);
    db = GetPortBitMapForCopyBits(dst->port);
    if (src->gw) LockPixels(GetGWorldPixMap(src->gw));
    CopyBits(sb, db, &sr, &dr, srcCopy, NULL);
    if (src->gw) UnlockPixels(GetGWorldPixMap(src->gw));
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
            GrafPtr save;

            f->used = 1;
            f->size = (short)(h < 0 ? -h : h);
            if (f->size <= 0) f->size = 12;
            f->face = 0;
            if (weight >= 700) f->face |= bold;
            if (italic) f->face |= italic;
            if (underline) f->face |= underline;

            /* Map the requested face onto something that exists on Mac OS 9.
             * The source asks for Consolas/Courier New for code and Tahoma or
             * MS Sans Serif for UI; Monaco and Geneva are the local stand-ins. */
            if (face && (pitch & 0x01)) fam = 0;
            {
                const char *mac = "Geneva";
                if (face) {
                    if (strstr(face, "Consolas") || strstr(face, "Courier") ||
                        strstr(face, "Mono") || strstr(face, "Fixed"))
                        mac = "Monaco";
                }
                pname[0] = (unsigned char)strlen(mac);
                memcpy(pname + 1, mac, pname[0]);
                GetFNum(pname, &fam);
            }
            f->family = fam;

            GetPort(&save);
            if (s_rootPort) SetPort((GrafPtr)s_rootPort);
            TextFont(f->family); TextSize(f->size); TextFace(f->face);
            GetFontInfo(&fi);
            f->ch = fi.ascent + fi.descent + fi.leading;
            f->ascent = fi.ascent;
            f->cw = CharWidth('0');
            SetPort(save);

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

BOOL GetTextMetrics(HDC dc, TEXTMETRIC *tm)
{
    FontInfo fi;
    if (!activate(dc)) return FALSE;
    apply_font(dc);
    GetFontInfo(&fi);
    tm->tmAscent = fi.ascent;
    tm->tmDescent = fi.descent;
    tm->tmHeight = fi.ascent + fi.descent + fi.leading;
    tm->tmAveCharWidth = CharWidth('0');
    tm->tmMaxCharWidth = fi.widMax;
    return TRUE;
}

BOOL GetTextExtentPoint32(HDC dc, LPCSTR s, int n, LPSIZE sz)
{
    FontInfo fi;
    sz->cx = sz->cy = 0;
    if (!activate(dc)) return FALSE;
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
    set_fore(dc->fg);
    MoveTo((short)(dc->ox + dc->pen.x), (short)(dc->oy + dc->pen.y));
    /* Carbon's LineTo is shadowed by the shim's; MoveTo plus Line is equivalent. */
    Line((short)(x - dc->pen.x), (short)(y - dc->pen.y));
    dc->pen.x = x; dc->pen.y = y;
}

HPEN   CreatePen(int style, int width, COLORREF c) { (void)style; (void)width; (void)c; return NULL; }
HBRUSH CreateSolidBrush(COLORREF c) { (void)c; return NULL; }

void Rectangle(HDC dc, int l, int t, int r, int b)
{
    Rect mr;
    if (!activate(dc)) return;
    mr.left = (short)(dc->ox + l); mr.top = (short)(dc->oy + t);
    mr.right = (short)(dc->ox + r); mr.bottom = (short)(dc->oy + b);
    set_fore(dc->fg);
    FrameRect(&mr);
}

void Ellipse(HDC dc, int l, int t, int r, int b)
{
    Rect mr;
    if (!activate(dc)) return;
    mr.left = (short)(dc->ox + l); mr.top = (short)(dc->oy + t);
    mr.right = (short)(dc->ox + r); mr.bottom = (short)(dc->oy + b);
    set_fore(dc->fg);
    PaintOval(&mr);
}

void Polygon(HDC dc, const POINT *pts, int n)
{
    PolyHandle p;
    int i;
    if (!activate(dc) || n < 2) return;
    set_fore(dc->fg);
    p = OpenPoly();
    MoveTo((short)(dc->ox + pts[0].x), (short)(dc->oy + pts[0].y));
    for (i = 1; i < n; i++)
        Line((short)(pts[i].x - pts[i - 1].x), (short)(pts[i].y - pts[i - 1].y));
    Line((short)(pts[0].x - pts[n - 1].x), (short)(pts[0].y - pts[n - 1].y));
    ClosePoly();
    PaintPoly(p);
    KillPoly(p);
}

void Arc(HDC dc, int l, int t, int r, int b, int xs, int ys, int xe, int ye)
{
    Rect mr;
    (void)xs; (void)ys; (void)xe; (void)ye;
    if (!activate(dc)) return;
    mr.left = (short)(dc->ox + l); mr.top = (short)(dc->oy + t);
    mr.right = (short)(dc->ox + r); mr.bottom = (short)(dc->oy + b);
    set_fore(dc->fg);
    FrameArc(&mr, 0, 360);
}

/* ------------------------------------------------------------ caret */

BOOL CreateCaret(HWND w, HBITMAP b, int cw, int ch)
{
    (void)b;
    s_caretOwner = w; s_caretW = cw ? cw : 2; s_caretH = ch;
    s_caretOn = 0; s_caretShown = 0;
    return TRUE;
}

BOOL ShowCaret(HWND w) { (void)w; s_caretShown = 1; return TRUE; }

BOOL DestroyCaret(void)
{
    if (s_caretShown && s_caretOn && s_caretOwner) InvalidateRect(s_caretOwner, NULL, FALSE);
    s_caretOwner = NULL; s_caretShown = 0; s_caretOn = 0;
    return TRUE;
}

BOOL SetCaretPos(int x, int y)
{
    s_caretX = x; s_caretY = y;
    return TRUE;
}

void gdi_caret_owner_gone(HWND w) { if (s_caretOwner == w) DestroyCaret(); }

/* Blink by inverting the caret rect twice a second. */
void gdi_caret_tick(void)
{
    if (!s_caretShown || !s_caretOwner || !s_rootPort) return;
    if (TickCount() < s_caretNext) return;
    s_caretNext = TickCount() + 30;
    s_caretOn = !s_caretOn;
    InvalidateRect(s_caretOwner, NULL, FALSE);
}
