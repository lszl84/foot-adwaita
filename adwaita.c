#include "adwaita.h"

/* Per-pixel math below: skip errno handling in sqrtf() */
#pragma GCC optimize ("no-math-errno")

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcft/fcft.h>
#include <pixman.h>

#define LOG_MODULE "adwaita"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "render.h"
#include "wayland.h"
#include "util.h"
#include "xmalloc.h"

#define MINF(a, b) ((a) < (b) ? (a) : (b))
#define MAXF(a, b) ((a) > (b) ? (a) : (b))

struct rgba { float r, g, b, a; };  /* straight (non-premultiplied) */

struct shadow { float x, y, blur, spread, alpha; };

/* window.csd box-shadow (black) */
static const struct shadow shadow_focused[] = {
    {0, 2,  8,  2, .13f},
    {0, 3, 20, 10, .09f},
    {0, 6, 32, 16, .04f},
    {0, 0,  0,  1, .005f},
};

/* window.csd:backdrop box-shadow */
static const struct shadow shadow_backdrop[] = {
    {0, 1,  3,  3, .09f},
    {0, 2, 14,  5, .05f},
    {0, 4, 28, 12, .03f},
    {0, 0,  0,  1, .02f},
};

#define RGB(hex, alpha) \
    {((hex) >> 16 & 0xff) / 255.f, ((hex) >> 8 & 0xff) / 255.f, ((hex) & 0xff) / 255.f, alpha}

/* defaults-dark.css / defaults-light.css */
static const struct {
    struct rgba header_bg;
    struct rgba header_backdrop;
    struct rgba fg;
    struct rgba shade;
} palette[2] = {
    [0] = {  /* light */
        .header_bg = RGB(0xffffff, 1),
        .header_backdrop = RGB(0xfafafb, 1),
        .fg = RGB(0x000006, .8f),
        .shade = RGB(0x000006, .12f),
    },
    [1] = {  /* dark */
        .header_bg = RGB(0x2e2e32, 1),
        .header_backdrop = RGB(0x222226, 1),
        .fg = RGB(0xffffff, 1),
        .shade = RGB(0x000006, .36f),
    },
};

struct ctx {
    float s;        /* scale */
    float W, H, T;  /* frame is x=[0,W], y=[-T,H] in grid pixel coordinates */
    float R;        /* corner radius */
    int E;          /* shadow extent (border surface size) */
    bool floating;  /* rounded corners and shadow */
    bool tiled;     /* 1px border only */
    bool focused;
    bool dark;
    const struct shadow *shadows;
    size_t shadow_count;
};

static void
ctx_init(struct ctx *c, const struct terminal *term)
{
    const struct wl_window *win = term->window;
    const float s = term->scale;

    c->s = s;
    c->W = term->width;
    c->H = term->height;
    c->T = wayl_win_csd_titlebar_visible(win)
        ? roundf(term->conf->csd.title_height * s) : 0;
    c->focused = term->visual_focus;
    c->dark = term->colors.active_theme != COLOR_THEME_LIGHT;

    const bool framed = win->csd_mode == CSD_YES &&
        !win->is_fullscreen && !win->is_maximized;
    c->floating = framed && !win->is_tiled;
    c->tiled = framed && win->is_tiled;

    c->R = c->floating ? roundf(15 * s) : 0;
    c->R = fminf(c->R, fminf(c->W, c->H + c->T) / 2);

    c->E = roundf(term->conf->csd.border_width * s);
    c->shadows = c->focused ? shadow_focused : shadow_backdrop;
    c->shadow_count = c->focused ? ALEN(shadow_focused) : ALEN(shadow_backdrop);
}

static inline float
clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Signed distance from (px,py) to a rounded rectangle (negative inside) */
static inline float
sdf_rrect(float px, float py, float x0, float y0, float x1, float y1, float r)
{
    const float hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
    r = MINF(r, MINF(hx, hy));
    const float qx = fabsf(px - (x0 + hx)) - (hx - r);
    const float qy = fabsf(py - (y0 + hy)) - (hy - r);
    const float ox = MAXF(qx, 0), oy = MAXF(qy, 0);
    return sqrtf(ox * ox + oy * oy) + MINF(MAXF(qx, qy), 0) - r;
}

static inline float
sdf_frame(const struct ctx *c, float px, float py)
{
    return sdf_rrect(px, py, 0, -c->T, c->W, c->H, c->R);
}

/*
 * Gaussian blurred edge: 0.5 * erfc(t / sqrt(2)), where t is the
 * distance from the edge in standard deviations. Looked up in a
 * table, since erfc() is far too slow to call per pixel.
 */
#define GAUSS_LUT_SIZE 1024
#define GAUSS_LUT_RANGE 4.f
static float gauss_lut[GAUSS_LUT_SIZE + 1];

