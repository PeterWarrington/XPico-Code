/* GDI over Quartz for the Win32 shim.
 *
 * Every panel in XPico Code paints the same way: get an offscreen DC, fill and
 * draw text into it, BitBlt it to the screen. That maps onto CGBitmapContexts
 * cleanly, so the double buffering survives the port unchanged.
 *
 * Two things shape the rest of this file.
 *
 * A CGContext is write-only -- there is no reading pixels back out of a window
 * -- and main.c's antialiased icons do exactly that: they blit the destination
 * into a bitmap, blend the supersampled glyph against it in software and write
 * the result back. So a window DC is not the view's context but a window-sized
 * bitmap of our own, which gdi_end_window_paint copies to the view. Every DC
 * is then readable, and painting is flicker-free into the bargain.
 *
 * Child HWNDs share that one backing store, so a window DC carries an origin
 * offset and a clip rect; every primitive adds the offset rather than moving
 * the CTM, which would then have to be unwound for offscreen DCs. Each context
 * is flipped once at creation (origin top left, y down) so the whole file can
 * be written in the app's own coordinates.
 *
 * Text takes one of two paths. CGContextShowText is much the cheaper, and an
 * editor draws a great deal of text, but it can only encode Mac OS Roman --
 * and the editor's bytes are UTF-8 on this system. So a run that is pure ASCII
 * (which nearly all source code is, and where the two encodings agree) goes
 * through Core Graphics, and anything with a high byte in it is decoded and
 * drawn as an NSString. Both are measured the same way they are drawn, which
 * is what keeps the caret where the glyphs are. */
#import <Cocoa/Cocoa.h>
#include "win32.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* mac-cocoa/sys.m: the editor's bytes are UTF-8 */
NSString *shim_string(const char *s, int n);

#define MAX_DCS   16
#define MAX_FONTS 16
#define MAX_BMPS  16
#define MAX_PENS   8
#define MAX_BRUSHES 8

struct MBitmap {
    int used;
    CGContextRef ctx;
    unsigned char *data;
    int w, h;              /* logical size, in points: what the app drew with */
    int pw, ph;            /* pixel size, which is the logical size times the
                            * display scale -- the two differ only for the
                            * window's backing store on a Retina display */
    int rowBytes;
};

struct MDC {
    int       used;
    CGContextRef ctx;      /* NULL on a DC that is only used to measure text */
    struct MBitmap *bmp;   /* set when the DC is backed by a bitmap */
    int       ox, oy;      /* origin of this DC inside the context */
    RECT      clip;        /* in context coordinates */
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

/* The Core Graphics font name, not the Win32 face name: CreateFont resolves
 * the request through NSFont and keeps what the system actually gave it. */
struct MFont {
    int   used;
    char  cgname[128];     /* for the Core Graphics fast path */
    int   cgok;            /* 0 when that path cannot name this font */
    NSFont *nsfont;        /* for the Unicode path; retained */
    float size;
    int   fixed;
    float adv[256];        /* advance per ASCII byte, in points */
    float adv0;            /* advance of '0', which sets the cell width */
    int   cw, ch, ascent, leading;
};

/* draw_icon_raw() draws every icon with a coloured pen and brush, so these
 * have to be real objects: returning NULL would leave every shape in the DC's
 * default black, which is invisible on the dark theme. */
struct MPen   { int used; COLORREF c; int width; };
struct MBrush { int used; COLORREF c; int hollow; };

static struct MDC     s_dcs[MAX_DCS];
static struct MFont   s_fonts[MAX_FONTS];
static struct MBitmap s_bmps[MAX_BMPS];
static struct MPen    s_pens[MAX_PENS];
static struct MBrush  s_brushes[MAX_BRUSHES];
static struct MBrush  s_nullBrush = { 1, 0, 1 };

static CGColorSpaceRef s_cs;
static CGContextRef    s_viewCtx;      /* live only inside -drawRect: */
static struct MBitmap  s_back;         /* the window's backing store */
static float           s_backScale = 1.0f;

/* caret: Quartz has none, so it is drawn by inverting a rect on a timer */
static HWND  s_caretOwner;
static int   s_caretX, s_caretY, s_caretW, s_caretH, s_caretOn, s_caretShown;
static DWORD s_caretBase;              /* when the current blink cycle started */

static void invalidate_caret(void);

/* ------------------------------------------------------------ bitmaps */

static CGColorSpaceRef colour_space(void)
{
    if (!s_cs) s_cs = CGColorSpaceCreateDeviceRGB();
    return s_cs;
}

/* A bitmap is 32 bits a pixel, one host-order word per pixel laid out
 * 0x00RRGGBB -- which is exactly the 32-bit DIB the app asks GetDIBits for, so
 * the DIB calls below are a word-for-word copy and need know nothing about the
 * byte order. (On the PowerPC target 32Host is big-endian, so the words are
 * XRGB in memory; on a little-endian host build they are BGRX. Naming the
 * order explicitly would make the bitmaps unsupported on one or the other.) */
static int bmp_create_scaled(struct MBitmap *b, int w, int h, float scale)
{
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (scale < 1.0f) scale = 1.0f;
    b->w = w; b->h = h;
    b->pw = (int)(w * scale + 0.5f);
    b->ph = (int)(h * scale + 0.5f);
    b->rowBytes = b->pw * 4;
    b->data = (unsigned char *)calloc(1, (size_t)b->rowBytes * (size_t)b->ph);
    if (!b->data) return 0;
    b->ctx = CGBitmapContextCreate(b->data, (size_t)b->pw, (size_t)b->ph, 8, (size_t)b->rowBytes,
                                   colour_space(),
                                   kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Host);
    if (!b->ctx) { free(b->data); b->data = NULL; return 0; }
    /* Origin top left, y down, in points, for the rest of the file: the scale
     * lives here so that no drawing code has to know about it. */
    CGContextTranslateCTM(b->ctx, 0, (float)b->ph);
    CGContextScaleCTM(b->ctx, scale, -scale);
    CGContextSetInterpolationQuality(b->ctx, kCGInterpolationNone);
    return 1;
}

static int bmp_create(struct MBitmap *b, int w, int h)
{
    return bmp_create_scaled(b, w, h, 1.0f);
}

static void bmp_destroy(struct MBitmap *b)
{
    if (b->ctx) { CGContextRelease(b->ctx); b->ctx = NULL; }
    free(b->data);
    b->data = NULL;
    b->w = b->h = b->pw = b->ph = b->rowBytes = 0;
}

/* Wrap a bitmap's pixels in a CGImage without copying them. The image is used
 * once and released: a direct data provider over a buffer that is still being
 * drawn into must not be kept, or Quartz may serve cached pixels. */
static CGImageRef bmp_image(struct MBitmap *b)
{
    CGDataProviderRef prov;
    CGImageRef img;
    if (!b || !b->data) return NULL;
    prov = CGDataProviderCreateWithData(NULL, b->data,
                                        (size_t)b->rowBytes * (size_t)b->ph, NULL);
    if (!prov) return NULL;
    img = CGImageCreate((size_t)b->pw, (size_t)b->ph, 8, 32, (size_t)b->rowBytes,
                     colour_space(), kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Host,
                     prov, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(prov);
    return img;
}

/* Draw an image into a y-down context: Quartz puts an image's first row at the
 * bottom of the destination rect, so the axis is flipped around it. */
static void draw_image(CGContextRef ctx, CGImageRef img, float x, float y, float w, float h)
{
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, x, y);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, -h, w, h), img);
    CGContextRestoreGState(ctx);
}

