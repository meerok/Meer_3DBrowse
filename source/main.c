/*
 * 3DBrowse by Meer - a fast, text-first web browser for the Nintendo 3DS
 *
 * Layout: the page is one continuous 320px-wide column that runs down the
 * top screen and carries on onto the touch screen, so links on the lower
 * half can be tapped directly. The top screen has a slim title bar, the
 * touch screen a toolbar.
 */
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "html.h"
#include "net.h"
#include "pages.h"
#include "store.h"
#include "url.h"

#define SCR_TOP_W  400
#define SCR_BOT_W  320
#define SCR_H      240
#define TOPBAR_H   22
#define TOOLBAR_H  34
#define TOP_VIEW   (SCR_H - TOPBAR_H)
#define BOT_VIEW   (SCR_H - TOOLBAR_H)
#define VIEW_ALL   (TOP_VIEW + BOT_VIEW)
#define COL_W      320
#define MARGIN     10
#define TOP_COL_X  ((SCR_TOP_W - COL_W) / 2)
#define Z          0.5f

/* the .3dsx main thread gets 32 KB by default; URL handling needs more headroom */
u32 __stacksize__ = 256 * 1024;

/* ------------------------------------------------------------------ theme */

typedef struct {
    u32 bg, ink, muted, hair, bar, accent, link, field, code, focus, btn, btnInk, shade;
} Theme;
static Theme T;

static u32 rgb(u32 c) { return C2D_Color32((c >> 16) & 255, (c >> 8) & 255, c & 255, 255); }
static u32 rgba(u32 c, u8 a) { return C2D_Color32((c >> 16) & 255, (c >> 8) & 255, c & 255, a); }

static void theme_apply(int dark) {
    if (!dark) {
        T.bg = rgb(0xF5F2EB); T.ink = rgb(0x1E1C18); T.muted = rgb(0x857E72); T.hair = rgb(0xDCD5C7);
        T.bar = rgb(0xECE7DC); T.accent = rgb(0xD9482B); T.link = rgb(0x2257B0); T.field = rgb(0xFFFDF8);
        T.code = rgb(0xE8E2D5); T.focus = rgb(0xF5D3C6); T.btn = rgb(0x1E1C18); T.btnInk = rgb(0xF5F2EB);
        T.shade = rgba(0x1E1C18, 70);
    } else {
        T.bg = rgb(0x15140F); T.ink = rgb(0xE8E2D4); T.muted = rgb(0x8C8578); T.hair = rgb(0x2E2C26);
        T.bar = rgb(0x1C1B16); T.accent = rgb(0xFF6A45); T.link = rgb(0x8DB3FF); T.field = rgb(0x201F19);
        T.code = rgb(0x26241E); T.focus = rgb(0x4A2A20); T.btn = rgb(0xE8E2D4); T.btnInk = rgb(0x15140F);
        T.shade = rgba(0x000000, 120);
    }
}

/* ------------------------------------------------------------------ fonts */

static float s_lineFeed = 30;
static float s_ascii[128];
static u8 s_wcache[0x3000];   /* lazily filled widths for U+0080..U+2FFF, 0 = unknown */

static float char_w(u32 cp) {
    if (cp < 128) return s_ascii[cp];
    if (cp < 0x3000 && s_wcache[cp]) return (float)(s_wcache[cp] - 1);
    int gi = fontGlyphIndexFromCodePoint(NULL, cp);
    charWidthInfo_s *wi = fontGetCharWidthInfo(NULL, gi);
    float w = wi ? wi->charWidth : 12;
    if (cp < 0x3000 && w < 250) s_wcache[cp] = (u8)(w + 1);
    return w;
}

static float measure(const char *s, int n, float scale) {
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    float w = 0;
    while (p < e) {
        if (*p < 0x80) { w += s_ascii[*p++]; continue; }
        w += char_w(html_u8dec(&p, e));
    }
    return w * scale;
}

static void fonts_init(void) {
    fontEnsureMapped();
    FINF_s *fi = fontGetInfo(NULL);
    if (fi) s_lineFeed = fi->lineFeed;
    for (int c = 0; c < 128; c++) {
        if (c < 32) { s_ascii[c] = 0; continue; }
        int gi = fontGlyphIndexFromCodePoint(NULL, (u32)c);
        charWidthInfo_s *wi = fontGetCharWidthInfo(NULL, gi);
        s_ascii[c] = wi ? wi->charWidth : 12;
    }
}

static float base_scale(void) {
    static const float S[3] = {0.44f, 0.5f, 0.58f};
    return S[g_cfg.textSize];
}

static LayoutCfg layout_cfg(void) {
    LayoutCfg c = {measure, s_lineFeed, COL_W - 2 * MARGIN, base_scale()};
    return c;
}

/* ------------------------------------------------------------------ state */

static C3D_RenderTarget *s_top, *s_bot;
static C2D_TextBuf s_ui, s_pageBuf;

static Doc *g_doc;
static bool g_docNoCache;
static float g_scroll, g_vel, g_target = -1;
static int g_focus = -1;
static bool g_quit;
static u32 g_frame;

typedef struct { float x, y, w, h; int link, field; } Item;
static Item *s_items;
static int s_nitems, s_citems;

typedef struct { char url[2048]; float scroll; } Hist;
#define MAX_BACK 40
static Hist s_hist[MAX_BACK];
static int s_hn = 0, s_hi = -1;

#define DCACHE 3
static struct { char url[2048]; Doc *doc; } s_dc[DCACHE];

enum { NAV_PUSH, NAV_HIST, NAV_REPLACE };
static bool s_loading;
static char s_loadUrl[2048];
static float s_restore;
static u32 s_refreshAt;
static char s_refreshUrl[2048];

static char s_toast[120];
static int s_toastT;

/* touch */
static bool s_touch, s_drag;
static int s_tx0, s_ty0, s_tlx, s_tly;
static float s_dragV;
static int s_pressBtn = -1;
static int s_pressLink = -1, s_pressField = -1;

/* overlay list (menu and <select> picker) */
enum { LIST_NONE, LIST_MENU, LIST_SELECT };
#define LIST_MAX 256
static struct {
    int kind;
    char title[64];
    int n, sel, top, field;
    char label[LIST_MAX][80];
    char value[LIST_MAX][20];
    int id[LIST_MAX];
} L;

static void toast(const char *msg) {
    snprintf(s_toast, sizeof s_toast, "%s", msg);
    s_toastT = 150;
}

/* ------------------------------------------------------------- drawing kit */

static void text(const char *s, float x, float y, float sc, u32 col, u32 flags) {
    C2D_Text t;
    C2D_TextParse(&t, s_ui, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | flags, x, y, Z, sc, sc, col);
}

static void fit(const char *in, float maxw, float sc, char *out, size_t n) {
    int len = (int)strlen(in);
    if (measure(in, len, sc) <= maxw) { snprintf(out, n, "%s", in); return; }
    float dots = measure("...", 3, sc);
    const unsigned char *p = (const unsigned char *)in, *e = p + len;
    float w = 0;
    size_t o = 0;
    while (p < e) {
        const unsigned char *q = p;
        u32 cp = html_u8dec(&q, e);
        float cw = char_w(cp) * sc;
        if (w + cw + dots > maxw || o + (size_t)(q - p) + 4 >= n) break;
        memcpy(out + o, p, (size_t)(q - p));
        o += (size_t)(q - p);
        w += cw;
        p = q;
    }
    memcpy(out + o, "...", 4);
}

static void rect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, Z, w, h, c); }