static inline float
gauss_edge(float t)
{
    if (t <= -GAUSS_LUT_RANGE)
        return 1;
    if (t >= GAUSS_LUT_RANGE)
        return 0;

    if (gauss_lut[0] == 0) {
        for (size_t i = 0; i <= GAUSS_LUT_SIZE; i++) {
            float x = -GAUSS_LUT_RANGE + 2 * GAUSS_LUT_RANGE * i / GAUSS_LUT_SIZE;
            gauss_lut[i] = .5f * erfcf(x / sqrtf(2));
        }
    }

    const float f = (t + GAUSS_LUT_RANGE) * (GAUSS_LUT_SIZE / (2 * GAUSS_LUT_RANGE));
    const int i = (int)f;
    const float frac = f - i;
    return gauss_lut[i] + (gauss_lut[i + 1] - gauss_lut[i]) * frac;
}

static inline uint32_t
pack(float r, float g, float b, float a)
{
    /* Premultiplied ARGB8888 */
    return (uint32_t)(clampf(a, 0, 1) * 255 + .5f) << 24 |
           (uint32_t)(clampf(r, 0, 1) * 255 + .5f) << 16 |
           (uint32_t)(clampf(g, 0, 1) * 255 + .5f) << 8 |
           (uint32_t)(clampf(b, 0, 1) * 255 + .5f);
}

static inline uint32_t
pack_rgba(struct rgba c, float coverage)
{
    const float a = c.a * coverage;
    return pack(c.r * a, c.g * a, c.b * a, a);
}

/* src OVER dst, both premultiplied, src scaled by coverage */
static inline uint32_t
over(uint32_t src, uint32_t dst, float coverage)
{
    if (coverage >= 1 && src >> 24 == 0xff)
        return src;

    uint32_t out = 0;
    const float sa = (src >> 24) / 255.f * coverage;
    for (int shift = 0; shift <= 24; shift += 8) {
        const float sc = (src >> shift & 0xff) * coverage;
        const float dc = dst >> shift & 0xff;
        out |= (uint32_t)clampf(sc + dc * (1 - sa) + .5f, 0, 255) << shift;
    }
    return out;
}

/*
 * What's visible *outside* the window frame at (px,py): the window
 * shadow, and in dark mode a faint light outline (window.csd outline).
 */
static uint32_t
decor_pixel(const struct ctx *c, float px, float py)
{
    const float s = c->s;

    if (c->floating) {
        float keep = 1;

        for (size_t i = 0; i < c->shadow_count; i++) {
            const struct shadow *sh = &c->shadows[i];
            const float sp = sh->spread * s;
            const float d = sdf_rrect(
                px - sh->x * s, py - sh->y * s,
                -sp, -c->T - sp, c->W + sp, c->H + sp, c->R + sp);

            const float sigma = sh->blur * s / 2;
            const float a = sigma > 0
                ? gauss_edge(d / sigma)
                : clampf(.5f - d, 0, 1);
            keep *= 1 - sh->alpha * a;
        }

        return pack(0, 0, 0, 1 - keep);  /* black */
    }

    if (c->tiled) {
        const float d = sdf_frame(c, px, py);
        const float ring = clampf(.5f - (d - s), 0, 1) - clampf(.5f - d, 0, 1);
        struct rgba fg = palette[c->dark].fg;
        fg.a *= .15f;  /* --border-opacity */
        return pack_rgba(fg, ring);
    }

    return 0;
}

/*
 * window.csd { outline: 1px solid white/7%; outline-offset: -1px; }
 * i.e. a faint light line just *inside* the frame, drawn on top of
 * the header bar and the terminal.
 */
#define OUTLINE_PIXEL 0x12121212u  /* premultiplied white, 7% */

static inline float
outline_coverage(const struct ctx *c, float d)
{
    return clampf(.5f - d, 0, 1) - clampf(.5f - (d + c->s), 0, 1);
}

static inline uint32_t
with_outline(const struct ctx *c, uint32_t px, float d)
{
    const float cov = outline_coverage(c, d);
    return cov > 0 ? over(OUTLINE_PIXEL, px, cov) : px;
}

/* Frame coverage of the pixel at (px,py) - fractional on the rounded corners */
static inline float
frame_coverage(const struct ctx *c, float px, float py)
{
    return clampf(.5f - sdf_frame(c, px, py), 0, 1);
}

/*
 * The decorations outside the frame only vary near the corners; along
 * the edges they depend on the distance to the frame alone. So we
 * compute the top-left and bottom-left corner patches plus the three
 * edge profiles once, and mirror them for the right side (shadows have
 * no horizontal offset). Shared by all windows; one cache per focus
 * state, since the shadow differs.
 */
struct decor_cache {
    bool valid;
    float s;
    int R, E;
    bool floating, tiled, dark;