/* ------------------------------------------------------------ DC plumbing */

static void set_fill(CGContextRef ctx, COLORREF c)
{
    CGContextSetRGBFillColor(ctx, GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                             GetBValue(c) / 255.0f, 1.0f);
}

static void set_stroke(CGContextRef ctx, COLORREF c)
{
    CGContextSetRGBStrokeColor(ctx, GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                               GetBValue(c) / 255.0f, 1.0f);
}

/* Make dc current: save the graphics state and clip to the DC's rect. Quartz
 * clipping only ever narrows, so every primitive is wrapped in a save/restore
 * pair rather than trying to widen the clip back out. */
static int activate(HDC dc)
{
    if (!dc || !dc->used || !dc->ctx) return 0;
    CGContextSaveGState(dc->ctx);
    CGContextClipToRect(dc->ctx, CGRectMake((float)dc->clip.left, (float)dc->clip.top,
                                            (float)(dc->clip.right - dc->clip.left),
                                            (float)(dc->clip.bottom - dc->clip.top)));
    return 1;
}

static void done(HDC dc) { CGContextRestoreGState(dc->ctx); }

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

/* Called by mac-cocoa/wnd.m at the top and bottom of -drawRect:. The backing
 * store follows the view's size. */
/* The scale comes from mac-cocoa/wnd.m, which asks the window: a layer-backed
 * view hands drawRect: a context whose transform is already in points, so the
 * context cannot be asked. The backing store has to match the display or the
 * whole window is drawn at half resolution and scaled up. */
void gdi_set_window_context(void *ctx, int w, int h, float scale)
{
    s_viewCtx = (CGContextRef)ctx;
    if (!s_viewCtx) return;
    if (scale < 1.0f) scale = 1.0f;

    if (s_back.w != w || s_back.h != h || s_backScale != scale) {
        bmp_destroy(&s_back);
        bmp_create_scaled(&s_back, w, h, scale);
        s_backScale = scale;
    }
}

HDC gdi_begin_window_paint(HWND w, const RECT *screenRC)
{
    HDC dc;
    (void)w;
    if (!s_viewCtx || !s_back.ctx) return NULL;
    dc = alloc_dc();
    if (!dc) return NULL;
    dc->ctx = s_back.ctx;
    dc->bmp = &s_back;
    dc->ox = screenRC->left; dc->oy = screenRC->top;
    dc->clip = *screenRC;
    if (dc->clip.right > s_back.w) dc->clip.right = s_back.w;
    if (dc->clip.bottom > s_back.h) dc->clip.bottom = s_back.h;
    return dc;
}