static void rrect(float x, float y, float w, float h, float r, u32 c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    rect(x + r, y, w - 2 * r, h, c);
    rect(x, y + r, r, h - 2 * r, c);
    rect(x + w - r, y + r, r, h - 2 * r, c);
    C2D_DrawCircleSolid(x + r, y + r, Z, r, c);
    C2D_DrawCircleSolid(x + w - r, y + r, Z, r, c);
    C2D_DrawCircleSolid(x + r, y + h - r, Z, r, c);
    C2D_DrawCircleSolid(x + w - r, y + h - r, Z, r, c);
}

static void rframe(float x, float y, float w, float h, float r, u32 border, u32 fill) {
    rrect(x, y, w, h, r, border);
    rrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, fill);
}

static void line(float x0, float y0, float x1, float y1, float t, u32 c) { C2D_DrawLine(x0, y0, c, x1, y1, c, t, Z); }

static void icon_chevron(float cx, float cy, int dir, u32 c) {
    float d = 5 * (float)dir;
    line(cx + d * 0.5f, cy - 6, cx - d * 0.5f, cy, 2, c);
    line(cx - d * 0.5f, cy, cx + d * 0.5f, cy + 6, 2, c);
}

static void icon_reload(float cx, float cy, u32 c) {
    const float r = 6.5f;
    float px = 0, py = 0;
    for (int i = 0; i <= 10; i++) {
        float a = -0.6f + (float)i * (4.9f / 10.0f);
        float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
        if (i) line(px, py, x, y, 1.8f, c);
        px = x; py = y;
    }
    float a = -0.6f;
    float ex = cx + cosf(a) * r, ey = cy + sinf(a) * r;
    C2D_DrawTriangle(ex - 4, ey - 1, c, ex + 3, ey - 2, c, ex, ey + 4, c, Z);
}

static void icon_x(float cx, float cy, u32 c) {
    line(cx - 5, cy - 5, cx + 5, cy + 5, 2, c);
    line(cx - 5, cy + 5, cx + 5, cy - 5, 2, c);
}

static void icon_star(float cx, float cy, bool filled, u32 c, u32 bg) {
    float px[10], py[10];
    for (int i = 0; i < 10; i++) {
        float a = -1.5708f + (float)i * 0.6283f;
        float r = (i & 1) ? 3.4f : 8.0f;
        px[i] = cx + cosf(a) * r;
        py[i] = cy + sinf(a) * r + 0.5f;
    }
    for (int i = 0; i < 10; i++) C2D_DrawTriangle(cx, cy + 0.5f, c, px[i], py[i], c, px[(i + 1) % 10], py[(i + 1) % 10], c, Z);
    if (!filled) {
        for (int i = 0; i < 10; i++) {
            float ix = cx + (px[i] - cx) * 0.62f, iy = cy + 0.5f + (py[i] - cy - 0.5f) * 0.62f;
            float jx = cx + (px[(i + 1) % 10] - cx) * 0.62f, jy = cy + 0.5f + (py[(i + 1) % 10] - cy - 0.5f) * 0.62f;
            C2D_DrawTriangle(cx, cy + 0.5f, bg, ix, iy, bg, jx, jy, bg, Z);
        }
    }
}

static void icon_menu(float cx, float cy, u32 c) {
    for (int i = -1; i <= 1; i++) rect(cx - 7, cy + (float)i * 5 - 1, 14, 2, c);
}

static void icon_lock(float x, float y, u32 c, u32 bg) {
    C2D_DrawCircleSolid(x + 4, y + 4, Z, 3.6f, c);
    C2D_DrawCircleSolid(x + 4, y + 4, Z, 2.0f, bg);
    rect(x + 1.6f, y + 4, 4.8f, 2, bg);
    rrect(x, y + 5, 8, 7, 1.5f, c);
}

/* ------------------------------------------------------------ doc helpers */

static float max_scroll(void) {
    if (!g_doc) return 0;
    float m = g_doc->height - VIEW_ALL;
    return m > 0 ? m : 0;
}

static void clamp_scroll(void) {
    float m = max_scroll();
    if (g_scroll > m) { g_scroll = m; g_vel = 0; }
    if (g_scroll < 0) { g_scroll = 0; g_vel = 0; }
}

static int cmp_item(const void *a, const void *b) {
    const Item *x = (const Item *)a, *y = (const Item *)b;
    if (x->y != y->y) return x->y < y->y ? -1 : 1;
    return x->x < y->x ? -1 : (x->x > y->x);
}

static bool add_item(Item it) {
    if (s_nitems >= s_citems) {
        int nc = s_citems ? s_citems * 2 : 256;
        Item *ni = (Item *)realloc(s_items, sizeof(Item) * (size_t)nc);
        if (!ni) return false;
        s_items = ni;
        s_citems = nc;
    }
    s_items[s_nitems++] = it;
    return true;
}

static void build_items(void) {
    s_nitems = 0;
    g_focus = -1;
    if (!g_doc) return;
    Doc *d = g_doc;
    int lastLink = -1;
    for (int i = 0; i < d->nruns; i++) {
        Run *r = &d->runs[i];
        if (r->link < 0) { lastLink = -1; continue; }
        if (r->link == lastLink && s_nitems) {
            Item *it = &s_items[s_nitems - 1];
            float x0 = fminf(it->x, r->x), y0 = fminf(it->y, r->y);
            float x1 = fmaxf(it->x + it->w, r->x + r->w), y1 = fmaxf(it->y + it->h, r->y + r->h);
            it->x = x0; it->y = y0; it->w = x1 - x0; it->h = y1 - y0;
            continue;
        }
        Item it = {r->x, r->y, r->w, r->h, r->link, -1};
        if (!add_item(it)) return;
        lastLink = r->link;
    }
    for (int i = 0; i < d->nboxes; i++) {
        Box *b = &d->boxes[i];
        if (b->kind != BX_FIELD || b->ref < 0) continue;
        Item it = {b->x, b->y, b->w, b->h, -1, b->ref};
        if (!add_item(it)) return;
    }
    qsort(s_items, (size_t)s_nitems, sizeof(Item), cmp_item);
}

/* visible text cache: parsed C2D_Text objects for the runs around the viewport */
#define TCACHE 2600
#define PAGE_GLYPHS 9000
static C2D_Text s_ptxt[TCACHE];
static int s_cFirst, s_cCount;
static float s_cTop = 1, s_cBot = 0;
static Doc *s_cDoc;
static int s_cGen, s_docGen;

static void cache_invalidate(void) { s_cDoc = NULL; s_docGen++; }