    int cols;     /* E + R */
    int tl_rows;  /* E + R + y_ofs: rows from E above the frame */
    int bl_rows;  /* R + E: rows from R above the frame's bottom */
    int y_ofs;    /* largest shadow y-offset */
    uint32_t *tl, *bl;
    uint32_t *top, *bottom, *side;  /* E entries each, outermost first
                                       (bottom: innermost first) */
};

static struct decor_cache decor_caches[2];  /* [focused] */

static const struct decor_cache *
decor_cache_get(const struct ctx *c)
{
    struct decor_cache *k = &decor_caches[c->focused];

    if (k->valid && k->s == c->s && k->R == (int)c->R && k->E == c->E &&
        k->floating == c->floating && k->tiled == c->tiled && k->dark == c->dark)
    {
        return k;
    }

    free(k->tl); free(k->bl); free(k->top); free(k->bottom); free(k->side);

    float max_y_ofs = 0;
    for (size_t i = 0; c->floating && i < c->shadow_count; i++)
        max_y_ofs = MAXF(max_y_ofs, c->shadows[i].y * c->s);

    *k = (struct decor_cache){
        .valid = true, .s = c->s, .R = c->R, .E = c->E,
        .floating = c->floating, .tiled = c->tiled, .dark = c->dark,
        .y_ofs = ceilf(max_y_ofs),
    };

    const int E = k->E, R = k->R;
    k->cols = E + R;
    k->tl_rows = E + R + k->y_ofs;
    k->bl_rows = R + E;
    k->tl = xmalloc(k->cols * k->tl_rows * sizeof(uint32_t));
    k->bl = xmalloc(k->cols * k->bl_rows * sizeof(uint32_t));
    k->top = xmalloc(E * sizeof(uint32_t));
    k->bottom = xmalloc(E * sizeof(uint32_t));
    k->side = xmalloc(E * sizeof(uint32_t));

    /* A frame big enough that its far edges cast nothing on the patches */
    struct ctx v = *c;
    v.T = 0;
    v.W = v.H = 16384;

    for (int y = 0; y < k->tl_rows; y++)
        for (int x = 0; x < k->cols; x++)
            k->tl[y * k->cols + x] = decor_pixel(&v, x - E + .5f, y - E + .5f);

    for (int y = 0; y < k->bl_rows; y++)
        for (int x = 0; x < k->cols; x++)
            k->bl[y * k->cols + x] = decor_pixel(&v, x - E + .5f, v.H - R + y + .5f);

    for (int i = 0; i < E; i++) {
        k->top[i] = decor_pixel(&v, v.W / 2, i - E + .5f);
        k->bottom[i] = decor_pixel(&v, v.W / 2, v.H + i + .5f);
        k->side[i] = decor_pixel(&v, i - E + .5f, v.H / 2);
    }

    return k;
}

/* Is the window large enough for the cached patches not to overlap? */
static inline bool
decor_cache_fits(const struct ctx *c, const struct decor_cache *k)
{
    return c->W >= 2 * k->R && c->T + c->H >= 2 * k->R + k->y_ofs;
}

/* Decoration pixel at integer grid pixel (x, y), from the cache */
static inline uint32_t
decor_at(const struct ctx *c, const struct decor_cache *k, int x, int y)
{
    const int W = c->W;
    const int mx = x < W / 2 ? x : W - 1 - x;  /* mirror right half */
    const int ft = y + (int)c->T;              /* rows below frame top */
    const int fb = y - (int)c->H;              /* rows below frame bottom */

    if (mx < k->R) {
        if (ft < k->R + k->y_ofs)
            return k->tl[(ft + k->E) * k->cols + mx + k->E];
        if (fb >= -k->R)
            return k->bl[(fb + k->R) * k->cols + mx + k->E];
        return mx < 0 ? k->side[mx + k->E] : 0;
    }

    if (ft < 0)
        return k->top[ft + k->E];
    if (fb >= 0)
        return k->bottom[fb];
    return 0;
}

static inline void
fill32(uint32_t *dst, uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = v;
}

/*
 * One row of decorations, grid pixels x0..x1 at row y, built from
 * whole spans of the cache: left corner/edge, the (constant) middle,
 * and the mirrored right corner/edge.
 */