void gdi_end_window_paint(HDC dc)
{
    if (!dc || !dc->used) return;
    /* copy just what this window owns from the backing store to the view */
    if (s_viewCtx && s_back.data && !IsRectEmpty(&dc->clip)) {
        CGImageRef img = bmp_image(&s_back);
        if (img) {
            CGContextSaveGState(s_viewCtx);
            CGContextClipToRect(s_viewCtx,
                CGRectMake((float)dc->clip.left, (float)dc->clip.top,
                           (float)(dc->clip.right - dc->clip.left),
                           (float)(dc->clip.bottom - dc->clip.top)));
            draw_image(s_viewCtx, img, 0, 0, (float)s_back.w, (float)s_back.h);
            CGContextRestoreGState(s_viewCtx);
            CGImageRelease(img);
        }
    }
    dc->used = 0;
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

/* GetDC is only ever used to measure text -- main.c asks for one before the
 * window exists, to derive the editor's glyph grid -- and every measurement
 * here is answered from the font's own tables, so this DC needs no context. */
HDC GetDC(HWND w)
{
    HDC dc = alloc_dc();
    if (!dc) return NULL;
    dc->ctx = NULL;
    dc->ox = 0; dc->oy = 0;
    if (w) GetClientRect(w, &dc->clip);
    if (!w || IsRectEmpty(&dc->clip)) SetRect(&dc->clip, 0, 0, 1, 1);
    return dc;
}

void ReleaseDC(HWND w, HDC dc) { (void)w; if (dc && dc->used) dc->used = 0; }

/* ------------------------------------------------------- offscreen DCs */

/* The DC starts unbacked and is attached by SelectObject(bitmap). If the
 * bitmap cannot be allocated, SelectObject leaves this aliasing the source DC
 * so drawing still reaches the screen -- without double buffering, but
 * visible. Losing the whole frame is far worse than a little flicker. */
HDC CreateCompatibleDC(HDC src)
{
    HDC dc = alloc_dc();
    if (!dc) return NULL;
    dc->ctx = NULL;
    dc->fallbackOf = src;
    return dc;
}

void DeleteDC(HDC dc) { if (dc && dc->used) dc->used = 0; }

/* Every panel paints through one of these and blits it to the window, so they
 * follow the display's scale as the backing store does -- otherwise everything
 * the panels draw is rendered at half resolution and scaled up, which is most
 * of the window. The app still addresses them in its own coordinates; only
 * GetDIBits has to know, because it hands the app raw pixels. */
HBITMAP CreateCompatibleBitmap(HDC dc, int w, int h)
{
    int i;
    (void)dc;
    for (i = 0; i < MAX_BMPS; i++)
        if (!s_bmps[i].used) {
            if (!bmp_create_scaled(&s_bmps[i], w, h, s_backScale)) return NULL;
            s_bmps[i].used = 1;
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
         * still draws, straight into the backing store. */
        if (!dc->ctx && dc->fallbackOf && dc->fallbackOf->used) {
            dc->ctx = dc->fallbackOf->ctx;
            dc->bmp = dc->fallbackOf->bmp;
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
            dc->bmp = &s_bmps[i];
            dc->ctx = s_bmps[i].ctx;
            dc->ox = dc->oy = 0;
            SetRect(&dc->clip, 0, 0, s_bmps[i].w, s_bmps[i].h);
            return NULL;
        }
    return NULL;
}

BOOL DeleteObject(HGDIOBJ obj)
{
    int i;
    if (!obj) return FALSE;
    for (i = 0; i < MAX_BMPS; i++)
        if (obj == (HGDIOBJ)&s_bmps[i] && s_bmps[i].used) {
            int j;
            /* any DC still pointing at this bitmap must let go of it first */
            for (j = 0; j < MAX_DCS; j++)
                if (s_dcs[j].used && s_dcs[j].bmp == &s_bmps[i]) {
                    s_dcs[j].bmp = NULL;
                    s_dcs[j].ctx = NULL;
                }
            bmp_destroy(&s_bmps[i]);
            s_bmps[i].used = 0;
            return TRUE;
        }
    for (i = 0; i < MAX_FONTS; i++)
        if (obj == (HGDIOBJ)&s_fonts[i] && s_fonts[i].used) {
            [s_fonts[i].nsfont release];
            s_fonts[i].nsfont = nil;
            s_fonts[i].used = 0;
            return TRUE;
        }
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
    CGImageRef img;
    (void)rop;
    if (!dst || !src || !dst->ctx || !src->ctx || !src->bmp) return FALSE;
    /* src never got its own bitmap and is aliasing dst: already in place */
    if (src->ctx == dst->ctx && src->bmp == dst->bmp && src->ox == dst->ox) return TRUE;

    img = bmp_image(src->bmp);
    if (!img) return FALSE;
    if (activate(dst)) {
        /* Clip to the destination and slide the whole image so that the
         * requested source rect lands on it. Cheaper than cropping, and it
         * does not depend on CGImageCreateWithImageInRect's orientation. */
        CGContextClipToRect(dst->ctx, CGRectMake((float)(dst->ox + x), (float)(dst->oy + y),
                                                 (float)w, (float)h));
        draw_image(dst->ctx, img,
                   (float)(dst->ox + x - (src->ox + sx)),
                   (float)(dst->oy + y - (src->oy + sy)),
                   (float)src->bmp->w, (float)src->bmp->h);
        done(dst);
    }
    CGImageRelease(img);
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
static float pen_width(HDC dc) { return (float)(dc->penObj && dc->penObj->width > 0 ? dc->penObj->width : 1); }
static void apply_pen(HDC dc)
{
    CGContextSetLineWidth(dc->ctx, pen_width(dc));
    /* A wide Win32 pen has round ends and round joins; Quartz would give it
     * butt ends and a mitre. It shows wherever the app strokes something
     * thick -- the editor's watermark is a 22px chevron drawn as two separate
     * lines, and with butt ends they meet in a notch instead of a point. */
    CGContextSetLineCap(dc->ctx, kCGLineCapRound);
    CGContextSetLineJoin(dc->ctx, kCGLineJoinRound);
    set_stroke(dc->ctx, pen_colour(dc));
}
/* non-zero when the shape should be filled, with the colour in *out */
static int brush_fill(HDC dc, COLORREF *out)
{
    if (!dc->brushObj || dc->brushObj->hollow) return 0;
    *out = dc->brushObj->c;
    return 1;
}

/* A 1px GDI line covers whole pixels; a Quartz stroke straddles the path, so
 * an odd width has to sit on a half-pixel to come out crisp. */
static float stroke_offset(HDC dc)
{
    int w = (int)pen_width(dc);
    return (w & 1) ? 0.5f : 0.0f;
}

/* ------------------------------------------------------------ fonts */

/* What Core Graphics will advance for these bytes; defined with the rest of
 * the text measuring, below. */
static float cg_advance(struct MFont *f, const char *s, int n);

HFONT CreateFont(int h, int w, int esc, int orient, int weight, DWORD italic,
                 DWORD underline, DWORD strikeout, DWORD charset, DWORD outprec,
                 DWORD clipprec, DWORD quality, DWORD pitch, LPCSTR face)
{
    int i;
    (void)w; (void)esc; (void)orient; (void)charset;
    (void)outprec; (void)clipprec; (void)quality; (void)strikeout; (void)underline;

    for (i = 0; i < MAX_FONTS; i++)
        if (!s_fonts[i].used) {
            struct MFont *f = &s_fonts[i];
            NSFont *nsf;
            int mono = 0, k;

            memset(f, 0, sizeof *f);
            f->used = 1;

            /* FIXED_PITCH decides whether this is a code font, not the face
             * name. The editor asks for "Lucida Console", which matches none
             * of the obvious monospace names, so a name-based guess handed it
             * a proportional font: the text was proportionally spaced while
             * the caret and column arithmetic still assumed a fixed cell, and
             * the cursor drifted further right the longer the line. */
            if ((pitch & 0x03) == FIXED_PITCH) mono = 1;
            else if (face && (strstr(face, "Consolas") || strstr(face, "Courier") ||
                              strstr(face, "Console") || strstr(face, "Mono") ||
                              strstr(face, "Fixed") || strstr(face, "Terminal")))
                mono = 1;

            /* The height Win32 is given is in device pixels, on a display it
             * calls 96 dpi; a Quartz point is a 72nd of an inch. The two font
             * roles want different things done about that.
             *
             * The code font is converted, because the editor measures its
             * whole text grid in these units -- line height, gutter, caret
             * column -- and carrying the number across unchanged inflates the
             * editor by a third against the layout the app was written for.
             *
             * The interface font is not. Its size is a judgement about
             * legibility rather than a grid, and the Mac's own interface font
             * runs 11pt to 13pt where Windows asks for 11 and 12 -- so the
             * number the app gives is already the right one here, and
             * converting it leaves every label too small to read. */
            f->size = (float)(h < 0 ? -h : h);
            if (mono) f->size = f->size * 72.0f / 96.0f;
            if (f->size <= 0) f->size = 11;

            /* The requested faces are Tahoma, Trebuchet MS and Lucida Console,
             * none of which Mac OS X has; take what the system offers for the
             * same job -- Monaco for code, Lucida Grande for everything else.
             *
             * Named outright rather than asked for as "the system font",
             * because CGContextSelectFont has to be able to look the font up
             * again by name, and on 10.4 these are the system's own fonts. */
            if (mono) {
                nsf = [NSFont fontWithName:@"Monaco" size:f->size];
                if (!nsf) nsf = [NSFont userFixedPitchFontOfSize:f->size];
            } else {
                nsf = [NSFont fontWithName:(weight >= FW_BOLD ? @"LucidaGrande-Bold"
                                                              : @"LucidaGrande")
                                      size:f->size];
                if (!nsf) nsf = (weight >= FW_BOLD) ? [NSFont boldSystemFontOfSize:f->size]
                                                    : [NSFont systemFontOfSize:f->size];
            }
            if (italic) {
                NSFont *slanted = [[NSFontManager sharedFontManager]
                                      convertFont:nsf toHaveTrait:NSItalicFontMask];
                if (slanted) nsf = slanted;
            }
            if (!nsf) nsf = [NSFont systemFontOfSize:f->size];

            f->fixed = mono;
            f->nsfont = [nsf retain];
            strncpy(f->cgname, [[nsf fontName] UTF8String], sizeof f->cgname - 1);
            /* A name beginning with a dot is one of the system's hidden fonts
             * (.AppleSystemUIFont and the like), which CGContextSelectFont
             * cannot look up -- it silently draws nothing. Everything about
             * such a font goes through AppKit instead. */
            f->cgok = f->cgname[0] && f->cgname[0] != '.';

            f->ascent = (int)ceil([nsf ascender]);
            f->ch = f->ascent + (int)ceil(-[nsf descender]);
            f->leading = (int)ceil([nsf leading]);
            if (f->leading < 0) f->leading = 0;

            /* Measure every ASCII byte once, through Core Graphics, which is
             * what draws them. ShowText applies no kerning, so the sum of
             * these advances is exactly what it will draw -- and none of it
             * needs a window, which matters because main.c builds its fonts
             * and lays the whole editor out from them before there is one. */
            for (k = 0; k < 128; k++) {
                char byte = (char)k;
                f->adv[k] = cg_advance(f, &byte, 1);
            }
            f->adv0 = f->adv['0'];
            f->cw = (int)(f->adv0 + 0.5f);

            /* Never leave a font with a zero cell: main.c derives the editor's
             * whole glyph grid from these, and a zero width means it can lay
             * nothing out and the window stays blank. */
            if (f->ch <= 0)     f->ch = (int)f->size + (int)f->size / 4 + 1;
            if (f->ascent <= 0) f->ascent = (int)f->size;
            if (f->cw <= 0)     f->cw = ((int)f->size * 3 + 4) / 5;
            if (f->adv0 <= 0)   f->adv0 = (float)f->cw;

            return (HFONT)f;
        }
    return NULL;
}

static void apply_font(HDC dc)
{
    struct MFont *f = dc->font;
    if (!f) return;
    /* The only encodings Core Graphics text takes are Mac OS Roman and the
     * font's own, and this path only ever draws ASCII, where Mac OS Roman and
     * UTF-8 are the same bytes. */
    CGContextSelectFont(dc->ctx, f->cgname, f->size, kCGEncodingMacRoman);
    /* the context is y-down, so the glyphs need flipping back upright */
    CGContextSetTextMatrix(dc->ctx, CGAffineTransformMakeScale(1, -1));
    /* A fixed-pitch cell is an integer in the editor's arithmetic but a
     * fraction in the font (Monaco at 14pt advances 8.4px). Padding each glyph
     * out to the whole pixel the editor was told about is what keeps the text
     * under the caret. */
    CGContextSetCharacterSpacing(dc->ctx, f->fixed ? (float)f->cw - f->adv0 : 0.0f);
}

/* The two paths differ only above 0x7F, where UTF-8 and the advance table
 * part company. */
static int all_ascii(LPCSTR s, int n)
{
    int i;
    for (i = 0; i < n; i++) if ((unsigned char)s[i] >= 0x80) return 0;
    return 1;
}

/* A 1x1 scratch context, only ever used to ask Core Graphics how far a string
 * advances. */
static CGContextRef measure_context(void)
{
    static CGContextRef ctx;
    static unsigned char pixel[4];
    if (!ctx)
        ctx = CGBitmapContextCreate(pixel, 1, 1, 8, 4, colour_space(),
                                    kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Host);
    return ctx;
}

/* What CGContextShowText will advance for these bytes.
 *
 * Measured by drawing them invisibly and reading the text position back,
 * because that is the only way to ask the engine that will actually draw
 * them. AppKit's -sizeWithAttributes: answers a different question on 10.4:
 * it measures a screen font, whose advances are whole pixels -- 7 where the
 * outline advances 6.3 for Monaco at this size. Sizing the fixed-pitch cell
 * from that number and then drawing with Core Graphics put the text and the
 * caret on different grids, and the two drifted a pixel apart per character
 * all the way along the line. */
static float cg_advance(struct MFont *f, const char *s, int n)
{
    CGContextRef m = measure_context();
    if (!m || n <= 0) return 0.0f;
    CGContextSetTextDrawingMode(m, kCGTextInvisible);
    CGContextSelectFont(m, f->cgname, f->size, kCGEncodingMacRoman);
    CGContextSetCharacterSpacing(m, 0.0f);
    CGContextSetTextMatrix(m, CGAffineTransformIdentity);
    CGContextShowTextAtPoint(m, 0, 0, s, (size_t)n);
    return (float)CGContextGetTextPosition(m).x;
}

static NSDictionary *font_attrs(struct MFont *f)
{
    return [NSDictionary dictionaryWithObject:f->nsfont forKey:NSFontAttributeName];
}

static int text_width(struct MFont *f, LPCSTR s, int n)
{
    float sum = 0;
    int i;
    if (!f || n <= 0) return 0;
    /* A fixed-pitch run is always n cells wide, even where a character spans
     * several bytes: the editor's column arithmetic counts bytes, and the
     * caret has to land where the editor thinks it is. */
    if (f->fixed) return n * f->cw;
    if ((!all_ascii(s, n) || !f->cgok) && f->nsfont)
        return (int)([shim_string(s, n) sizeWithAttributes:font_attrs(f)].width + 0.5f);
    for (i = 0; i < n; i++) sum += f->adv[(unsigned char)s[i]];
    return (int)(sum + 0.5f);
}

/* Answered from the font's own cached metrics, which CreateFont guarantees are
 * non-zero. main.c calls this through GetDC(NULL) before the window exists and
 * uses the result without checking it. */
BOOL GetTextMetrics(HDC dc, TEXTMETRIC *tm)
{
    memset(tm, 0, sizeof *tm);
    if (!dc || !dc->font) return FALSE;
    tm->tmAscent = dc->font->ascent;
    tm->tmHeight = dc->font->ch;
    tm->tmDescent = dc->font->ch - dc->font->ascent;
    tm->tmAveCharWidth = dc->font->cw;
    tm->tmMaxCharWidth = dc->font->cw;
    return TRUE;
}

BOOL GetTextExtentPoint32(HDC dc, LPCSTR s, int n, LPSIZE sz)
{
    sz->cx = sz->cy = 0;
    if (!dc || !dc->font) return FALSE;
    if (n < 0) n = (int)strlen(s);
    sz->cx = text_width(dc->font, s, n);
    sz->cy = dc->font->ch;
    return TRUE;
}

/* ------------------------------------------------------------ text & fills */

/* The Unicode path: decode the bytes and let AppKit lay them out. Only runs
 * with a high byte in them come through here. */
static void draw_unicode(HDC dc, int x, int y, LPCSTR s, int n)
{
    struct MFont *f = dc->font;
    NSGraphicsContext *gc;
    NSMutableDictionary *attrs;
    NSColor *colour;

    gc = [NSGraphicsContext graphicsContextWithGraphicsPort:dc->ctx flipped:YES];
    if (!gc) return;
    colour = [NSColor colorWithDeviceRed:GetRValue(dc->fg) / 255.0f
                                   green:GetGValue(dc->fg) / 255.0f
                                    blue:GetBValue(dc->fg) / 255.0f
                                   alpha:1.0f];
    attrs = [NSMutableDictionary dictionaryWithObjectsAndKeys:
                f->nsfont, NSFontAttributeName,
                colour, NSForegroundColorAttributeName, NULL];

    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:gc];
    /* -drawAtPoint: takes the top of the line box, and puts the baseline a
     * leading plus an ascent below it; the Core Graphics path above puts it an
     * ascent below y. Backing the leading out keeps the two on one baseline. */
    [shim_string(s, n) drawAtPoint:NSMakePoint((float)(dc->ox + x),
                                               (float)(dc->oy + y - f->leading))
                   withAttributes:attrs];
    [NSGraphicsContext restoreGraphicsState];
}

static void draw_text(HDC dc, int x, int y, LPCSTR s, int n)
{
    if (dc->bkmode == OPAQUE) {
        /* Win32 paints the text cell behind the glyphs in this mode. */
        set_fill(dc->ctx, dc->bk);
        CGContextFillRect(dc->ctx, CGRectMake((float)(dc->ox + x), (float)(dc->oy + y),
                                              (float)text_width(dc->font, s, n),
                                              (float)(dc->font ? dc->font->ch : 12)));
    }
    if (dc->font && dc->font->nsfont && (!all_ascii(s, n) || !dc->font->cgok)) {
        draw_unicode(dc, x, y, s, n);
        return;
    }
    apply_font(dc);
    set_fill(dc->ctx, dc->fg);
    CGContextSetTextDrawingMode(dc->ctx, kCGTextFill);
    CGContextShowTextAtPoint(dc->ctx, (float)(dc->ox + x),
                             (float)(dc->oy + y + (dc->font ? dc->font->ascent : 10)),
                             s, (size_t)n);
}

BOOL ExtTextOut(HDC dc, int x, int y, UINT flags, const RECT *r, LPCSTR s, UINT n, const int *dx)
{
    (void)dx;
    if (!activate(dc)) return FALSE;

    /* fill() and fill_rc() in main.c paint solid rectangles through this call
     * with ETO_OPAQUE and no text, which is the shim's commonest operation. */
    if ((flags & ETO_OPAQUE) && r) {
        set_fill(dc->ctx, dc->bk);
        CGContextFillRect(dc->ctx, CGRectMake((float)(dc->ox + r->left), (float)(dc->oy + r->top),
                                              (float)(r->right - r->left),
                                              (float)(r->bottom - r->top)));
    }
    if (s && n) draw_text(dc, x, y, s, (int)n);
    done(dc);
    return TRUE;
}

BOOL TextOut(HDC dc, int x, int y, LPCSTR s, int n)
{
    if (!activate(dc)) return FALSE;
    if (s && n > 0) draw_text(dc, x, y, s, n);
    done(dc);
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
    if (!dc) return;
    if (activate(dc)) {
        float off = stroke_offset(dc);
        apply_pen(dc);
        CGContextBeginPath(dc->ctx);
        CGContextMoveToPoint(dc->ctx, (float)(dc->ox + dc->pen.x) + off,
                             (float)(dc->oy + dc->pen.y) + off);
        CGContextAddLineToPoint(dc->ctx, (float)(dc->ox + x) + off, (float)(dc->oy + y) + off);
        CGContextStrokePath(dc->ctx);
        done(dc);
    }
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

void Rectangle(HDC dc, int l, int t, int r, int b)
{
    COLORREF fillc;
    float off;
    if (!activate(dc)) return;
    off = stroke_offset(dc);
    if (brush_fill(dc, &fillc)) {
        set_fill(dc->ctx, fillc);
        CGContextFillRect(dc->ctx, CGRectMake((float)(dc->ox + l), (float)(dc->oy + t),
                                              (float)(r - l), (float)(b - t)));
    }
    apply_pen(dc);
    CGContextStrokeRect(dc->ctx, CGRectMake((float)(dc->ox + l) + off, (float)(dc->oy + t) + off,
                                            (float)(r - l) - 2 * off, (float)(b - t) - 2 * off));
    done(dc);
}

void Ellipse(HDC dc, int l, int t, int r, int b)
{
    COLORREF fillc;
    float off;
    CGRect box;
    if (!activate(dc)) return;
    off = stroke_offset(dc);
    box = CGRectMake((float)(dc->ox + l), (float)(dc->oy + t), (float)(r - l), (float)(b - t));
    if (brush_fill(dc, &fillc)) {
        set_fill(dc->ctx, fillc);
        CGContextBeginPath(dc->ctx);
        CGContextAddEllipseInRect(dc->ctx, box);
        CGContextFillPath(dc->ctx);
    }
    apply_pen(dc);
    CGContextBeginPath(dc->ctx);
    CGContextAddEllipseInRect(dc->ctx, CGRectMake(box.origin.x + off, box.origin.y + off,
                                                  box.size.width - 2 * off,
                                                  box.size.height - 2 * off));
    CGContextStrokePath(dc->ctx);
    done(dc);
}

void Polygon(HDC dc, const POINT *pts, int n)
{
    COLORREF fillc;
    float off;
    int i;
    if (n < 2 || !activate(dc)) return;
    off = stroke_offset(dc);
    if (brush_fill(dc, &fillc)) {
        set_fill(dc->ctx, fillc);
        CGContextBeginPath(dc->ctx);
        CGContextMoveToPoint(dc->ctx, (float)(dc->ox + pts[0].x), (float)(dc->oy + pts[0].y));
        for (i = 1; i < n; i++)
            CGContextAddLineToPoint(dc->ctx, (float)(dc->ox + pts[i].x), (float)(dc->oy + pts[i].y));
        CGContextClosePath(dc->ctx);
        CGContextFillPath(dc->ctx);
    }
    apply_pen(dc);
    CGContextBeginPath(dc->ctx);
    CGContextMoveToPoint(dc->ctx, (float)(dc->ox + pts[0].x) + off, (float)(dc->oy + pts[0].y) + off);
    for (i = 1; i < n; i++)
        CGContextAddLineToPoint(dc->ctx, (float)(dc->ox + pts[i].x) + off,
                                (float)(dc->oy + pts[i].y) + off);
    CGContextClosePath(dc->ctx);
    CGContextStrokePath(dc->ctx);
    done(dc);
}

void Arc(HDC dc, int l, int t, int r, int b, int xs, int ys, int xe, int ye)
{
    (void)xs; (void)ys; (void)xe; (void)ye;
    /* the icons only ever ask for whole circles */
    Ellipse(dc, l, t, r, b);
}

/* --------------------------------------------------------- DIB access */

/* main.c draws every icon by supersampling it into an offscreen bitmap,
 * reading the pixels back, antialiasing against the destination in software
 * and writing the result out again. That needs real GetDIBits and
 * SetDIBitsToDevice, so these walk the bitmap's pixels directly.
 *
 * The app asks for 32-bit top-down DIBs (biBitCount 32, negative biHeight), so
 * each pixel is a DWORD laid out 0x00RRGGBB -- which is what a pixel of one of
 * these bitmaps already is, with the top row first, so both directions are a
 * word copy with the unused byte masked off. */

static struct MBitmap *find_bmp(HBITMAP b)
{
    int i;
    for (i = 0; i < MAX_BMPS; i++)
        if ((HGDIOBJ)&s_bmps[i] == (HGDIOBJ)b && s_bmps[i].used) return &s_bmps[i];
    return NULL;
}

int GetDIBits(HDC dc, HBITMAP b, UINT start, UINT lines, LPVOID bits, LPVOID info, UINT usage)
{
    struct MBitmap *bm = find_bmp(b);
    BITMAPINFO *bi = (BITMAPINFO *)info;
    DWORD *out = (DWORD *)bits;
    int w, h, y, x, sx, sy;
    (void)dc; (void)usage;

    if (!bm || !bm->data || !out || !bi) return 0;
    w = (int)bi->bmiHeader.biWidth;
    if (w <= 0 || w > bm->w) w = bm->w;
    h = (int)lines;
    if (h <= 0 || (int)start + h > bm->h) h = bm->h - (int)start;
    if (h <= 0) return 0;

    /* A bitmap may hold several pixels to the point (see
     * CreateCompatibleBitmap); the app counts in points, so take the first
     * pixel of each block. The icon code this feeds is blending against the
     * destination, where a sample is as good as an average. */
    sx = bm->w > 0 ? bm->pw / bm->w : 1;
    sy = bm->h > 0 ? bm->ph / bm->h : 1;
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;

    for (y = 0; y < h; y++) {
        const DWORD *row = (const DWORD *)(bm->data +
                               (size_t)(((int)start + y) * sy) * bm->rowBytes);
        for (x = 0; x < w; x++) out[y * w + x] = row[x * sx] & 0x00FFFFFFu;
    }
    return h;
}

int SetDIBitsToDevice(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                      UINT start, UINT lines, LPVOID bits, LPVOID info, UINT usage)
{
    const DWORD *in = (const DWORD *)bits;
    struct MBitmap tmp;
    CGImageRef img;
    int iy, ix;
    (void)sx; (void)sy; (void)start; (void)lines; (void)info; (void)usage;

    if (!dc || !dc->ctx || !in || w == 0 || h == 0) return 0;

    memset(&tmp, 0, sizeof tmp);
    if (!bmp_create(&tmp, (int)w, (int)h)) return 0;
    for (iy = 0; iy < (int)h; iy++) {
        DWORD *row = (DWORD *)(tmp.data + (size_t)iy * tmp.rowBytes);
        for (ix = 0; ix < (int)w; ix++) row[ix] = in[iy * (int)w + ix] & 0x00FFFFFFu;
    }

    img = bmp_image(&tmp);
    if (img) {
        if (activate(dc)) {
            draw_image(dc->ctx, img, (float)(dc->ox + x), (float)(dc->oy + y),
                       (float)w, (float)h);
            done(dc);
        }
        CGImageRelease(img);
    }
    bmp_destroy(&tmp);
    return (int)h;
}

/* PatBlt is used with BLACKNESS to clear the supersampling buffer, so the
 * pattern operations have to mean what they say rather than paint bk. */
BOOL PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    if (!activate(dc)) return FALSE;
    if (rop == BLACKNESS)      set_fill(dc->ctx, RGB(0, 0, 0));
    else if (rop == WHITENESS) set_fill(dc->ctx, RGB(255, 255, 255));
    else                       set_fill(dc->ctx, dc->bk);
    CGContextFillRect(dc->ctx, CGRectMake((float)(dc->ox + x), (float)(dc->oy + y),
                                          (float)w, (float)h));
    done(dc);
    return TRUE;
}

/* ------------------------------------------------------------ caret */

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
    s_caretBase = GetTickCount();
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
    s_caretBase = GetTickCount();
    s_caretOn = 1;
    if (s_caretShown) invalidate_caret();
    return TRUE;
}

void gdi_caret_owner_gone(HWND w) { if (s_caretOwner == w) DestroyCaret(); }

int gdi_caret_active(void) { return s_caretShown && s_caretOwner != NULL; }

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
 * gets toggled. The editor calls SetCaretPos on every redraw, and anything
 * that reset a toggle from there would pin the caret on and stop it blinking. */
#define CARET_BLINK_MS 500

static int caret_phase(void)
{
    return ((GetTickCount() - s_caretBase) / CARET_BLINK_MS) % 2 == 0;
}

void gdi_caret_tick(void)
{
    int on;
    if (!s_caretShown || !s_caretOwner) return;
    on = caret_phase();
    if (on == s_caretOn) return;    /* nothing to redraw yet */
    s_caretOn = on;
    invalidate_caret();
}

/* Draw the caret over a window that has just painted.
 *
 * Win32 has the system draw the caret on the application's behalf, so the
 * editor never draws one itself -- it only calls CreateCaret/SetCaretPos and
 * expects a blinking bar to appear. mac-cocoa/wnd.m calls this after each
 * window's WM_PAINT, which puts it on top of the panel's own blitted buffer. */
void gdi_paint_caret(HWND w, HDC dc)
{
    if (!s_caretShown || !s_caretOn || !s_caretOwner || s_caretOwner != w) return;
    if (!activate(dc)) return;
    /* Difference against white inverts, which shows up over any background the
     * editor painted -- the dark theme included. */
    CGContextSetBlendMode(dc->ctx, kCGBlendModeDifference);
    set_fill(dc->ctx, RGB(255, 255, 255));
    CGContextFillRect(dc->ctx, CGRectMake((float)(dc->ox + s_caretX), (float)(dc->oy + s_caretY),
                                          (float)(s_caretW > 0 ? s_caretW : 2),
                                          (float)(s_caretH > 0 ? s_caretH : 12)));
    CGContextSetBlendMode(dc->ctx, kCGBlendModeNormal);
    done(dc);
}