static int first_run_from(float y) {
    Doc *d = g_doc;
    int lo = 0, hi = d->nruns;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (d->runs[mid].y + d->runs[mid].h < y) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void cache_ensure(float top, float bot) {
    if (!g_doc) return;
    if (s_cDoc == g_doc && s_cGen == s_docGen && top >= s_cTop && bot <= s_cBot) return;
    Doc *d = g_doc;
    float t = top - 200, b = bot + 260;
    C2D_TextBufClear(s_pageBuf);
    int first = first_run_from(t);
    int i = first, c = 0;
    float lim = b;
    while (i < d->nruns && c < TCACHE) {
        Run *r = &d->runs[i];
        if (r->y > b) break;
        if (C2D_TextBufGetNumGlyphs(s_pageBuf) + r->len + 8 > PAGE_GLYPHS) { lim = r->y; break; }
        C2D_TextParse(&s_ptxt[c], s_pageBuf, doc_text(d, r));
        C2D_TextOptimize(&s_ptxt[c]);
        i++;
        c++;
    }
    if (c == TCACHE && i < d->nruns) lim = d->runs[i].y;
    s_cFirst = first;
    s_cCount = c;
    s_cTop = t;
    s_cBot = lim < bot ? bot : lim;   /* never rebuild every frame even if the buffer is full */
    s_cDoc = g_doc;
    s_cGen = s_docGen;
}

/* ------------------------------------------------------------- navigation */

static void dc_clear(void) {
    for (int i = 0; i < DCACHE; i++) { doc_free(s_dc[i].doc); s_dc[i].doc = NULL; s_dc[i].url[0] = 0; }
}

static void dc_put(Doc *d) {
    doc_free(s_dc[DCACHE - 1].doc);
    memmove(&s_dc[1], &s_dc[0], sizeof s_dc[0] * (DCACHE - 1));
    free(d->raw);   /* the raw HTML is only needed to re-flow the current page */
    d->raw = NULL;
    d->rawLen = 0;
    snprintf(s_dc[0].url, sizeof s_dc[0].url, "%s", d->url);
    s_dc[0].doc = d;
}

static Doc *dc_take(const char *url) {
    for (int i = 0; i < DCACHE; i++) {
        if (s_dc[i].doc && !strcmp(s_dc[i].url, url)) {
            Doc *d = s_dc[i].doc;
            memmove(&s_dc[i], &s_dc[i + 1], sizeof s_dc[0] * (size_t)(DCACHE - 1 - i));
            s_dc[DCACHE - 1].doc = NULL;
            s_dc[DCACHE - 1].url[0] = 0;
            return d;
        }
    }
    return NULL;
}

static void doc_base(const Doc *d, char *out, size_t n) {
    if (d->base[0]) url_resolve(d->url, d->base, out, n);
    else snprintf(out, n, "%s", d->url);
}

static void install(Doc *d, int kind, int target, float restore, bool noCache) {
    if (s_hi >= 0) s_hist[s_hi].scroll = g_scroll;
    if (g_doc) {
        if (!g_docNoCache && g_doc->status > 0 && !url_is_internal(g_doc->url) && strcmp(g_doc->url, d->url))
            dc_put(g_doc);
        else doc_free(g_doc);
    }
    g_doc = d;
    g_docNoCache = noCache;
    s_pressLink = s_pressField = -1;
    if (L.kind == LIST_SELECT) L.kind = LIST_NONE;

    if (kind == NAV_PUSH || s_hi < 0) {
        if (s_hi >= 0 && !strcmp(s_hist[s_hi].url, d->url)) {
            /* same page again (reload-like): keep one entry */
        } else {
            s_hi++;
            if (s_hi >= MAX_BACK) {
                memmove(&s_hist[0], &s_hist[1], sizeof(Hist) * (MAX_BACK - 1));
                s_hi = MAX_BACK - 1;
            }
        }
        s_hn = s_hi + 1;
    } else if (kind == NAV_HIST) {
        if (target >= 0 && target < s_hn) s_hi = target;
    }
    snprintf(s_hist[s_hi].url, sizeof s_hist[s_hi].url, "%s", d->url);
    s_hist[s_hi].scroll = 0;

    g_scroll = restore;
    g_vel = 0;
    g_target = -1;
    clamp_scroll();
    cache_invalidate();
    build_items();
    s_refreshAt = 0;

    if (d->status > 0 && !url_is_internal(d->url)) {
        char host[128];
        url_host(d->url, host, sizeof host);
        hist_add(d->url, d->title[0] ? d->title : host);
    }
    if (d->refresh[0] && d->refreshDelay <= 5 && d->status > 0) {
        char base[2048];
        doc_base(d, base, sizeof base);
        url_resolve(base, d->refresh, s_refreshUrl, sizeof s_refreshUrl);
        if (strcmp(s_refreshUrl, d->url)) s_refreshAt = g_frame + (u32)(d->refreshDelay * 60) + 1;
    }
    if (d->truncated) toast("Page was very large - showing the first 3 MB");
    else if (d->oom) toast("Page was too long - showing what fits");
}

static Doc *layout_html(char *html, const char *url, int status) {
    Doc *d = doc_new();
    if (!d) { free(html); return NULL; }
    d->raw = html;
    d->rawLen = html ? strlen(html) : 0;
    LayoutCfg c = layout_cfg();
    if (html) html_layout(d, html, d->rawLen, false, &c);
    snprintf(d->url, sizeof d->url, "%s", url);
    d->status = status;
    return d;
}

static void navigate(const char *urlIn, const char *post, int kind, int target, float restore, bool retryHttp) {
    char url[2048];
    snprintf(url, sizeof url, "%s", urlIn);
    s_refreshAt = 0;

    if (url_is_internal(url)) {
        if (s_loading) { net_cancel(); s_loading = false; }
        char *html = page_internal(url);
        char clean[64];
        snprintf(clean, sizeof clean, "%s", url);
        char *q = strchr(clean, '?');
        if (q) *q = 0;
        Doc *d = layout_html(html, clean, 0);
        if (d) install(d, kind, target, restore, true);
        return;
    }
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) {
        char sch[24];
        size_t i = 0;
        while (url[i] && url[i] != ':' && i < sizeof sch - 1) { sch[i] = url[i]; i++; }
        sch[i] = 0;
        char m[80];
        snprintf(m, sizeof m, "Can't open %s: links", sch);
        toast(m);
        return;
    }
    if (kind == NAV_HIST && !post) {
        Doc *c = dc_take(url);
        if (c) {
            if (s_loading) { net_cancel(); s_loading = false; }
            install(c, kind, target, restore, false);
            return;
        }
    }
    LayoutCfg c = layout_cfg();
    int tag = kind | ((target & 0xFF) << 4) | (post ? 0x1000 : 0);
    net_fetch(url, post, g_doc && !url_is_internal(g_doc->url) ? g_doc->url : NULL, &c, retryHttp, tag);
    s_loading = true;
    snprintf(s_loadUrl, sizeof s_loadUrl, "%s", url);
    s_restore = restore;
}

static void go_back(void) {
    if (s_hi <= 0) { toast("No previous page"); return; }
    if (s_loading) { net_cancel(); s_loading = false; }
    s_hist[s_hi].scroll = g_scroll;
    navigate(s_hist[s_hi - 1].url, NULL, NAV_HIST, s_hi - 1, s_hist[s_hi - 1].scroll, false);
}

static void go_forward(void) {
    if (s_hi + 1 >= s_hn) { toast("No next page"); return; }
    if (s_loading) { net_cancel(); s_loading = false; }
    s_hist[s_hi].scroll = g_scroll;
    navigate(s_hist[s_hi + 1].url, NULL, NAV_HIST, s_hi + 1, s_hist[s_hi + 1].scroll, false);
}

static void reload(void) {
    if (!g_doc) return;
    if (s_loading) { net_cancel(); s_loading = false; return; }
    navigate(g_doc->url, NULL, NAV_REPLACE, s_hi, g_scroll, false);
}

static void poll_net(void) {
    NetResult r;
    if (!net_take(&r)) return;
    if (!s_loading) { doc_free(r.doc); return; }
    s_loading = false;
    int kind = r.tag & 0xF;
    int target = (r.tag >> 4) & 0xFF;
    bool post = (r.tag & 0x1000) != 0;
    if (r.doc) {
        install(r.doc, kind, target, kind == NAV_PUSH ? 0 : s_restore, post);
    } else {
        Doc *d = layout_html(page_error(r.url, r.errTitle, r.err), r.url, -1);
        if (d) install(d, kind, target, 0, true);
    }
}

/* --------------------------------------------------------------- keyboard */

static bool keyboard(const char *hint, const char *init, char *out, size_t n, bool password, bool multi, const char *ok) {
    SwkbdState kb;
    int maxLen = (int)n - 1;
    if (maxLen > 2000) maxLen = 2000;
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, maxLen);
    swkbdSetHintText(&kb, hint);
    swkbdSetInitialText(&kb, init);
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, ok, true);
    swkbdSetValidation(&kb, SWKBD_ANYTHING, 0, 0);
    u32 feat = SWKBD_DARKEN_TOP_SCREEN;
    if (multi) feat |= SWKBD_MULTILINE;
    swkbdSetFeatures(&kb, feat);
    if (password) swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    out[0] = 0;
    SwkbdButton b = swkbdInputText(&kb, out, n);
    return b == SWKBD_BUTTON_RIGHT;
}