static void
decor_row(const struct ctx *c, const struct decor_cache *k,
          uint32_t *row, int x0, int x1, int y)
{
    const int W = c->W, R = k->R, E = k->E, cols = k->cols;
    const int ft = y + (int)c->T;
    const int fb = y - (int)c->H;

    /* Source for the corner columns (grid x -E..R-1) */
    const uint32_t *corner;
    uint32_t mid;

    if (ft < R + k->y_ofs)
        corner = &k->tl[(ft + E) * cols];
    else if (fb >= -R)
        corner = &k->bl[(fb + R) * cols];
    else
        corner = NULL;  /* straight left/right edges */

    mid = ft < 0 ? k->top[ft + E] : fb >= 0 ? k->bottom[fb] : 0;

    for (int x = x0; x < x1;) {
        const int mx = x < W / 2 ? x : W - 1 - x;

        if (mx >= R) {
            /* Middle: up to the right corner zone */
            const int end = MINF(x1, W - R);
            fill32(&row[x - x0], mid, end - x);
            x = end;
        } else if (x < W / 2) {
            /* Left zone, x = -E .. R-1 */
            const int end = MINF(x1, MINF(R, W / 2));
            if (corner != NULL)
                memcpy(&row[x - x0], &corner[x + E], (end - x) * 4);
            else {
                for (int i = x; i < end; i++)
                    row[i - x0] = i < 0 ? k->side[i + E] : 0;
            }
            x = end;
        } else {
            /* Right zone, mirrored */
            for (; x < x1; x++) {
                const int m = W - 1 - x;
                row[x - x0] = corner != NULL ? corner[m + E]
                            : m < 0 ? k->side[m + E] : 0;
            }
        }
    }
}

/* Decoration pixel, cached when possible */
static inline uint32_t
decor(const struct ctx *c, const struct decor_cache *k, int x, int y)
{
    if (k != NULL)
        return decor_at(c, k, x, y);
    return decor_pixel(c, x + .5f, y + .5f);
}

static const struct decor_cache *
decor_cache_for(const struct ctx *c)
{
    if (!c->floating && !c->tiled)
        return NULL;
    const struct decor_cache *k = decor_cache_get(c);
    return decor_cache_fits(c, k) ? k : NULL;
}

void
adw_prewarm(const struct wayland *wayl, const struct config *conf)
{
    /* Compute the shadows for each output scale up front, so that
     * the first window doesn't have to */
    tll_foreach(wayl->monitors, it) {
        const float s = it->item.scale;
        for (int focused = 0; focused < 2; focused++) {
            struct ctx c = {
                .s = s,
                .W = 16384, .H = 16384,
                .T = roundf(conf->csd.title_height * s),
                .R = roundf(15 * s),
                .E = roundf(conf->csd.border_width * s),
                .floating = true,
                .focused = focused,
                .dark = conf->initial_color_theme != COLOR_THEME_LIGHT,
                .shadows = focused ? shadow_focused : shadow_backdrop,
                .shadow_count = focused ? ALEN(shadow_focused) : ALEN(shadow_backdrop),
            };
            decor_cache_get(&c);
        }
    }
}

/*
 * Pixel access. We write premultiplied ARGB8888; if the buffer uses
 * some other format (10/16-bit surfaces), render to a temporary image
 * and let pixman convert.
 */
struct canvas {
    uint32_t *data;
    int stride;  /* in pixels */
    pixman_image_t *tmp;
};

static void
canvas_begin(struct canvas *cv, struct buffer *buf)
{
    if (pixman_image_get_format(buf->pix[0]) == PIXMAN_a8r8g8b8) {
        cv->data = buf->data;
        cv->stride = buf->stride / 4;
        cv->tmp = NULL;
    } else {
        cv->tmp = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8, buf->width, buf->height, NULL, buf->width * 4);
        cv->data = pixman_image_get_data(cv->tmp);
        cv->stride = buf->width;
    }
}

static void
canvas_end(struct canvas *cv, struct buffer *buf)
{
    if (cv->tmp == NULL)
        return;

    pixman_image_composite32(
        PIXMAN_OP_SRC, cv->tmp, NULL, buf->pix[0],
        0, 0, 0, 0, 0, 0, buf->width, buf->height);
    pixman_image_unref(cv->tmp);
    cv->tmp = NULL;
}

static void
set_region(struct terminal *term, struct wl_surface *surf, bool opaque,
           const pixman_box32_t *boxes, size_t count)
{
    struct wl_region *region = wl_compositor_create_region(term->wl->compositor);
    if (region == NULL)
        return;

    for (size_t i = 0; i < count; i++) {
        const pixman_box32_t *b = &boxes[i];
        if (b->x2 > b->x1 && b->y2 > b->y1)
            wl_region_add(region, b->x1, b->y1, b->x2 - b->x1, b->y2 - b->y1);
    }

    if (opaque)
        wl_surface_set_opaque_region(surf, region);
    else
        wl_surface_set_input_region(surf, region);
    wl_region_destroy(region);
}

bool
adw_borders_need_render(struct terminal *term)
{
    struct ctx c;
    ctx_init(&c, term);

    char key[sizeof(term->render.adw_corners.border_key)];
    snprintf(key, sizeof(key), "%.3f %d %d %d %d %d %d %d %d %d",
             c.s, (int)c.W, (int)c.H, (int)c.T, (int)c.R, c.E,
             c.floating, c.tiled, c.focused, c.dark);

    if (strcmp(key, term->render.adw_corners.border_key) == 0)
        return false;

    strcpy(term->render.adw_corners.border_key, key);
    return true;
}

void
adw_render_border(struct terminal *term, enum csd_surface surf_idx,
                  const struct csd_data *info, struct buffer *buf)
{
    struct ctx c;
    ctx_init(&c, term);

    struct canvas cv;
    canvas_begin(&cv, buf);

    const int w = buf->width;
    const int h = buf->height;
    const struct decor_cache *k = decor_cache_for(&c);

    for (int y = 0; y < h; y++) {
        uint32_t *row = &cv.data[y * cv.stride];

        if (!c.floating && !c.tiled)
            memset(row, 0, w * 4);
        else if (k != NULL)
            decor_row(&c, k, row, info->x, info->x + w, info->y + y);
        else {
            for (int x = 0; x < w; x++)
                row[x] = decor_pixel(&c, info->x + x + .5f, info->y + y + .5f);
        }
    }

    canvas_end(&cv, buf);

    /* Only a narrow band next to the frame resizes; the rest of the
     * shadow lets clicks through to whatever is below */
    struct wl_surface *surf = term->window->csd.surface[surf_idx].surface.surf;
    const int bw = term->conf->csd.border_width;
    const int band = ADW_RESIZE_HANDLE;
    const int lw = roundf(w / c.s);
    const int lh = roundf(h / c.s);
    pixman_box32_t box;

    switch (surf_idx) {
    case CSD_SURF_LEFT:   box = (pixman_box32_t){bw - band, 0, bw, lh}; break;
    case CSD_SURF_RIGHT:  box = (pixman_box32_t){0, 0, band, lh}; break;
    case CSD_SURF_TOP:    box = (pixman_box32_t){bw - band, bw - band, lw - bw + band, bw}; break;
    case CSD_SURF_BOTTOM: box = (pixman_box32_t){bw - band, 0, lw - bw + band, band}; break;
    default: box = (pixman_box32_t){0, 0, lw, lh}; break;
    }

    set_region(term, surf, false, &box, 1);
}

static const struct fcft_glyph *
glyph_for(struct fcft_font *font, char32_t wc)
{
    return fcft_rasterize_char_utf32(font, wc, FCFT_SUBPIXEL_NONE);
}

static void
render_title_text(struct terminal *term, const struct ctx *c,
                  struct buffer *buf, pixman_color_t fg)
{
    struct wl_window *win = term->window;
    struct fcft_font *font = win->csd.font;
    if (font == NULL)
        return;

    char32_t *text = ambstoc32(term->window_title);
    if (text == NULL)
        return;

    const size_t len = c32len(text);
    struct fcft_text_run *run = NULL;
    const struct fcft_glyph **glyphs = NULL;
    const struct fcft_glyph *_glyphs[len + 1];
    size_t count = 0;

    if (fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING) {
        run = fcft_rasterize_text_run_utf32(font, len, text, FCFT_SUBPIXEL_NONE);
        if (run != NULL) {
            glyphs = run->glyphs;
            count = run->count;
        }
    }

    if (glyphs == NULL) {
        for (size_t i = 0; i < len; i++) {
            const struct fcft_glyph *g = glyph_for(font, text[i]);
            if (g != NULL)
                _glyphs[count++] = g;
        }
        glyphs = _glyphs;
    }

    /* Title is centered on the window, but kept clear of the buttons */
    const float s = c->s;
    int buttons = 0;
    for (int i = CSD_SURF_MINIMIZE; i <= CSD_SURF_CLOSE; i++)
        buttons += get_csd_data(term, i).width > 0;

    const float pad = 12 * s;  /* .title padding */
    const float left = ADW_HEADER_PADDING_X * s;
    const float right = c->W - (ADW_HEADER_PADDING_X +
                                buttons * ADW_BUTTON_SIZE +
                                (buttons > 0 ? (buttons - 1) * ADW_BUTTON_SPACING + 6 : 0)) * s;
    const float max_text = right - left - 2 * pad;

    int width = 0;
    for (size_t i = 0; i < count; i++)
        width += glyphs[i]->advance.x;

    size_t shown = count;
    const struct fcft_glyph *ellipsis = NULL;
    if (width > max_text) {
        ellipsis = glyph_for(font, U'…');
        const int ell = ellipsis != NULL ? ellipsis->advance.x : 0;
        while (shown > 0 && width + ell > max_text)
            width -= glyphs[--shown]->advance.x;
        width += ell;
    }