static void address_bar(void) {
    char in[1024];
    const char *cur = g_doc && !url_is_internal(g_doc->url) ? g_doc->url : "";
    if (!keyboard("Search or type a web address", cur, in, sizeof in, false, false, "Go")) return;
    if (!in[0]) return;
    char url[2048];
    bool typedScheme = true;
    url_from_input(in, url, sizeof url, &typedScheme);
    navigate(url, NULL, NAV_PUSH, 0, 0, !typedScheme);
}

/* ------------------------------------------------------------------ forms */

typedef struct { char *s; size_t n, cap; } SB;

static void sb_add(SB *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->n + l + 1) nc *= 2;
        char *ns = (char *)realloc(b->s, nc);
        if (!ns) return;
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, l + 1);
    b->n += l;
}

static void sb_pair(SB *b, const char *k, const char *v) {
    char *ek = (char *)malloc(strlen(k) * 3 + 1);
    char *ev = (char *)malloc(strlen(v) * 3 + 7);
    if (!ek || !ev) { free(ek); free(ev); return; }
    url_encode(k, ek, strlen(k) * 3 + 1);
    url_encode(v, ev, strlen(v) * 3 + 7);
    if (b->n) sb_add(b, "&");
    sb_add(b, ek);
    sb_add(b, "=");
    sb_add(b, ev);
    free(ek);
    free(ev);
}

static void submit_form(int form, int submitter) {
    Doc *d = g_doc;
    SB b = {0};
    sb_add(&b, "");
    for (int i = 0; i < d->nfields; i++) {
        Field *f = &d->fields[i];
        if (f->form != form) continue;
        const char *name = doc_str(d, f->name);
        if (!name[0]) continue;
        switch (f->type) {
            case F_TEXT: case F_PASSWORD: case F_HIDDEN: case F_TEXTAREA: sb_pair(&b, name, f->value); break;
            case F_CHECK: case F_RADIO: if (f->checked) sb_pair(&b, name, f->value[0] ? f->value : "on"); break;
            case F_SELECT: if (f->nopt) sb_pair(&b, name, doc_str(d, f->optv[f->sel])); break;
            case F_SUBMIT: if (i == submitter) sb_pair(&b, name, f->value); break;
            default: break;
        }
    }
    if (!b.s) return;
    char base[2048], action[2048];
    doc_base(d, base, sizeof base);
    const char *act = (form >= 0 && form < d->nforms) ? doc_str(d, d->forms[form].action) : "";
    bool post = form >= 0 && form < d->nforms && d->forms[form].post;
    url_resolve(base, act, action, sizeof action);
    if (post) {
        navigate(action, b.s, NAV_PUSH, 0, 0, false);
    } else {
        char *q = strchr(action, '?');
        if (q) *q = 0;
        char *u = (char *)malloc(strlen(action) + b.n + 2);
        if (u) {
            sprintf(u, "%s?%s", action, b.s);
            navigate(u, NULL, NAV_PUSH, 0, 0, false);
            free(u);
        }
    }
    free(b.s);
}

static int count_text_fields(int form) {
    int n = 0;
    for (int i = 0; i < g_doc->nfields; i++) {
        Field *f = &g_doc->fields[i];
        if (f->form == form && (f->type == F_TEXT || f->type == F_PASSWORD || f->type == F_TEXTAREA)) n++;
    }
    return n;
}

static void open_select(int fi);

static void activate_field(int fi) {
    Doc *d = g_doc;
    if (fi < 0 || fi >= d->nfields) return;
    Field *f = &d->fields[fi];
    switch (f->type) {
        case F_TEXT: case F_PASSWORD: case F_TEXTAREA: {
            char buf[2048];
            const char *ph = doc_str(d, f->placeholder);
            bool single = f->type != F_TEXTAREA && count_text_fields(f->form) == 1;
            if (!keyboard(ph[0] ? ph : "Type here", f->value, buf, sizeof buf, f->type == F_PASSWORD,
                          f->type == F_TEXTAREA, single ? "Go" : "OK"))
                return;
            char *nv = strdup(buf);
            if (!nv) return;
            free(f->value);
            f->value = nv;
            if (single && f->form >= 0) submit_form(f->form, -1);
            break;
        }
        case F_SUBMIT: submit_form(f->form, fi); break;
        case F_CHECK: f->checked = !f->checked; break;
        case F_RADIO: {
            const char *nm = doc_str(d, f->name);
            for (int i = 0; i < d->nfields; i++) {
                Field *o = &d->fields[i];
                if (o->type == F_RADIO && o->form == f->form && !strcmp(doc_str(d, o->name), nm)) o->checked = false;
            }
            f->checked = true;
            break;
        }
        case F_SELECT: open_select(fi); break;
        case F_BUTTON: toast("This button needs JavaScript"); break;
        default: break;
    }
}

static void open_link(int li) {
    Doc *d = g_doc;
    if (li < 0 || li >= d->nlinks) return;
    const char *href = doc_str(d, d->links[li]);
    if (href[0] == '#') {
        if (!href[1] || !strcasecmp(href, "#top")) { g_target = 0; return; }
        toast("Link to a spot on this page");
        return;
    }
    char base[2048], url[2048];
    doc_base(d, base, sizeof base);
    url_resolve(base, href, url, sizeof url);
    navigate(url, NULL, NAV_PUSH, 0, 0, false);
}

static void activate_item(int idx) {
    if (idx < 0 || idx >= s_nitems) return;
    if (s_items[idx].link >= 0) open_link(s_items[idx].link);
    else activate_field(s_items[idx].field);
}

/* ------------------------------------------------------------- list overlay */

enum { M_HOME, M_BOOKMARKS, M_HISTORY, M_BOOKMARK, M_READER, M_SIZE, M_THEME, M_TLS, M_CLEAR, M_ABOUT, M_EXIT };

static void list_add(const char *label, const char *value, int id) {
    if (L.n >= LIST_MAX) return;
    snprintf(L.label[L.n], sizeof L.label[0], "%s", label);
    snprintf(L.value[L.n], sizeof L.value[0], "%s", value ? value : "");
    L.id[L.n] = id;
    L.n++;
}

static bool page_is_web(void) { return g_doc && g_doc->status > 0 && !url_is_internal(g_doc->url); }

static void build_menu(void) {
    static const char *SZ[3] = {"Small", "Medium", "Large"};
    int keepSel = L.kind == LIST_MENU ? L.sel : 0;
    int keepTop = L.kind == LIST_MENU ? L.top : 0;
    L.kind = LIST_MENU;
    L.n = 0;
    snprintf(L.title, sizeof L.title, "Menu");
    list_add("Start page", NULL, M_HOME);
    list_add("Bookmarks", NULL, M_BOOKMARKS);
    list_add("History", NULL, M_HISTORY);
    if (page_is_web()) {
        list_add(bm_find(g_doc->url) >= 0 ? "Remove bookmark" : "Bookmark this page", NULL, M_BOOKMARK);
        list_add("Reader view", "FrogFind", M_READER);
    }
    list_add("Text size", SZ[g_cfg.textSize], M_SIZE);
    list_add("Theme", g_cfg.theme ? "Dark" : "Light", M_THEME);
    list_add("Check HTTPS certificates", g_cfg.verify ? "On" : "Off", M_TLS);
    list_add("Clear history and cookies", NULL, M_CLEAR);
    list_add("About 3DBrowse", NULL, M_ABOUT);
    list_add("Exit", NULL, M_EXIT);
    L.sel = keepSel < L.n ? keepSel : 0;
    L.top = keepTop;
}