    if (max_text > 0 && (shown > 0 || ellipsis != NULL)) {
        const float label = width + 2 * pad;
        const float lx = clampf((c->W - label) / 2, left, fmaxf(left, right - label));
        int x = roundf(lx + pad);

        const float center = (ADW_HEADER_PADDING_Y + ADW_BUTTON_SIZE / 2.f) * s;
        const int y = roundf(center - (font->ascent + font->descent) / 2.f) + font->ascent;

        pixman_image_t *src = pixman_image_create_solid_fill(&fg);

        for (size_t i = 0; i <= shown; i++) {
            const struct fcft_glyph *g = i < shown ? glyphs[i] : ellipsis;
            if (g == NULL)
                continue;

            pixman_image_composite32(
                PIXMAN_OP_OVER, g->is_color_glyph ? g->pix : src,
                g->is_color_glyph ? NULL : g->pix, buf->pix[0],
                0, 0, 0, 0, x + g->x, y - g->y, g->width, g->height);
            x += g->advance.x;
        }

        pixman_image_unref(src);
    }

    fcft_text_run_destroy(run);
    free(text);
}

void
adw_render_title(struct terminal *term, const struct csd_data *info,
                 struct buffer *buf)
{
    struct ctx c;
    ctx_init(&c, term);

    const float s = c.s;
    const int w = buf->width;
    const int h = buf->height;
    const int T = c.T;

    const struct rgba bg_color = c.focused
        ? palette[c.dark].header_bg : palette[c.dark].header_backdrop;
    const uint32_t bg = pack_rgba(bg_color, 1);

    struct canvas cv;
    canvas_begin(&cv, buf);

    const struct decor_cache *k = decor_cache_for(&c);

    /* Header bar, with rounded top corners and the window outline */
    const int ow = c.floating ? ceilf(s) : 0;  /* outline width */
    const int rc = ceilf(c.R);                 /* corner columns */

    for (int y = 0; y < T && y < h; y++) {
        uint32_t *row = &cv.data[y * cv.stride];
        const float py = info->y + y + .5f;

        fill32(row, y < ow ? with_outline(&c, bg, -(y + .5f)) : bg, w);

        if (py < -c.T + c.R) {
            /* Rounded corners */
            for (int x = 0; x < w; x++) {
                if (x == rc && w - rc > rc)
                    x = w - rc;

                const float d = sdf_frame(&c, x + .5f, py);
                const uint32_t v = over(bg, decor(&c, k, x, info->y + y),
                                        clampf(.5f - d, 0, 1));
                row[x] = c.floating ? with_outline(&c, v, d) : v;
            }
        } else {
            /* Outline along the sides */
            for (int x = 0; x < ow; x++) {
                row[x] = with_outline(&c, bg, -(x + .5f));
                row[w - 1 - x] = row[x];
            }
        }
    }

    /* .top-bar.raised: box-shadow: 0 1px shade/2, 0 2px 4px shade/2 */
    struct rgba shade = palette[c.dark].shade;
    shade.a /= 2;
    for (int y = T; y < h; y++) {
        uint32_t *row = &cv.data[y * cv.stride];
        const float dy = y - T + .5f;
        const float line = clampf(s - (dy - .5f), 0, 1);
        const float blur = gauss_edge((dy - 2 * s) / (2 * s));
        const float a = 1 - (1 - line * shade.a) * (1 - blur * shade.a);
        const uint32_t v = pack(shade.r * a, shade.g * a, shade.b * a, a);

        for (int x = 0; x < w; x++)
            row[x] = v;
    }

    canvas_end(&cv, buf);

    const struct rgba fg = palette[c.dark].fg;
    const uint16_t a16 = fg.a * (c.focused ? 1 : .5f) * 0xffff;
    render_title_text(term, &c, buf, (pixman_color_t){
        .red = fg.r * a16, .green = fg.g * a16, .blue = fg.b * a16, .alpha = a16});

    /* Clicks in the shade band go to the terminal */
    struct wl_surface *surf = term->window->csd.surface[CSD_SURF_TITLE].surface.surf;
    const int lw = roundf(w / s);
    const int lt = roundf(T / s);
    const int lr = ceilf(c.R / s);

    set_region(term, surf, false, &(pixman_box32_t){0, 0, lw, lt}, 1);
    set_region(term, surf, true, (pixman_box32_t[]){
            {lr, 0, lw - lr, lr},
            {0, lr, lw, lt},
        }, 2);
}

/* Adwaita symbolic window icons, in 16x16 icon coordinates */
static bool
icon_hit(enum csd_surface surf_idx, bool maximized, float u, float v)
{
    switch (surf_idx) {
    case CSD_SURF_CLOSE:
        return u >= 4 && u <= 12 && v >= 4 && v <= 12 &&
            (fabsf(u - v) <= 1.41f || fabsf(u + v - 16) <= 1.41f);  /* 2px wide arms */

    case CSD_SURF_MINIMIZE:
        return u >= 4 && u <= 12 && v >= 10 && v <= 12;

    case CSD_SURF_MAXIMIZE:
        if (maximized)
            return u >= 5 && u <= 11 && v >= 5 && v <= 11 &&
                !(u > 7 && u < 9 && v > 7 && v < 9);
        return u >= 4 && u <= 12 && v >= 4 && v <= 12 &&
            !(u > 6 && u < 10 && v > 6 && v < 10);

    default:
        return false;
    }
}

/* Rendered buttons, by state; they rarely change */
struct button_image {
    enum csd_surface idx;
    float s;
    int w, h;
    int level;  /* 0: normal, 1: hover, 2: pressed */
    bool dark, maximized, focused;
    uint32_t *pix;
};

static struct button_image button_cache[24];
static size_t button_cache_next;

static void
render_button_pixels(uint32_t *data, int stride, enum csd_surface surf_idx,
                     int w, int h, float s, bool dark, int level, bool maximized,
                     bool focused)
{
    const float cx = w / 2.f, cy = h / 2.f;
    struct rgba fg = palette[dark].fg;
    if (!focused)
        fg.a *= .5f;  /* headerbar:backdrop > windowhandle { filter: opacity(.5) } */

    /* windowcontrols > button > image: 16px icon + 4px padding, round */
    const float radius = 12 * s;
    const float bg_alpha = level == 2 ? .30f : level == 1 ? .15f : .10f;

    enum { SS = 4 };
    const float icon_x = cx - 8 * s, icon_y = cy - 8 * s;

    for (int y = 0; y < h; y++) {
        uint32_t *row = &data[y * stride];

        for (int x = 0; x < w; x++) {
            const float px = x + .5f, py = y + .5f;
            const float dc = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - radius;
            const float circle = clampf(.5f - dc, 0, 1) * bg_alpha;

            float icon = 0;
            if (px >= icon_x - 1 && px <= icon_x + 16 * s + 1 &&
                py >= icon_y - 1 && py <= icon_y + 16 * s + 1)
            {
                int hits = 0;
                for (int sy = 0; sy < SS; sy++) {
                    for (int sx = 0; sx < SS; sx++) {
                        const float u = (x + (sx + .5f) / SS - icon_x) / s;
                        const float v = (y + (sy + .5f) / SS - icon_y) / s;
                        hits += icon_hit(surf_idx, maximized, u, v);
                    }
                }
                icon = (float)hits / (SS * SS);
            }

            row[x] = over(pack_rgba(fg, icon), pack_rgba(fg, circle), 1);
        }
    }
}

void
adw_render_button(struct terminal *term, enum csd_surface surf_idx,
                  struct buffer *buf, bool hover, bool pressed)
{
    const float s = term->scale;
    const int w = buf->width;
    const int h = buf->height;
    const bool dark = term->colors.active_theme != COLOR_THEME_LIGHT;
    const bool focused = term->visual_focus;
    const int level = pressed ? 2 : hover ? 1 : 0;
    const bool maximized = surf_idx == CSD_SURF_MAXIMIZE && term->window->is_maximized;

    struct button_image *img = NULL;
    for (size_t i = 0; i < ALEN(button_cache); i++) {
        struct button_image *b = &button_cache[i];
        if (b->pix != NULL && b->idx == surf_idx && b->s == s &&
            b->w == w && b->h == h && b->level == level &&
            b->dark == dark && b->maximized == maximized &&
            b->focused == focused)
        {
            img = b;
            break;
        }
    }

    if (img == NULL) {
        img = &button_cache[button_cache_next++ % ALEN(button_cache)];
        free(img->pix);
        *img = (struct button_image){
            .idx = surf_idx, .s = s, .w = w, .h = h, .level = level,
            .dark = dark, .maximized = maximized, .focused = focused,
            .pix = xmalloc(w * h * sizeof(uint32_t)),
        };
        render_button_pixels(
            img->pix, w, surf_idx, w, h, s, dark, level, maximized, focused);
    }

    struct canvas cv;
    canvas_begin(&cv, buf);
    for (int y = 0; y < h; y++)
        memcpy(&cv.data[y * cv.stride], &img->pix[y * w], w * 4);
    canvas_end(&cv, buf);
}

/*
 * Rounded bottom corners, and the window outline along the left, right
 * and bottom edges of the grid surface.
 *
 * The grid buffer is re-used between frames, and parts of it are
 * scrolled rather than re-rendered. We therefore stash the pixels we
 * are about to modify, and put them back before the next frame is
 * rendered, so our changes never leak into scrolled content.
 */
static size_t
grid_edge_rects(const struct terminal *term, const struct buffer *buf,
                struct ctx *c, pixman_box32_t rects[5])
{
    if (pixman_image_get_format(buf->pix[0]) != PIXMAN_a8r8g8b8)
        return 0;

    ctx_init(c, term);

    const int W = buf->width, H = buf->height;
    const int R = ceilf(c->R);
    const int ow = ceilf(c->s);

    if (!c->floating || R < ow || 2 * R > W || R > H)
        return 0;