static void open_select(int fi) {
    Field *f = &g_doc->fields[fi];
    if (!f->nopt) return;
    L.kind = LIST_SELECT;
    L.n = 0;
    L.field = fi;
    snprintf(L.title, sizeof L.title, "Choose an option");
    for (int i = 0; i < f->nopt && i < LIST_MAX; i++) list_add(doc_str(g_doc, f->optl[i]), i == f->sel ? "*" : NULL, i);
    L.sel = f->sel < L.n ? f->sel : 0;
    L.top = L.sel > 4 ? L.sel - 4 : 0;
}

static void relayout(void) {
    dc_clear();
    if (!g_doc) return;
    if (!g_doc->raw) { reload(); return; }
    float ratio = g_doc->height > 0 ? g_scroll / g_doc->height : 0;
    LayoutCfg c = layout_cfg();
    doc_clear_layout(g_doc);
    html_layout(g_doc, g_doc->raw, g_doc->rawLen, g_doc->plain, &c);
    g_scroll = ratio * g_doc->height;
    clamp_scroll();
    cache_invalidate();
    build_items();
}

static void menu_choose(int id) {
    switch (id) {
        case M_HOME: L.kind = LIST_NONE; navigate("about:home", NULL, NAV_PUSH, 0, 0, false); break;
        case M_BOOKMARKS: L.kind = LIST_NONE; navigate("about:bookmarks", NULL, NAV_PUSH, 0, 0, false); break;
        case M_HISTORY: L.kind = LIST_NONE; navigate("about:history", NULL, NAV_PUSH, 0, 0, false); break;
        case M_BOOKMARK:
            if (page_is_web()) toast(bm_toggle(g_doc->url, g_doc->title) ? "Bookmarked" : "Bookmark removed");
            build_menu();
            break;
        case M_READER:
            if (page_is_web()) {
                char enc[4096], u[4200];
                url_encode(g_doc->url, enc, sizeof enc);
                snprintf(u, sizeof u, "http://frogfind.com/read.php?a=%s", enc);
                L.kind = LIST_NONE;
                navigate(u, NULL, NAV_PUSH, 0, 0, false);
            }
            break;
        case M_SIZE:
            g_cfg.textSize = (g_cfg.textSize + 1) % 3;
            settings_save();
            relayout();
            build_menu();
            break;
        case M_THEME:
            g_cfg.theme = !g_cfg.theme;
            theme_apply(g_cfg.theme);
            settings_save();
            build_menu();
            break;
        case M_TLS:
            g_cfg.verify = !g_cfg.verify;
            net_set_verify(g_cfg.verify);
            settings_save();
            build_menu();
            toast(g_cfg.verify ? "Certificates will be checked" : "Certificate checks off - less secure");
            break;
        case M_CLEAR:
            hist_clear();
            net_clear_cookies();
            toast("History and cookies cleared");
            build_menu();
            break;
        case M_ABOUT: L.kind = LIST_NONE; navigate("about:about", NULL, NAV_PUSH, 0, 0, false); break;
        case M_EXIT: g_quit = true; break;
        default: break;
    }
}

#define LIST_HEAD 26
#define LIST_ROW  21
#define LIST_ROWS ((SCR_H - LIST_HEAD) / LIST_ROW)

static void list_choose(int i) {
    if (i < 0 || i >= L.n) return;
    if (L.kind == LIST_MENU) { menu_choose(L.id[i]); return; }
    if (L.kind == LIST_SELECT && g_doc && L.field < g_doc->nfields) {
        g_doc->fields[L.field].sel = L.id[i];
        L.kind = LIST_NONE;
    }
}

static void list_keep_visible(void) {
    if (L.sel < L.top) L.top = L.sel;
    if (L.sel >= L.top + LIST_ROWS) L.top = L.sel - LIST_ROWS + 1;
    if (L.top < 0) L.top = 0;
}

static void list_input(u32 down, u32 rep, touchPosition tp) {
    if (rep & KEY_DUP) { L.sel = (L.sel + L.n - 1) % L.n; list_keep_visible(); }
    if (rep & KEY_DDOWN) { L.sel = (L.sel + 1) % L.n; list_keep_visible(); }
    if (rep & KEY_DLEFT) { L.sel -= LIST_ROWS; if (L.sel < 0) L.sel = 0; list_keep_visible(); }
    if (rep & KEY_DRIGHT) { L.sel += LIST_ROWS; if (L.sel >= L.n) L.sel = L.n - 1; list_keep_visible(); }
    if (down & KEY_A) list_choose(L.sel);
    if (down & (KEY_B | KEY_X)) L.kind = LIST_NONE;
    if (down & KEY_TOUCH) {
        if (tp.py < LIST_HEAD) { if (tp.px > SCR_BOT_W - 50) L.kind = LIST_NONE; return; }
        int row = L.top + (tp.py - LIST_HEAD) / LIST_ROW;
        if (row >= 0 && row < L.n) { L.sel = row; list_choose(row); }
    }
    circlePosition cp;
    hidCircleRead(&cp);
    static int acc;
    if (abs(cp.dy) > 40) {
        acc += abs(cp.dy);
        if (acc > 600) {
            acc = 0;
            L.sel += cp.dy < 0 ? 1 : -1;
            if (L.sel < 0) L.sel = 0;
            if (L.sel >= L.n) L.sel = L.n - 1;
            list_keep_visible();
        }
    } else acc = 0;
}

static void draw_list(void) {
    rect(0, 0, SCR_BOT_W, SCR_H, T.bg);
    rect(0, 0, SCR_BOT_W, LIST_HEAD, T.bar);
    rect(0, LIST_HEAD - 1, SCR_BOT_W, 1, T.hair);
    rect(12, 8, 3, 10, T.accent);
    text(L.title, 21, 5, 0.55f, T.ink, 0);
    text("Close", SCR_BOT_W - 12, 6, 0.48f, T.muted, C2D_AlignRight);
    for (int r = 0; r < LIST_ROWS && L.top + r < L.n; r++) {
        int i = L.top + r;
        float y = LIST_HEAD + (float)r * LIST_ROW;
        bool sel = i == L.sel;
        if (sel) { rect(0, y, SCR_BOT_W, LIST_ROW, T.focus); rect(0, y, 3, LIST_ROW, T.accent); }
        char lab[96];
        float vw = L.value[i][0] ? measure(L.value[i], (int)strlen(L.value[i]), 0.48f) + 24 : 12;
        fit(L.label[i], SCR_BOT_W - 16 - vw, 0.5f, lab, sizeof lab);
        u32 col = (L.kind == LIST_MENU && L.id[i] == M_EXIT) ? T.accent : T.ink;
        text(lab, 14, y + 3, 0.5f, col, 0);
        if (L.value[i][0]) {
            if (!strcmp(L.value[i], "*")) C2D_DrawCircleSolid(SCR_BOT_W - 18, y + LIST_ROW / 2.0f, Z, 3.5f, T.accent);
            else text(L.value[i], SCR_BOT_W - 12, y + 4, 0.48f, T.muted, C2D_AlignRight);
        }
        if (!sel && r < LIST_ROWS - 1) rect(14, y + LIST_ROW - 1, SCR_BOT_W - 28, 1, T.hair);
    }
    if (L.n > LIST_ROWS) {
        float th = (float)(SCR_H - LIST_HEAD) * LIST_ROWS / (float)L.n;
        float ty = LIST_HEAD + (float)(SCR_H - LIST_HEAD - th) * (float)L.top / (float)(L.n - LIST_ROWS);
        rect(SCR_BOT_W - 3, ty, 2, th, T.muted);
    }
}