    rects[0] = (pixman_box32_t){0, 0, ow, H - R};           /* left */
    rects[1] = (pixman_box32_t){W - ow, 0, W, H - R};       /* right */
    rects[2] = (pixman_box32_t){R, H - ow, W - R, H};       /* bottom */
    rects[3] = (pixman_box32_t){0, H - R, R, H};            /* corners */
    rects[4] = (pixman_box32_t){W - R, H - R, W, H};
    return 5;
}

static size_t
rects_area(const pixman_box32_t *rects, size_t count)
{
    size_t area = 0;
    for (size_t i = 0; i < count; i++)
        area += (rects[i].x2 - rects[i].x1) * (rects[i].y2 - rects[i].y1);
    return area;
}

/* Copy the pixels in 'rects' between the buffer and the stash */
static void
stash_rects(struct adw_corners *st, struct buffer *buf, bool save)
{
    const int stride = buf->stride / 4;
    uint32_t *data = buf->data;
    uint32_t *p = st->pix;

    for (size_t i = 0; i < st->rect_count; i++) {
        const pixman_box32_t *r = &st->rects[i];
        const int n = r->x2 - r->x1;
        for (int y = r->y1; y < r->y2; y++, p += n) {
            uint32_t *row = &data[y * stride + r->x1];
            if (save)
                memcpy(p, row, n * 4);
            else
                memcpy(row, p, n * 4);
        }
    }
}

void
adw_grid_corners_restore(struct terminal *term, struct buffer *buf)
{
    struct adw_corners *st = &term->render.adw_corners;
    if (st->pix == NULL || st->width != buf->width || st->height != buf->height)
        return;

    stash_rects(st, buf, false);
}

void
adw_grid_corners_apply(struct terminal *term, struct buffer *buf)
{
    struct adw_corners *st = &term->render.adw_corners;
    struct wl_surface *surf = term->window->surface.surf;
    const float s = term->scale;

    struct ctx c;
    pixman_box32_t rects[5];
    const size_t count = grid_edge_rects(term, buf, &c, rects);
    const int r = count > 0 ? ceilf(c.R) : 0;
    const bool opaque = term->colors.alpha == 0xffff;

    /* Opaque region: everything but the corners */
    if (opaque && (st->opaque_key != r ||
                   st->opaque_width != buf->width ||
                   st->opaque_height != buf->height))
    {
        const int lw = roundf(buf->width / s);
        const int lh = roundf(buf->height / s);
        const int lr = ceilf(r / s);

        set_region(term, surf, true, (pixman_box32_t[]){
                {0, 0, lw, lh - lr},
                {lr, lh - lr, lw - lr, lh},
            }, 2);

        st->opaque_key = r;
        st->opaque_width = buf->width;
        st->opaque_height = buf->height;
    }

    if (count == 0) {
        free(st->pix);
        st->pix = NULL;
        return;
    }

    const size_t area = rects_area(rects, count);
    if (st->pix == NULL || st->pix_size < area) {
        free(st->pix);
        st->pix = xmalloc(area * sizeof(st->pix[0]));
        st->pix_size = area;
    }

    memcpy(st->rects, rects, sizeof(rects));
    st->rect_count = count;
    st->width = buf->width;
    st->height = buf->height;
    stash_rects(st, buf, true);

    const struct decor_cache *k = decor_cache_for(&c);
    const int stride = buf->stride / 4;
    uint32_t *data = buf->data;
    const int W = buf->width, H = buf->height;

    /* Straight edges: the outline is constant along each column/row */
    for (int i = 0; i < 3; i++) {
        const pixman_box32_t *b = &rects[i];
        for (int y = b->y1; y < b->y2; y++) {
            uint32_t *row = &data[y * stride];
            for (int x = b->x1; x < b->x2; x++) {
                const float d = i == 2 ? -(H - y - .5f)
                              : -MINF(x + .5f, W - x - .5f);
                row[x] = with_outline(&c, row[x], d);
            }
        }
    }

    /* Rounded corners */
    for (int i = 3; i < 5; i++) {
        const pixman_box32_t *b = &rects[i];
        for (int y = b->y1; y < b->y2; y++) {
            uint32_t *row = &data[y * stride];
            for (int x = b->x1; x < b->x2; x++) {
                const float d = sdf_frame(&c, x + .5f, y + .5f);
                const float cov = clampf(.5f - d, 0, 1);
                uint32_t v = row[x];
                if (cov < 1)
                    v = over(v, decor(&c, k, x, y), cov);
                row[x] = with_outline(&c, v, d);
            }
        }
    }

    for (size_t i = 0; i < count; i++) {
        wl_surface_damage_buffer(
            surf, rects[i].x1, rects[i].y1,
            rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
    }
}

void
adw_grid_corners_reset(struct terminal *term)
{
    struct adw_corners *st = &term->render.adw_corners;
    free(st->pix);
    st->pix = NULL;
    st->opaque_key = -1;
}