/* ------------------------------------------------------------ page drawing */

static void draw_field(const Box *b, float sx, float sy, bool focused) {
    Doc *d = g_doc;
    if (b->ref < 0 || b->ref >= d->nfields) return;
    Field *f = &d->fields[b->ref];
    float sc = base_scale();
    float th = s_lineFeed * sc;
    float ty = sy + (b->h - th) * 0.5f;
    u32 border = focused ? T.accent : T.hair;
    char buf[160];
    switch (f->type) {
        case F_SUBMIT: case F_BUTTON: {
            rrect(sx, sy, b->w, b->h, 5, focused ? T.accent : T.btn);
            fit(doc_str(d, f->label), b->w - 12, sc, buf, sizeof buf);
            text(buf, sx + b->w / 2, ty, sc, T.btnInk, C2D_AlignCenter);
            break;
        }
        case F_CHECK:
            rframe(sx, sy, b->w, b->h, 3, f->checked ? T.accent : border, f->checked ? T.accent : T.field);
            if (f->checked) {
                line(sx + b->w * 0.22f, sy + b->h * 0.52f, sx + b->w * 0.42f, sy + b->h * 0.72f, 2, T.btnInk);
                line(sx + b->w * 0.42f, sy + b->h * 0.72f, sx + b->w * 0.78f, sy + b->h * 0.3f, 2, T.btnInk);
            }
            break;
        case F_RADIO: {
            float r = b->w / 2;
            C2D_DrawCircleSolid(sx + r, sy + r, Z, r, focused || f->checked ? T.accent : T.hair);
            C2D_DrawCircleSolid(sx + r, sy + r, Z, r - 1.2f, T.field);
            if (f->checked) C2D_DrawCircleSolid(sx + r, sy + r, Z, r * 0.5f, T.accent);
            break;
        }
        case F_SELECT: {
            rframe(sx, sy, b->w, b->h, 5, border, T.field);
            const char *lab = f->nopt ? doc_str(d, f->optl[f->sel]) : "";
            fit(lab, b->w - 26, sc, buf, sizeof buf);
            text(buf, sx + 7, ty, sc, T.ink, 0);
            float cx = sx + b->w - 11, cy = sy + b->h / 2;
            C2D_DrawTriangle(cx - 4, cy - 2, T.muted, cx + 4, cy - 2, T.muted, cx, cy + 3, T.muted, Z);
            break;
        }
        default: {   /* text, password, textarea */
            rframe(sx, sy, b->w, b->h, 5, border, T.field);
            if (f->value[0]) {
                if (f->type == F_PASSWORD) {
                    size_t n = strlen(f->value);
                    if (n > 40) n = 40;
                    for (size_t i = 0; i < n; i++) C2D_DrawCircleSolid(sx + 10 + (float)i * 8, sy + b->h / 2, Z, 2.4f, T.ink);
                } else {
                    char one[512];
                    snprintf(one, sizeof one, "%s", f->value);
                    for (char *p = one; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
                    fit(one, b->w - 14, sc, buf, sizeof buf);
                    text(buf, sx + 7, ty, sc, T.ink, 0);
                }
            } else {
                const char *ph = doc_str(d, f->placeholder);
                if (ph[0]) { fit(ph, b->w - 14, sc, buf, sizeof buf); text(buf, sx + 7, ty, sc, T.muted, 0); }
            }
            if (focused) rect(sx + 3, sy + b->h - 3, b->w - 6, 2, T.accent);
            break;
        }
    }
}

static int focused_link(void) { return (g_focus >= 0 && g_focus < s_nitems) ? s_items[g_focus].link : -1; }
static int focused_field(void) { return (g_focus >= 0 && g_focus < s_nitems) ? s_items[g_focus].field : -1; }

/* draws the slice of the page [viewTop, viewTop+viewH) at screen (offX, offY) */
static void draw_page(float viewTop, float viewH, float offX, float offY) {
    Doc *d = g_doc;
    if (!d) return;
    float x0 = offX + MARGIN;
    int fl = focused_link(), ff = focused_field();
    int pl = s_pressLink, pf = s_pressField;

    /* boxes first (quote bars, rules, bullets, fields) */
    for (int i = 0; i < d->nboxes; i++) {
        Box *b = &d->boxes[i];
        if (b->y + b->h < viewTop || b->y > viewTop + viewH) continue;
        float sx = x0 + b->x, sy = offY + b->y - viewTop;
        switch (b->kind) {
            case BX_HR: rect(sx, sy, b->w, 1, T.hair); break;
            case BX_QUOTE: rect(sx, sy, b->w, b->h, T.accent); break;
            case BX_BULLET:
                if (b->ref == 0) C2D_DrawCircleSolid(sx + b->w / 2, sy + b->h / 2, Z, b->w / 2, T.accent);
                else rect(sx, sy, b->w, b->h, T.muted);
                break;
            case BX_FIELD: draw_field(b, sx, sy, b->ref == ff || b->ref == pf); break;
            default: break;
        }
    }

    for (int c = 0; c < s_cCount; c++) {
        Run *r = &d->runs[s_cFirst + c];
        if (r->y + r->h < viewTop) continue;
        if (r->y > viewTop + viewH) break;
        float sx = x0 + r->x, sy = offY + r->y - viewTop;
        u32 col = T.ink;
        if (r->style & RS_LINK) col = T.link;
        else if (r->style & RS_MUTED) col = T.muted;
        bool hot = r->link >= 0 && (r->link == fl || r->link == pl);
        if (hot) rrect(sx - 3, sy - 1, r->w + 6, r->h + 2, 4, T.focus);
        else if (r->style & RS_MONO) rect(sx - 2, sy, r->w + 4, r->h, T.code);
        if (r->style & RS_HEAD) col = (r->style & RS_LINK) ? T.link : T.ink;
        C2D_DrawText(&s_ptxt[c], C2D_WithColor, sx, sy, Z, r->scale, r->scale, col);
        if (r->style & RS_BOLD) C2D_DrawText(&s_ptxt[c], C2D_WithColor, sx + 0.6f, sy, Z, r->scale, r->scale, col);
        if (r->style & RS_ITALIC) { /* the system font has no italic: a hairline under the run marks emphasis */
            if (!(r->style & (RS_LINK | RS_UNDER))) rect(sx, sy + r->h - 1.5f, r->w, 0.6f, rgba(0x888888, 90));
        }
        if (r->style & (RS_LINK | RS_UNDER)) rect(sx, sy + r->h - 2, r->w, hot ? 1.4f : 0.8f, hot ? T.accent : col);
        if (r->style & RS_STRIKE) rect(sx, sy + r->h * 0.55f, r->w, 1, col);
    }
}

static void draw_top(void) {
    float viewTop = g_scroll;
    draw_page(viewTop, TOP_VIEW, TOP_COL_X, TOPBAR_H);

    /* scroll position */
    if (g_doc && g_doc->height > VIEW_ALL) {
        float track = TOP_VIEW - 8;
        float th = track * VIEW_ALL / g_doc->height;
        if (th < 14) th = 14;
        float ty = TOPBAR_H + 4 + (track - th) * (g_scroll / max_scroll());
        rrect(SCR_TOP_W - 6, ty, 3, th, 1.5f, T.hair);
    }

    /* title bar */
    rect(0, 0, SCR_TOP_W, TOPBAR_H, T.bar);
    rect(0, TOPBAR_H - 1, SCR_TOP_W, 1, T.hair);
    float x = 10;
    const char *u = s_loading ? s_loadUrl : (g_doc ? g_doc->url : "");
    if (url_is_internal(u)) { rrect(x, 6, 10, 10, 2.5f, T.accent); x += 16; }
    else if (url_is_https(u)) { icon_lock(x + 1, 5, T.muted, T.bar); x += 15; }
    else { C2D_DrawCircleSolid(x + 4, 11, Z, 3, T.muted); x += 14; }

    char host[128] = "", title[300], buf[300];
    if (!url_is_internal(u)) url_host(u, host, sizeof host);
    if (s_loading) snprintf(title, sizeof title, "%s", host);
    else if (g_doc && g_doc->title[0]) snprintf(title, sizeof title, "%s", g_doc->title);
    else if (g_doc && url_is_internal(g_doc->url)) snprintf(title, sizeof title, "3DBrowse");
    else snprintf(title, sizeof title, "%s", host);

    /* right side: clock + wifi */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char clk[8];
    snprintf(clk, sizeof clk, "%d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
    text(clk, SCR_TOP_W - 10, 3, 0.5f, T.ink, C2D_AlignRight);
    float cw = measure(clk, (int)strlen(clk), 0.5f);
    u8 wifi = osGetWifiStrength();
    float wx = SCR_TOP_W - 18 - cw - 14;
    for (int i = 0; i < 3; i++) {
        float bh = 4.0f + (float)i * 3.0f;
        rect(wx + (float)i * 4, 16 - bh, 3, bh, (int)wifi > i ? T.ink : T.hair);
    }

    float avail = wx - x - 12;
    if (!s_loading && host[0] && g_doc && g_doc->title[0]) {
        float hw = measure(host, (int)strlen(host), 0.45f);
        if (hw < avail * 0.4f) {
            fit(title, avail - hw - 12, 0.5f, buf, sizeof buf);
            text(buf, x, 3, 0.5f, T.ink, 0);
            float tw = measure(buf, (int)strlen(buf), 0.5f);
            text(host, x + tw + 8, 4, 0.45f, T.muted, 0);
            goto progress;
        }
    }
    fit(title, avail, 0.5f, buf, sizeof buf);
    text(buf, x, 3, 0.5f, s_loading ? T.muted : T.ink, 0);

progress:
    if (s_loading) {
        int ph = net_phase();
        size_t tot = net_total(), got = net_bytes();
        if (ph == NP_DOWNLOAD && tot > 0) {
            float f = (float)got / (float)tot;
            if (f > 1) f = 1;
            rect(0, TOPBAR_H - 2, SCR_TOP_W * f, 2, T.accent);
        } else {
            float t = (float)(g_frame % 90) / 90.0f;
            float w = ph == NP_RENDER ? SCR_TOP_W : 90;
            float px = ph == NP_RENDER ? 0 : (SCR_TOP_W + w) * t - w;
            rect(px, TOPBAR_H - 2, w, 2, ph == NP_RENDER ? rgba(0xD9482B, 160) : T.accent);
        }
    }
}

typedef struct { float x, w; } Btn;
static const Btn BTNS[6] = {{0, 40}, {40, 40}, {80, 40}, {120, 128}, {248, 36}, {284, 36}};
enum { B_BACK, B_FWD, B_RELOAD, B_ADDR, B_STAR, B_MENU };

static int toolbar_hit(int px, int py) {
    if (py < SCR_H - TOOLBAR_H) return -1;
    for (int i = 0; i < 6; i++) if (px >= BTNS[i].x && px < BTNS[i].x + BTNS[i].w) return i;
    return -1;
}

static void draw_bottom(void) {
    if (L.kind != LIST_NONE) { draw_list(); return; }
    draw_page(g_scroll + TOP_VIEW, BOT_VIEW, 0, 0);

    /* toast */
    if (s_toastT > 0) {
        u8 a = s_toastT > 20 ? 235 : (u8)(s_toastT * 11);
        float w = measure(s_toast, (int)strlen(s_toast), 0.48f) + 24;
        if (w > SCR_BOT_W - 20) w = SCR_BOT_W - 20;
        float x = (SCR_BOT_W - w) / 2, y = SCR_H - TOOLBAR_H - 30;
        rrect(x, y, w, 22, 11, rgba(g_cfg.theme ? 0xE8E2D4 : 0x1E1C18, a));
        char b[120];
        fit(s_toast, w - 20, 0.48f, b, sizeof b);
        text(b, SCR_BOT_W / 2.0f, y + 4, 0.48f, rgba(g_cfg.theme ? 0x15140F : 0xF5F2EB, a), C2D_AlignCenter);
    }

    /* toolbar */
    float y0 = SCR_H - TOOLBAR_H;
    rect(0, y0, SCR_BOT_W, TOOLBAR_H, T.bar);
    rect(0, y0, SCR_BOT_W, 1, T.hair);
    float cy = y0 + TOOLBAR_H / 2.0f;
    if (s_pressBtn >= 0 && s_pressBtn != B_ADDR) {
        const Btn *b = &BTNS[s_pressBtn];
        C2D_DrawCircleSolid(b->x + b->w / 2, cy, Z, 14, T.hair);
    }
    icon_chevron(20, cy, 1, s_hi > 0 ? T.ink : T.hair);
    icon_chevron(60, cy, -1, s_hi + 1 < s_hn ? T.ink : T.hair);
    if (s_loading) icon_x(100, cy, T.accent); else icon_reload(100, cy, T.ink);

    /* address pill */
    rframe(124, y0 + 6, 120, 22, 11, s_pressBtn == B_ADDR ? T.accent : T.hair, T.field);
    char host[128] = "", buf[128];
    const char *u = s_loading ? s_loadUrl : (g_doc ? g_doc->url : "");
    if (!url_is_internal(u)) url_host(u, host, sizeof host);
    if (host[0]) {
        fit(host, 104, 0.45f, buf, sizeof buf);
        text(buf, 184, y0 + 10, 0.45f, T.ink, C2D_AlignCenter);
    } else text("Search or address", 184, y0 + 10, 0.45f, T.muted, C2D_AlignCenter);

    bool starred = page_is_web() && bm_find(g_doc->url) >= 0;
    icon_star(266, cy, starred, starred ? T.accent : (page_is_web() ? T.ink : T.hair), T.bar);
    icon_menu(302, cy, T.ink);
}

/* ------------------------------------------------------------------ input */

static void hit_test(int px, int py, int *link, int *field) {
    *link = -1;
    *field = -1;
    if (!g_doc) return;
    cache_ensure(g_scroll, g_scroll + VIEW_ALL);
    float dx = (float)px - MARGIN, dy = (float)py + g_scroll + TOP_VIEW;
    for (int i = 0; i < s_nitems; i++) {
        Item *it = &s_items[i];
        if (it->y > dy + 6) break;
        if (it->y + it->h < dy - 6) continue;
        if (it->field < 0) continue;
        if (dx >= it->x - 3 && dx <= it->x + it->w + 3 && dy >= it->y - 3 && dy <= it->y + it->h + 3) { *field = it->field; return; }
    }
    Doc *d = g_doc;
    for (int c = 0; c < s_cCount; c++) {
        Run *r = &d->runs[s_cFirst + c];
        if (r->link < 0) continue;
        if (dx >= r->x - 3 && dx <= r->x + r->w + 3 && dy >= r->y - 3 && dy <= r->y + r->h + 3) { *link = r->link; return; }
    }
}

static int item_for(int link, int field) {
    for (int i = 0; i < s_nitems; i++)
        if ((link >= 0 && s_items[i].link == link) || (field >= 0 && s_items[i].field == field)) return i;
    return -1;
}

static bool item_visible(int i) {
    Item *it = &s_items[i];
    return it->y >= g_scroll - 2 && it->y + it->h <= g_scroll + VIEW_ALL + 2;
}

static void focus_move(int dir) {
    if (!s_nitems) { g_target = g_scroll + dir * VIEW_ALL * 0.5f; return; }
    float top = g_scroll, bot = g_scroll + VIEW_ALL;
    int cand = -1;
    if (g_focus < 0 || !item_visible(g_focus)) {
        if (dir > 0) { for (int i = 0; i < s_nitems; i++) if (s_items[i].y >= top) { cand = i; break; } }
        else { for (int i = s_nitems - 1; i >= 0; i--) if (s_items[i].y + s_items[i].h <= bot) { cand = i; break; } }
    } else cand = g_focus + dir;
    if (cand < 0 || cand >= s_nitems) {
        g_target = g_scroll + dir * VIEW_ALL * 0.6f;
        return;
    }
    Item *it = &s_items[cand];
    if (it->y >= top && it->y + it->h <= bot) { g_focus = cand; return; }
    float reach = VIEW_ALL * 0.75f;
    if (dir > 0) {
        if (it->y + it->h <= bot + reach) { g_focus = cand; g_target = it->y + it->h - VIEW_ALL + 40; }
        else { g_target = g_scroll + reach; g_focus = -1; }
    } else {
        if (it->y >= top - reach) { g_focus = cand; g_target = it->y - 30; }
        else { g_target = g_scroll - reach; g_focus = -1; }
    }
}

static void handle_input(void) {
    u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp(), rep = hidKeysDownRepeat();
    touchPosition tp;
    hidTouchRead(&tp);

    if (L.kind != LIST_NONE) { list_input(down, rep, tp); return; }

    if (down & KEY_START) { g_quit = true; return; }
    if (down & KEY_Y) { address_bar(); return; }
    if (down & KEY_X) { build_menu(); L.sel = 0; L.top = 0; return; }
    if (down & KEY_SELECT) { navigate("about:bookmarks", NULL, NAV_PUSH, 0, 0, false); return; }
    if (down & KEY_L) go_back();
    if (down & KEY_R) go_forward();
    if (down & KEY_B) { if (s_loading) { net_cancel(); s_loading = false; toast("Stopped"); } else go_back(); }
    if (rep & KEY_DDOWN) focus_move(1);
    if (rep & KEY_DUP) focus_move(-1);
    if (rep & KEY_DRIGHT) { g_target = (g_target >= 0 ? g_target : g_scroll) + VIEW_ALL * 0.85f; g_focus = -1; }
    if (rep & KEY_DLEFT) { g_target = (g_target >= 0 ? g_target : g_scroll) - VIEW_ALL * 0.85f; g_focus = -1; }
    if (down & KEY_ZL) g_target = 0;
    if (down & KEY_ZR) g_target = max_scroll();
    if (down & KEY_A) {
        if (g_focus >= 0 && item_visible(g_focus)) activate_item(g_focus);
        else focus_move(1);
    }

    /* circle pad / c-stick scroll */
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    int dy = abs(cp.dy) > abs(cs.dy) ? cp.dy : cs.dy;
    if (abs(dy) > 16) {
        float v = (float)(abs(dy) - 16) / 140.0f;
        if (v > 1) v = 1;
        float speed = 2.0f + v * v * 16.0f;
        g_scroll += dy < 0 ? speed : -speed;
        g_vel = 0;
        g_target = -1;
        if (g_focus >= 0 && !item_visible(g_focus)) g_focus = -1;
    }

    /* touch */
    if (down & KEY_TOUCH) {
        s_touch = true;
        s_drag = false;
        s_tx0 = s_tlx = tp.px;
        s_ty0 = s_tly = tp.py;
        s_dragV = 0;
        s_pressBtn = toolbar_hit(tp.px, tp.py);
        s_pressLink = s_pressField = -1;
        if (s_pressBtn < 0) {
            g_vel = 0;
            g_target = -1;
            hit_test(tp.px, tp.py, &s_pressLink, &s_pressField);
        }
    } else if ((held & KEY_TOUCH) && s_touch) {
        if (s_pressBtn < 0) {
            int ddy = tp.py - s_tly;
            if (!s_drag && (abs(tp.py - s_ty0) > 7 || abs(tp.px - s_tx0) > 7)) {
                s_drag = true;
                s_pressLink = s_pressField = -1;
            }
            if (s_drag) {
                g_scroll -= (float)ddy;
                s_dragV = s_dragV * 0.4f + (float)(-ddy) * 0.6f;
                if (g_focus >= 0 && !item_visible(g_focus)) g_focus = -1;
            }
        }
        s_tlx = tp.px;
        s_tly = tp.py;
    }
    if ((up & KEY_TOUCH) && s_touch) {
        s_touch = false;
        if (s_pressBtn >= 0) {
            int b = toolbar_hit(s_tlx, s_tly);
            int pressed = s_pressBtn;
            s_pressBtn = -1;
            if (b == pressed) {
                switch (b) {
                    case B_BACK: go_back(); break;
                    case B_FWD: go_forward(); break;
                    case B_RELOAD: reload(); break;
                    case B_ADDR: address_bar(); break;
                    case B_STAR:
                        if (page_is_web()) toast(bm_toggle(g_doc->url, g_doc->title) ? "Bookmarked" : "Bookmark removed");
                        else toast("Open a web page to bookmark it");
                        break;
                    case B_MENU: build_menu(); L.sel = 0; L.top = 0; break;
                    default: break;
                }
            }
        } else if (s_drag) {
            g_vel = s_dragV;
        } else if (s_pressLink >= 0 || s_pressField >= 0) {
            int link = s_pressLink, field = s_pressField;
            g_focus = item_for(link, field);
            s_pressLink = s_pressField = -1;
            if (link >= 0) open_link(link);
            else activate_field(field);
        }
        s_pressLink = s_pressField = -1;
    }
}

static void physics(void) {
    if (g_target >= 0) {
        float m = max_scroll();
        if (g_target > m) g_target = m;
        if (g_target < 0) g_target = 0;
        float d = g_target - g_scroll;
        g_scroll += d * 0.28f;
        if (fabsf(d) < 0.5f) { g_scroll = g_target; g_target = -1; }
    } else if (!s_touch || !s_drag) {
        if (fabsf(g_vel) > 0.15f) { g_scroll += g_vel; g_vel *= 0.94f; }
        else g_vel = 0;
    }
    clamp_scroll();
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    romfsInit();
    gfxInitDefault();
    osSetSpeedupEnable(true);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(12000);
    C2D_Prepare();
    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    s_ui = C2D_TextBufNew(4096);
    s_pageBuf = C2D_TextBufNew(PAGE_GLYPHS);

    fonts_init();
    store_init();
    theme_apply(g_cfg.theme);
    hidSetRepeatParameters(18, 5);

    bool netOk = net_init();
    net_set_verify(g_cfg.verify);
    navigate("about:home", NULL, NAV_PUSH, 0, 0, false);
    if (!netOk) toast("Network couldn't start - check Wi-Fi");

    while (aptMainLoop() && !g_quit) {
        hidScanInput();
        g_frame++;
        poll_net();
        handle_input();
        if (g_quit) break;
        physics();
        if (s_refreshAt && g_frame >= s_refreshAt && !s_loading) {
            s_refreshAt = 0;
            navigate(s_refreshUrl, NULL, NAV_REPLACE, s_hi, 0, false);
        }
        if (s_toastT > 0) s_toastT--;

        cache_ensure(g_scroll, g_scroll + VIEW_ALL);
        C2D_TextBufClear(s_ui);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(s_top, T.bg);
        C2D_SceneBegin(s_top);
        draw_top();
        C2D_TargetClear(s_bot, T.bg);
        C2D_SceneBegin(s_bot);
        draw_bottom();
        C3D_FrameEnd(0);
    }

    store_save();
    net_exit();
    doc_free(g_doc);
    dc_clear();
    free(s_items);
    C2D_TextBufDelete(s_ui);
    C2D_TextBufDelete(s_pageBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return 0;
}
