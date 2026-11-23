/*
 * 3DBrowse by Meer - HTML parser and text layout engine
 *
 * One pass over the document: a forgiving tokenizer feeds a line-breaking
 * layout engine. There is no DOM, so memory use stays close to the size of
 * the visible text, and malformed HTML can never make it loop or crash -
 * every buffer is bounded and every allocation failure just ends the page.
 */
#include "html.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_TEXT   (6u << 20)
#define MAX_POOL   (2u << 20)
#define MAX_RUNS   120000
#define MAX_BOXES  60000
#define MAX_LINKS  20000
#define MAX_FIELDS 2000
#define MAX_FORMS  400
#define MAX_OPTS   256
#define LEADING    3.0f
#define INDENT     16.0f

/* ------------------------------------------------------------------ utf-8 */

uint32_t html_u8dec(const unsigned char **pp, const unsigned char *end) {
    const unsigned char *p = *pp;
    uint32_t c = *p++;
    if (c < 0x80) { *pp = p; return c; }
    int n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { *pp = p; return 0xFFFD; }
    if (end - p < n) { *pp = end; return 0xFFFD; }
    for (int i = 0; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp = p; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp = p + n;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    return cp;
}

int html_u8enc(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static const uint16_t CP1252[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

void html_escape(const char *in, char *out, size_t n) {
    size_t o = 0;
    if (!n) return;
    for (; *in && o + 7 < n; in++) {
        const char *r = NULL;
        switch (*in) {
            case '<': r = "&lt;"; break;
            case '>': r = "&gt;"; break;
            case '&': r = "&amp;"; break;
            case '"': r = "&quot;"; break;
            default: break;
        }
        if (r) { size_t l = strlen(r); memcpy(out + o, r, l); o += l; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- charset */

static bool find_charset(const char *s, size_t n, char *out, size_t on) {
    for (size_t i = 0; i + 8 < n; i++) {
        if (strncasecmp(s + i, "charset", 7)) continue;
        size_t j = i + 7;
        while (j < n && (s[j] == ' ' || s[j] == '=' || s[j] == '"' || s[j] == '\'')) j++;
        size_t k = 0;
        while (j < n && k + 1 < on && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '_')) out[k++] = s[j++];
        out[k] = 0;
        if (k) return true;
    }
    return false;
}

static bool is_latin(const char *cs) {
    return !strncasecmp(cs, "iso-8859", 8) || !strncasecmp(cs, "windows-125", 11) ||
           !strncasecmp(cs, "latin", 5) || !strcasecmp(cs, "cp1252") || !strncasecmp(cs, "us-ascii", 8) ||
           !strcasecmp(cs, "ascii");
}

char *html_to_utf8(const char *src, size_t n, const char *ctype, size_t *outLen) {
    char cs[40] = "";
    bool known = false;
    if (ctype && find_charset(ctype, strlen(ctype), cs, sizeof cs)) known = true;
    else if (find_charset(src, n < 4096 ? n : 4096, cs, sizeof cs)) known = true;

    if (n >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
        src += 3; n -= 3;
        known = true; strcpy(cs, "utf-8");
    }

    bool latin = known && is_latin(cs);
    if (!known) {
        /* no charset given: if it isn't valid UTF-8 it is almost always Windows-1252 */
        const unsigned char *p = (const unsigned char *)src, *e = p + n;
        int bad = 0;
        while (p < e && bad < 4) {
            if (*p < 0x80) { p++; continue; }
            if (html_u8dec(&p, e) == 0xFFFD) bad++;
        }
        latin = bad >= 2;
    }

    size_t cap = latin ? n * 3 + 1 : n + 1;
    char *out = (char *)malloc(cap);
    if (!out) { *outLen = 0; return NULL; }
    size_t o = 0;
    if (latin) {
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)src[i];
            uint32_t cp = c < 0x80 ? c : (c < 0xA0 ? CP1252[c - 0x80] : c);
            o += (size_t)html_u8enc(cp, out + o);
        }
    } else {
        /* copy, replacing any invalid UTF-8 so nothing downstream sees broken sequences */
        const unsigned char *p = (const unsigned char *)src, *e = p + n;
        bool bad = false;
        while (p < e) {
            if (*p < 0x80) { p++; continue; }
            if (html_u8dec(&p, e) == 0xFFFD) { bad = true; break; }
        }
        if (!bad) { memcpy(out, src, n); o = n; }
        else {
            free(out);
            cap = n * 3 + 1;
            out = (char *)malloc(cap);
            if (!out) { *outLen = 0; return NULL; }
            p = (const unsigned char *)src;
            while (p < e) {
                if (*p < 0x80) { out[o++] = (char)*p++; continue; }
                o += (size_t)html_u8enc(html_u8dec(&p, e), out + o);
            }
        }
    }
    out[o] = 0;
    *outLen = o;
    return out;
}

/* ------------------------------------------------------------------- doc */

Doc *doc_new(void) {
    Doc *d = (Doc *)calloc(1, sizeof(Doc));
    return d;
}

void doc_clear_layout(Doc *d) {
    if (!d) return;
    for (int i = 0; i < d->nfields; i++) {
        free(d->fields[i].value);
        free(d->fields[i].optv);
        free(d->fields[i].optl);
    }
    free(d->text); free(d->pool); free(d->runs); free(d->boxes);
    free(d->links); free(d->fields); free(d->forms);
    d->text = d->pool = NULL;
    d->runs = NULL; d->boxes = NULL; d->links = NULL; d->fields = NULL; d->forms = NULL;
    d->tlen = d->tcap = d->plen = d->pcap = 0;
    d->nruns = d->cruns = d->nboxes = d->cboxes = d->nlinks = d->clinks = 0;
    d->nfields = d->cfields = d->nforms = d->cforms = 0;
    d->title[0] = d->base[0] = d->refresh[0] = 0;
    d->refreshDelay = 0;
    d->height = 0;
    d->oom = false;
}

void doc_free(Doc *d) {
    if (!d) return;
    doc_clear_layout(d);
    free(d->raw);
    free(d);
}

static bool grow(void **p, int *cap, int need, size_t elem, int max) {
    if (need <= *cap) return true;
    if (need > max) return false;
    int nc = *cap ? *cap * 2 : 128;
    while (nc < need) nc *= 2;
    if (nc > max) nc = max;
    void *np = realloc(*p, (size_t)nc * elem);
    if (!np) return false;
    *p = np;
    *cap = nc;
    return true;
}

static bool grow_bytes(char **p, size_t *cap, size_t need, size_t max) {
    if (need <= *cap) return true;
    if (need > max) return false;
    size_t nc = *cap ? *cap * 2 : 16384;
    while (nc < need) nc *= 2;
    if (nc > max) nc = max;
    char *np = (char *)realloc(*p, nc);
    if (!np) return false;
    *p = np;
    *cap = nc;
    return true;
}

static uint32_t pool_add(Doc *d, const char *s, size_t n) {
    if (!d->pool) {
        if (!grow_bytes(&d->pool, &d->pcap, 1024, MAX_POOL)) { d->oom = true; return 0; }
        d->pool[0] = 0;
        d->plen = 1;
    }
    if (!s || !n) return 0;
    if (!grow_bytes(&d->pool, &d->pcap, d->plen + n + 1, MAX_POOL)) { d->oom = true; return 0; }
    uint32_t off = (uint32_t)d->plen;
    memcpy(d->pool + off, s, n);
    d->pool[off + n] = 0;
    d->plen += n + 1;
    return off;
}

/* ---------------------------------------------------------------- layout */

#define MAX_ATTRS 24
typedef struct { char name[16]; char *val; bool has; } Attr;
typedef struct { Attr a[MAX_ATTRS]; int n; char buf[6144]; size_t used; } Attrs;

enum { CAP_NONE, CAP_OPTION, CAP_BUTTON };

typedef struct {
    Doc *d;
    const LayoutCfg *c;
    float x, y, left, right;
    float lineMaxH, gapAcc;
    int lineRun, lineBox, lineItems;
    bool pendSpace;

    int bold, ital, mono, pre, muted, under, strike, small, head;
    int link;
    int listDepth, listType[16], listNum[16];
    float quoteY[16]; int quoteDepth;
    int ddDepth;
    float indent;
    int form;

    char word[600]; int wlen;

    int capture;
    char cap[512]; int caplen;
    int selField;          /* open <select> */
    bool optOpen; char optVal[256]; bool optHasVal, optSel;
    int btnField;

    char skipTag[16]; int skipDepth;
    bool plain, wordSpace;
    Attrs as;
} L;

static float cur_scale(L *l) {
    static const float H[7] = {1.0f, 1.5f, 1.3f, 1.15f, 1.05f, 1.0f, 0.95f};
    float s = l->c->base * H[l->head > 6 ? 6 : l->head];
    if (l->small && !l->head) s *= 0.86f;
    return s;
}

static uint16_t cur_style(L *l) {
    uint16_t st = 0;
    if (l->bold || l->head) st |= RS_BOLD;
    if (l->ital) st |= RS_ITALIC;
    if (l->mono) st |= RS_MONO;
    if (l->link >= 0) st |= RS_LINK;
    if (l->muted || (l->small && !l->head)) st |= RS_MUTED;
    if (l->under) st |= RS_UNDER;
    if (l->strike) st |= RS_STRIKE;
    if (l->head) st |= RS_HEAD;
    return st;
}

static bool line_empty(L *l) { return l->d->nruns == l->lineRun && l->lineItems == 0; }

static void line_end(L *l) {
    Doc *d = l->d;
    if (line_empty(l)) { l->x = l->left; return; }
    float h = l->lineMaxH;
    for (int i = l->lineRun; i < d->nruns; i++) d->runs[i].y = l->y + (h - d->runs[i].h);
    for (int i = l->lineBox; i < d->nboxes; i++) {
        Box *b = &d->boxes[i];
        if (b->inl == 1) b->y = l->y + (h - b->h) * 0.5f;
        else if (b->inl == 2) b->y = l->y + h - l->c->lineFeed * l->c->base * 0.5f - b->h * 0.5f;
    }
    l->y += h + LEADING;
    l->x = l->left;
    l->lineMaxH = 0;
    l->lineRun = d->nruns;
    l->lineBox = d->nboxes;
    l->lineItems = 0;
    l->gapAcc = 0;
}

static void flush_word(L *l);

static void block(L *l, float gap) {
    flush_word(l);
    line_end(l);
    l->pendSpace = false;
    if (l->y <= 0.5f) return;
    if (gap > l->gapAcc) { l->y += gap - l->gapAcc; l->gapAcc = gap; }
}

static void set_indent(L *l, float ind) {
    if (ind < 0) ind = 0;
    float maxInd = l->c->width * 0.4f;
    l->indent = ind;
    l->left = ind > maxInd ? maxInd : ind;
    if (line_empty(l)) l->x = l->left;
}

static int add_box(L *l, int kind, float x, float y, float w, float h, int ref, int inl) {
    Doc *d = l->d;
    if (!grow((void **)&d->boxes, &d->cboxes, d->nboxes + 1, sizeof(Box), MAX_BOXES)) { d->oom = true; return -1; }
    Box *b = &d->boxes[d->nboxes];
    b->x = x; b->y = y; b->w = w; b->h = h; b->kind = (uint8_t)kind; b->inl = (uint8_t)inl; b->ref = ref;
    return d->nboxes++;
}

static bool text_append(Doc *d, const char *s, int n) {
    if (!grow_bytes(&d->text, &d->tcap, d->tlen + (size_t)n + 2, MAX_TEXT)) { d->oom = true; return false; }
    memcpy(d->text + d->tlen, s, (size_t)n);
    d->tlen += (size_t)n;
    d->text[d->tlen] = 0;
    return true;
}

/* adds text at the pen position, merging into the previous run when it continues it */
static void add_run(L *l, const char *s, int n, float w, float h, float sc, uint16_t st, float sw) {
    Doc *d = l->d;
    if (d->oom) return;
    if (d->nruns > l->lineRun) {
        Run *last = &d->runs[d->nruns - 1];
        if (last->style == st && last->link == l->link && last->scale == sc &&
            fabsf(last->x + last->w - l->x) < 0.01f &&
            last->text + last->len == d->tlen && (int)last->len + n + 1 < 60000) {
            if (sw > 0 && !text_append(d, " ", 1)) return;
            if (!text_append(d, s, n)) return;
            last->len = (uint16_t)(last->len + n + (sw > 0 ? 1 : 0));
            last->w += sw + w;
            l->x += sw + w;
            return;
        }
    }
    l->x += sw;
    if (!grow((void **)&d->runs, &d->cruns, d->nruns + 1, sizeof(Run), MAX_RUNS)) { d->oom = true; return; }
    if (d->tlen) { d->tlen++; }   /* keep the previous run's NUL terminator */
    uint32_t off = (uint32_t)d->tlen;
    if (!text_append(d, s, n)) return;
    Run *r = &d->runs[d->nruns++];
    r->x = l->x; r->y = l->y; r->w = w; r->h = h; r->scale = sc;
    r->text = off; r->len = (uint16_t)n; r->style = st; r->link = l->link;
    l->x += w;
    if (h > l->lineMaxH) l->lineMaxH = h;
}

static int fit_chars(L *l, const char *s, int n, float sc, float room, float *outW) {
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    float w = 0;
    int taken = 0;
    while (p < e) {
        const unsigned char *q = p;
        html_u8dec(&q, e);
        float cw = l->c->measure((const char *)p, (int)(q - p), sc);
        if (w + cw > room && taken > 0) break;
        if (w + cw > room && l->x > l->left) break;
        w += cw;
        taken = (int)(q - (const unsigned char *)s);
        p = q;
    }
    *outW = w;
    return taken;
}

static void emit_text(L *l, const char *s, int n) {
    if (l->d->oom || n <= 0) return;
    float sc = cur_scale(l);
    uint16_t st = cur_style(l);
    float h = l->c->lineFeed * sc;
    float sw = (l->pendSpace && l->x > l->left) ? l->c->measure(" ", 1, sc) : 0;
    l->pendSpace = false;
    float w = l->c->measure(s, n, sc);
    float avail = l->right - l->left;

    if (l->x + sw + w > l->right && l->x > l->left) { line_end(l); sw = 0; }
    if (w <= avail) { add_run(l, s, n, w, h, sc, st, sw); return; }

    /* a word wider than the column (long URLs, pre lines): split it */
    const char *p = s;
    int rem = n;
    int guard = 0;
    while (rem > 0 && guard++ < 10000 && !l->d->oom) {
        float tw;
        int take = fit_chars(l, p, rem, sc, l->right - l->x - sw, &tw);
        if (take <= 0) { line_end(l); sw = 0; continue; }
        add_run(l, p, take, tw, h, sc, st, sw);
        sw = 0;
        p += take;
        rem -= take;
        if (rem > 0) line_end(l);
    }
}

static void flush_word(L *l) {
    if (!l->wlen) return;
    emit_text(l, l->word, l->wlen);
    l->wlen = 0;
}

static void hard_break(L *l) {
    flush_word(l);
    if (!line_empty(l)) line_end(l);
    else { l->y += l->c->lineFeed * l->c->base + LEADING; }
    l->pendSpace = false;
}

static void inline_box(L *l, float w, float h, int ref) {
    flush_word(l);
    float sw = (l->pendSpace && l->x > l->left) ? l->c->measure(" ", 1, cur_scale(l)) : 0;
    l->pendSpace = false;
    if (w > l->right - l->left) w = l->right - l->left;
    if (l->x + sw + w > l->right && l->x > l->left) { line_end(l); sw = 0; }
    l->x += sw;
    int b = add_box(l, BX_FIELD, l->x, l->y, w, h, ref, 1);
    if (b >= 0 && ref >= 0 && ref < l->d->nfields) l->d->fields[ref].box = b;
    l->x += w;
    if (h > l->lineMaxH) l->lineMaxH = h;
    l->lineItems++;
    l->pendSpace = true;
}

static void word_add(L *l, uint32_t cp) {
    if (l->wlen + 5 >= (int)sizeof l->word) flush_word(l);
    l->wlen += html_u8enc(cp, l->word + l->wlen);
}

static void cap_add(L *l, uint32_t cp) {
    if (cp == '\n' || cp == '\t' || cp == '\r' || cp == 0xA0) cp = ' ';
    if (cp == ' ' && (l->caplen == 0 || l->cap[l->caplen - 1] == ' ')) return;
    if (l->caplen + 5 >= (int)sizeof l->cap) return;
    l->caplen += html_u8enc(cp, l->cap + l->caplen);
    l->cap[l->caplen] = 0;
}

static void put_cp(L *l, uint32_t cp) {
    if (l->skipDepth) return;
    if (l->capture) { cap_add(l, cp); return; }
    if (l->selField >= 0) return;   /* stray text inside <select> */
    if (l->pre) {
        if (cp == '\r') return;
        if (cp == '\n') { flush_word(l); hard_break(l); return; }
        if (cp == '\t') cp = ' ';
        if (cp == ' ') {
            /* keep indentation at the start of a line, otherwise allow wrapping here */
            if (line_empty(l) && (l->wlen == 0 || l->wordSpace)) { word_add(l, ' '); l->wordSpace = true; return; }
            flush_word(l);
            l->pendSpace = true;
            return;
        }
        if (cp < 32) return;
        l->wordSpace = false;
        word_add(l, cp);
        return;
    }
    if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r' || cp == '\f') {
        flush_word(l);
        l->pendSpace = true;
        return;
    }
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return;
    if (cp == 0xAD || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF || cp == 0x2060) return;
    if (cp == 0xA0 || cp == 0x2002 || cp == 0x2003 || cp == 0x2009 || cp == 0x202F) cp = ' ';
    word_add(l, cp);
}

/* --------------------------------------------------------------- entities */

typedef struct { const char *n; uint16_t cp; } Ent;
static const Ent ENTS[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013},
    {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"lsaquo", 0x2039},
    {"rsaquo", 0x203A}, {"middot", 0xB7}, {"bull", 0x2022}, {"deg", 0xB0}, {"plusmn", 0xB1},
    {"times", 0xD7}, {"divide", 0xF7}, {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2},
    {"sect", 0xA7}, {"para", 0xB6}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE},
    {"sup1", 0xB9}, {"sup2", 0xB2}, {"sup3", 0xB3}, {"micro", 0xB5}, {"iexcl", 0xA1}, {"iquest", 0xBF},
    {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"harr", 0x2194},
    {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"zwj", 0x200D}, {"zwnj", 0x200C},
    {"shy", 0xAD}, {"prime", 0x2032}, {"Prime", 0x2033}, {"dagger", 0x2020}, {"Dagger", 0x2021},
    {"aacute", 0xE1}, {"eacute", 0xE9}, {"iacute", 0xED}, {"oacute", 0xF3}, {"uacute", 0xFA},
    {"Aacute", 0xC1}, {"Eacute", 0xC9}, {"Iacute", 0xCD}, {"Oacute", 0xD3}, {"Uacute", 0xDA},
    {"agrave", 0xE0}, {"egrave", 0xE8}, {"igrave", 0xEC}, {"ograve", 0xF2}, {"ugrave", 0xF9},
    {"auml", 0xE4}, {"euml", 0xEB}, {"iuml", 0xEF}, {"ouml", 0xF6}, {"uuml", 0xFC},
    {"Auml", 0xC4}, {"Ouml", 0xD6}, {"Uuml", 0xDC}, {"szlig", 0xDF}, {"ntilde", 0xF1}, {"Ntilde", 0xD1},
    {"ccedil", 0xE7}, {"Ccedil", 0xC7}, {"acirc", 0xE2}, {"ecirc", 0xEA}, {"icirc", 0xEE},
    {"ocirc", 0xF4}, {"ucirc", 0xFB}, {"aring", 0xE5}, {"Aring", 0xC5}, {"aelig", 0xE6},
    {"oslash", 0xF8}, {"Oslash", 0xD8}, {"atilde", 0xE3}, {"otilde", 0xF5}, {"hearts", 0x2665},
    {"check", 0x2713}, {"minus", 0x2212}, {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260},
    {"infin", 0x221E}, {"alpha", 0x3B1}, {"beta", 0x3B2}, {"pi", 0x3C0}, {"mu", 0x3BC},
};

/* p points just after '&'. Returns the code point and advances *pp, or 0 if not an entity. */
static uint32_t entity(const char **pp, const char *end) {
    const char *p = *pp;
    if (p < end && *p == '#') {
        p++;
        uint32_t v = 0;
        bool hex = false;
        if (p < end && (*p == 'x' || *p == 'X')) { hex = true; p++; }
        const char *s = p;
        while (p < end && p - s < 8 && (hex ? isxdigit((unsigned char)*p) : isdigit((unsigned char)*p))) {
            char c = *p++;
            v = v * (hex ? 16 : 10) + (uint32_t)(isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
        }
        if (p == s) return 0;
        if (p < end && *p == ';') p++;
        *pp = p;
        if (v >= 0x80 && v < 0xA0) v = CP1252[v - 0x80];
        if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
        return v;
    }
    char name[12];
    int k = 0;
    while (p < end && k < 10 && isalnum((unsigned char)*p)) name[k++] = *p++;
    name[k] = 0;
    if (!k) return 0;
    for (size_t i = 0; i < sizeof ENTS / sizeof ENTS[0]; i++) {
        if (!strcmp(ENTS[i].n, name)) {
            if (p < end && *p == ';') p++;
            else if (k > 4) return 0;   /* only short legacy names may omit the ';' */
            *pp = p;
            return ENTS[i].cp;
        }
    }
    return 0;
}

/* decodes entities in [s,e) into out (UTF-8) */
static void decode_into(const char *s, const char *e, char *out, size_t on) {
    size_t o = 0;
    while (s < e && o + 5 < on) {
        if (*s == '&') {
            const char *q = s + 1;
            uint32_t cp = entity(&q, e);
            if (cp) { o += (size_t)html_u8enc(cp, out + o); s = q; continue; }
        }
        out[o++] = *s++;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ tags */

enum {
    T_UNKNOWN, T_A, T_ABBR, T_ADDRESS, T_ARTICLE, T_ASIDE, T_B, T_BASE, T_BIG, T_BLOCKQUOTE, T_BODY, T_BR,
    T_BUTTON, T_CANVAS, T_CAPTION, T_CENTER, T_CITE, T_CODE, T_DD, T_DEL, T_DETAILS, T_DFN, T_DIV, T_DL, T_DT,
    T_EM, T_FIELDSET, T_FIGCAPTION, T_FIGURE, T_FOOTER, T_FORM, T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_HEAD,
    T_HEADER, T_HR, T_HTML, T_I, T_IFRAME, T_IMG, T_INPUT, T_INS, T_KBD, T_LABEL, T_LEGEND, T_LI, T_LINK, T_MAIN,
    T_MARK, T_MATH, T_META, T_NAV, T_NOSCRIPT, T_OBJECT, T_OL, T_OPTGROUP, T_OPTION, T_P, T_PRE, T_Q, T_S,
    T_SAMP, T_SCRIPT, T_SECTION, T_SELECT, T_SMALL, T_SPAN, T_STRIKE, T_STRONG, T_STYLE, T_SUB, T_SUMMARY,
    T_SUP, T_SVG, T_TABLE, T_TBODY, T_TD, T_TEMPLATE, T_TEXTAREA, T_TFOOT, T_TH, T_THEAD, T_TITLE, T_TR, T_TT,
    T_U, T_UL, T_VAR, T_WBR, T_XMP, T_VIDEO, T_AUDIO, T_PICTURE, T_SOURCE, T_DIALOG, T_HGROUP, T_MENU,
};

typedef struct { const char *n; int id; } TagName;
static const TagName TAGS[] = {
    {"a", T_A}, {"abbr", T_ABBR}, {"address", T_ADDRESS}, {"article", T_ARTICLE}, {"aside", T_ASIDE},
    {"audio", T_AUDIO}, {"b", T_B}, {"base", T_BASE}, {"big", T_BIG}, {"blockquote", T_BLOCKQUOTE},
    {"body", T_BODY}, {"br", T_BR}, {"button", T_BUTTON}, {"canvas", T_CANVAS}, {"caption", T_CAPTION},
    {"center", T_CENTER}, {"cite", T_CITE}, {"code", T_CODE}, {"dd", T_DD}, {"del", T_DEL},
    {"details", T_DETAILS}, {"dfn", T_DFN}, {"dialog", T_DIALOG}, {"div", T_DIV}, {"dl", T_DL}, {"dt", T_DT},
    {"em", T_EM}, {"fieldset", T_FIELDSET}, {"figcaption", T_FIGCAPTION}, {"figure", T_FIGURE},
    {"footer", T_FOOTER}, {"form", T_FORM}, {"h1", T_H1}, {"h2", T_H2}, {"h3", T_H3}, {"h4", T_H4},
    {"h5", T_H5}, {"h6", T_H6}, {"head", T_HEAD}, {"header", T_HEADER}, {"hgroup", T_HGROUP}, {"hr", T_HR},
    {"html", T_HTML}, {"i", T_I}, {"iframe", T_IFRAME}, {"img", T_IMG}, {"input", T_INPUT}, {"ins", T_INS},
    {"kbd", T_KBD}, {"label", T_LABEL}, {"legend", T_LEGEND}, {"li", T_LI}, {"link", T_LINK},
    {"main", T_MAIN}, {"mark", T_MARK}, {"math", T_MATH}, {"menu", T_MENU}, {"meta", T_META}, {"nav", T_NAV},
    {"noscript", T_NOSCRIPT}, {"object", T_OBJECT}, {"ol", T_OL}, {"optgroup", T_OPTGROUP},
    {"option", T_OPTION}, {"p", T_P}, {"picture", T_PICTURE}, {"pre", T_PRE}, {"q", T_Q}, {"s", T_S},
    {"samp", T_SAMP}, {"script", T_SCRIPT}, {"section", T_SECTION}, {"select", T_SELECT},
    {"small", T_SMALL}, {"source", T_SOURCE}, {"span", T_SPAN}, {"strike", T_STRIKE}, {"strong", T_STRONG},
    {"style", T_STYLE}, {"sub", T_SUB}, {"summary", T_SUMMARY}, {"sup", T_SUP}, {"svg", T_SVG},
    {"table", T_TABLE}, {"tbody", T_TBODY}, {"td", T_TD}, {"template", T_TEMPLATE},
    {"textarea", T_TEXTAREA}, {"tfoot", T_TFOOT}, {"th", T_TH}, {"thead", T_THEAD}, {"title", T_TITLE},
    {"tr", T_TR}, {"tt", T_TT}, {"u", T_U}, {"ul", T_UL}, {"var", T_VAR}, {"video", T_VIDEO},
    {"wbr", T_WBR}, {"xmp", T_XMP},
};

static int tag_id(const char *n) {
    int lo = 0, hi = (int)(sizeof TAGS / sizeof TAGS[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(n, TAGS[mid].n);
        if (!c) return TAGS[mid].id;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return T_UNKNOWN;
}

static bool is_void(int id) {
    switch (id) {
        case T_BR: case T_HR: case T_IMG: case T_INPUT: case T_META: case T_LINK: case T_BASE:
        case T_WBR: case T_SOURCE: return true;
        default: return false;
    }
}

static const char *attr(const Attrs *as, const char *name) {
    for (int i = 0; i < as->n; i++)
        if (!strcmp(as->a[i].name, name)) return as->a[i].val;
    return NULL;
}
static bool has_attr(const Attrs *as, const char *name) {
    for (int i = 0; i < as->n; i++)
        if (!strcmp(as->a[i].name, name)) return true;
    return false;
}

static bool css_hidden(const char *style) {
    if (!style) return false;
    char s[256];
    size_t o = 0;
    for (const char *p = style; *p && o + 1 < sizeof s; p++)
        if (!isspace((unsigned char)*p)) s[o++] = (char)tolower((unsigned char)*p);
    s[o] = 0;
    return strstr(s, "display:none") || strstr(s, "visibility:hidden");
}

/* ---- fields */

static int add_field(L *l, int type, const Attrs *as) {
    Doc *d = l->d;
    if (!grow((void **)&d->fields, &d->cfields, d->nfields + 1, sizeof(Field), MAX_FIELDS)) { d->oom = true; return -1; }
    Field *f = &d->fields[d->nfields];
    memset(f, 0, sizeof *f);
    f->form = l->form;
    f->type = (uint8_t)type;
    f->box = -1;
    const char *nm = attr(as, "name");
    const char *vl = attr(as, "value");
    const char *ph = attr(as, "placeholder");
    if (!ph) ph = attr(as, "aria-label");
    f->name = nm ? pool_add(d, nm, strlen(nm)) : 0;
    f->placeholder = ph ? pool_add(d, ph, strlen(ph)) : 0;
    f->value = strdup(vl ? vl : "");
    if (!f->value) { d->oom = true; return -1; }
    f->checked = has_attr(as, "checked");
    return d->nfields++;
}

static float field_h(L *l) { return l->c->lineFeed * l->c->base + 10.0f; }

static void finish_option(L *l) {
    if (!l->optOpen) return;
    l->optOpen = false;
    l->capture = CAP_NONE;
    Doc *d = l->d;
    if (l->selField < 0 || l->selField >= d->nfields) return;
    Field *f = &d->fields[l->selField];
    if (f->nopt >= MAX_OPTS) return;
    if (f->nopt == 0) {
        f->optv = (uint32_t *)malloc(sizeof(uint32_t) * MAX_OPTS);
        f->optl = (uint32_t *)malloc(sizeof(uint32_t) * MAX_OPTS);
        if (!f->optv || !f->optl) { free(f->optv); free(f->optl); f->optv = f->optl = NULL; d->oom = true; return; }
    }
    if (!f->optv) return;
    while (l->caplen && l->cap[l->caplen - 1] == ' ') l->cap[--l->caplen] = 0;
    const char *val = l->optHasVal ? l->optVal : l->cap;
    f->optl[f->nopt] = pool_add(d, l->cap, (size_t)l->caplen);
    f->optv[f->nopt] = pool_add(d, val, strlen(val));
    if (l->optSel) f->sel = f->nopt;
    f->nopt++;
}

static void finish_select(L *l) {
    if (l->selField < 0) return;
    finish_option(l);
    Doc *d = l->d;
    int fi = l->selField;
    l->selField = -1;
    if (fi >= d->nfields) return;
    Field *f = &d->fields[fi];
    float maxw = 40;
    for (int i = 0; i < f->nopt; i++) {
        const char *s = doc_str(d, f->optl[i]);
        float w = l->c->measure(s, (int)strlen(s), l->c->base);
        if (w > maxw) maxw = w;
    }
    inline_box(l, maxw + 30, field_h(l), fi);
}

static void finish_button(L *l) {
    if (l->capture != CAP_BUTTON) return;
    l->capture = CAP_NONE;
    Doc *d = l->d;
    int fi = l->btnField;
    l->btnField = -1;
    if (fi < 0 || fi >= d->nfields) return;
    while (l->caplen && l->cap[l->caplen - 1] == ' ') l->cap[--l->caplen] = 0;
    const char *lab = l->caplen ? l->cap : (d->fields[fi].value[0] ? d->fields[fi].value : "Submit");
    d->fields[fi].label = pool_add(d, lab, strlen(lab));
    lab = doc_str(d, d->fields[fi].label);
    inline_box(l, l->c->measure(lab, (int)strlen(lab), l->c->base) + 20, field_h(l), fi);
}

static void do_input(L *l, const Attrs *as) {
    const char *t = attr(as, "type");
    char type[16] = "text";
    if (t) { size_t i; for (i = 0; t[i] && i < 15; i++) type[i] = (char)tolower((unsigned char)t[i]); type[i] = 0; }
    Doc *d = l->d;

    if (!strcmp(type, "hidden")) { add_field(l, F_HIDDEN, as); return; }
    if (has_attr(as, "hidden") || css_hidden(attr(as, "style"))) return;

    if (!strcmp(type, "submit") || !strcmp(type, "image") || !strcmp(type, "button") || !strcmp(type, "reset")) {
        int fi = add_field(l, !strcmp(type, "button") || !strcmp(type, "reset") ? F_BUTTON : F_SUBMIT, as);
        if (fi < 0) return;
        Field *f = &d->fields[fi];
        const char *lab = f->value[0] ? f->value : (!strcmp(type, "image") ? (attr(as, "alt") ? attr(as, "alt") : "Submit")
                                                   : !strcmp(type, "reset") ? "Reset" : "Submit");
        f->label = pool_add(d, lab, strlen(lab));
        lab = doc_str(d, f->label);
        inline_box(l, l->c->measure(lab, (int)strlen(lab), l->c->base) + 20, field_h(l), fi);
        return;
    }
    if (!strcmp(type, "checkbox") || !strcmp(type, "radio")) {
        int fi = add_field(l, type[0] == 'c' ? F_CHECK : F_RADIO, as);
        if (fi < 0) return;
        if (!attr(as, "value")) { free(d->fields[fi].value); d->fields[fi].value = strdup("on"); }
        float s = l->c->lineFeed * l->c->base * 0.9f;
        inline_box(l, s, s, fi);
        return;
    }
    if (!strcmp(type, "file") || !strcmp(type, "range") || !strcmp(type, "color")) return;
    int fi = add_field(l, !strcmp(type, "password") ? F_PASSWORD : F_TEXT, as);
    if (fi < 0) return;
    float w = l->right - l->left;
    if (w > 220) w = 220;
    inline_box(l, w, field_h(l), fi);
}

/* ---- the tag handler */

static void open_list(L *l, int type, const Attrs *as) {
    block(l, l->listDepth ? 2 : 8);
    if (l->listDepth < 16) {
        l->listType[l->listDepth] = type;
        const char *st = as ? attr(as, "start") : NULL;
        l->listNum[l->listDepth] = st ? atoi(st) - 1 : 0;
    }
    l->listDepth++;
    set_indent(l, l->indent + INDENT);
}

static void close_list(L *l) {
    if (!l->listDepth) return;
    block(l, l->listDepth > 1 ? 2 : 8);
    l->listDepth--;
    set_indent(l, l->indent - INDENT);
}

static void close_dd(L *l) {
    if (!l->ddDepth) return;
    block(l, 2);
    l->ddDepth--;
    set_indent(l, l->indent - INDENT);
}

static void handle_tag(L *l, int id, bool closing, const Attrs *as) {
    Doc *d = l->d;
    /* a tag ends the current word, so styles change at the right place */
    if (!l->capture && l->selField < 0) flush_word(l);

    /* while collecting <option>/<button> text only a few tags matter */
    if (l->capture == CAP_BUTTON) {
        if (id == T_IMG && !closing) {
            const char *alt = attr(as, "alt");
            if (alt) for (const char *p = alt; *p; p++) cap_add(l, (unsigned char)*p);
            return;
        }
        if (id == T_BUTTON || id == T_FORM || id == T_DIV || id == T_P || id == T_LI || id == T_TD ||
            id == T_TR || id == T_TABLE || id == T_SELECT || id == T_TEXTAREA || id == T_INPUT) {
            finish_button(l);
            if (id == T_BUTTON && closing) return;
        } else return;
    }
    if (l->selField >= 0) {
        if (id == T_OPTION && !closing) {
            finish_option(l);
            l->optOpen = true;
            l->capture = CAP_OPTION;
            l->caplen = 0; l->cap[0] = 0;
            const char *v = attr(as, "value");
            l->optHasVal = v != NULL;
            snprintf(l->optVal, sizeof l->optVal, "%s", v ? v : "");
            l->optSel = has_attr(as, "selected");
            return;
        }
        if (id == T_OPTION || id == T_OPTGROUP) { finish_option(l); return; }
        if (id == T_SELECT || id == T_FORM || id == T_TEXTAREA || id == T_INPUT || id == T_BUTTON ||
            id == T_TABLE || id == T_TD || id == T_TR || id == T_DIV || id == T_P) {
            finish_select(l);
            if (id == T_SELECT) return;
        } else return;
    }

    if (closing) {
        switch (id) {
            case T_A: l->link = -1; break;
            case T_B: case T_STRONG: case T_TH: if (l->bold) l->bold--; if (id == T_TH) l->pendSpace = true; break;
            case T_I: case T_EM: case T_CITE: case T_VAR: case T_DFN: if (l->ital) l->ital--; break;
            case T_Q: put_cp(l, 0x201D); if (l->ital) l->ital--; break;
            case T_CODE: case T_KBD: case T_SAMP: case T_TT: if (l->mono) l->mono--; break;
            case T_SMALL: case T_SUB: case T_SUP: if (l->small) l->small--; break;
            case T_U: case T_INS: if (l->under) l->under--; break;
            case T_S: case T_STRIKE: case T_DEL: if (l->strike) l->strike--; break;
            case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
                block(l, 6); l->head = 0; break;
            case T_P: block(l, 9); break;
            case T_PRE: case T_XMP: block(l, 7); if (l->pre) l->pre--; if (l->mono) l->mono--; break;
            case T_UL: case T_OL: case T_MENU: close_list(l); break;
            case T_DD: close_dd(l); break;
            case T_DL: close_dd(l); block(l, 8); break;
            case T_BLOCKQUOTE:
                flush_word(l); line_end(l);
                if (l->quoteDepth) {
                    l->quoteDepth--;
                    float sy = l->quoteY[l->quoteDepth < 16 ? l->quoteDepth : 15];
                    set_indent(l, l->indent - 12);
                    if (l->y > sy) add_box(l, BX_QUOTE, l->left + 1, sy, 2.5f, l->y - sy - LEADING, -1, 0);
                }
                block(l, 9);
                break;
            case T_FORM: block(l, 6); l->form = -1; break;
            case T_TR: case T_TABLE: case T_CAPTION: block(l, id == T_TABLE ? 8 : 0); break;
            case T_LI: case T_DT: block(l, 2); break;
            case T_DIV: case T_SECTION: case T_ARTICLE: case T_HEADER: case T_FOOTER: case T_NAV:
            case T_MAIN: case T_ASIDE: case T_FIGURE: case T_FIGCAPTION: case T_ADDRESS: case T_FIELDSET:
            case T_LEGEND: case T_DETAILS: case T_SUMMARY: case T_CENTER: case T_NOSCRIPT: case T_HGROUP:
            case T_DIALOG:
                block(l, id == T_DIV ? 0 : 4); break;
            case T_LABEL: l->pendSpace = true; break;
            default: break;
        }
        return;
    }

    switch (id) {
        case T_A: {
            const char *h = attr(as, "href");
            l->link = -1;
            if (h && *h && strncasecmp(h, "javascript:", 11) &&
                grow((void **)&d->links, &d->clinks, d->nlinks + 1, sizeof(uint32_t), MAX_LINKS)) {
                d->links[d->nlinks] = pool_add(d, h, strlen(h));
                l->link = d->nlinks++;
            }
            break;
        }
        case T_B: case T_STRONG: l->bold++; break;
        case T_TH: l->bold++; /* fall through */
        case T_TD:
            flush_word(l);
            if (!line_empty(l)) {
                if (l->x + 12 < l->right) l->x += 12;
                else l->pendSpace = true;
            }
            break;
        case T_I: case T_EM: case T_CITE: case T_VAR: case T_DFN: l->ital++; break;
        case T_Q: l->ital++; put_cp(l, 0x201C); break;
        case T_CODE: case T_KBD: case T_SAMP: case T_TT: l->mono++; break;
        case T_SMALL: case T_SUB: case T_SUP: l->small++; break;
        case T_U: case T_INS: l->under++; break;
        case T_S: case T_STRIKE: case T_DEL: l->strike++; break;
        case T_BR: hard_break(l); break;
        case T_WBR: flush_word(l); break;
        case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: {
            int lv = id - T_H1 + 1;
            static const float G[7] = {0, 16, 14, 12, 10, 8, 8};
            block(l, G[lv]);
            l->head = lv;
            break;
        }
        case T_P: block(l, 9); break;
        case T_PRE: case T_XMP: block(l, 7); l->pre++; l->mono++; break;
        case T_HR:
            block(l, 9);
            add_box(l, BX_HR, l->left, l->y, l->right - l->left, 1, -1, 0);
            l->y += 1;
            l->gapAcc = 0;
            block(l, 9);
            break;
        case T_UL: case T_MENU: open_list(l, 0, as); break;
        case T_OL: open_list(l, 1, as); break;
        case T_LI: {
            block(l, 2);
            if (!l->listDepth) open_list(l, 0, NULL);
            int dpt = l->listDepth - 1 < 16 ? l->listDepth - 1 : 15;
            float sc = l->c->base;
            if (l->listType[dpt] == 1) {
                char num[16];
                int n = snprintf(num, sizeof num, "%d.", ++l->listNum[dpt]);
                float w = l->c->measure(num, n, sc);
                float savex = l->x;
                int saveLink = l->link;
                l->link = -1;
                l->x = l->left - w - 5;
                uint16_t st = (uint16_t)(cur_style(l) | RS_MUTED);
                st &= (uint16_t)~(RS_LINK | RS_UNDER);
                if (l->x < 0) l->x = 0;
                add_run(l, num, n, w, l->c->lineFeed * sc, sc, st, 0);
                l->x = savex;
                l->link = saveLink;
            } else {
                float s = dpt ? 4.0f : 5.0f;
                add_box(l, BX_BULLET, l->left - 10, l->y, s, s, dpt, 2);
                l->lineItems++;
            }
            break;
        }
        case T_DL: block(l, 8); break;
        case T_DT: close_dd(l); block(l, 4); break;
        case T_DD: close_dd(l); block(l, 2); l->ddDepth++; set_indent(l, l->indent + INDENT); break;
        case T_BLOCKQUOTE:
            block(l, 9);
            if (l->quoteDepth < 16) l->quoteY[l->quoteDepth] = l->y;
            l->quoteDepth++;
            set_indent(l, l->indent + 12);
            break;
        case T_IMG: {
            const char *alt = attr(as, "alt");
            const char *w = attr(as, "width"), *h = attr(as, "height");
            if ((w && atoi(w) > 0 && atoi(w) <= 2) || (h && atoi(h) > 0 && atoi(h) <= 2)) break;
            char buf[200];
            if (alt && *alt) {
                char a[180];
                snprintf(a, sizeof a, "%s", alt);
                snprintf(buf, sizeof buf, "[%s]", a);
            } else if (l->link >= 0) snprintf(buf, sizeof buf, "[image]");
            else break;
            l->muted++;
            flush_word(l);
            const unsigned char *p = (const unsigned char *)buf, *e = p + strlen(buf);
            while (p < e) put_cp(l, html_u8dec(&p, e));
            flush_word(l);
            l->muted--;
            break;
        }
        case T_FORM: {
            block(l, 6);
            l->form = -1;
            if (grow((void **)&d->forms, &d->cforms, d->nforms + 1, sizeof(Form), MAX_FORMS)) {
                const char *a = attr(as, "action");
                const char *m = attr(as, "method");
                d->forms[d->nforms].action = a ? pool_add(d, a, strlen(a)) : 0;
                d->forms[d->nforms].post = m && !strcasecmp(m, "post");
                l->form = d->nforms++;
            }
            break;
        }
        case T_INPUT: do_input(l, as); break;
        case T_BUTTON: {
            const char *t = attr(as, "type");
            int fi = add_field(l, (t && (!strcasecmp(t, "button") || !strcasecmp(t, "reset"))) ? F_BUTTON : F_SUBMIT, as);
            if (fi < 0) break;
            flush_word(l);
            l->capture = CAP_BUTTON;
            l->btnField = fi;
            l->caplen = 0; l->cap[0] = 0;
            break;
        }
        case T_SELECT: {
            int fi = add_field(l, F_SELECT, as);
            if (fi < 0) break;
            flush_word(l);
            l->selField = fi;
            break;
        }
        case T_BASE: {
            const char *h = attr(as, "href");
            if (h && !d->base[0]) snprintf(d->base, sizeof d->base, "%s", h);
            break;
        }
        case T_META: {
            const char *he = attr(as, "http-equiv");
            const char *ct = attr(as, "content");
            if (he && ct && !strcasecmp(he, "refresh")) {
                d->refreshDelay = atoi(ct);
                const char *u = strcasestr(ct, "url=");
                if (u) {
                    u += 4;
                    while (*u == '\'' || *u == '"' || *u == ' ') u++;
                    snprintf(d->refresh, sizeof d->refresh, "%s", u);
                    size_t n = strlen(d->refresh);
                    while (n && (d->refresh[n - 1] == '\'' || d->refresh[n - 1] == '"' || d->refresh[n - 1] == ' '))
                        d->refresh[--n] = 0;
                }
            }
            break;
        }
        case T_TR: case T_CAPTION: block(l, 0); break;
        case T_TABLE: block(l, 8); break;
        case T_DIV: case T_SECTION: case T_ARTICLE: case T_HEADER: case T_FOOTER: case T_NAV:
        case T_MAIN: case T_ASIDE: case T_FIGURE: case T_FIGCAPTION: case T_ADDRESS: case T_FIELDSET:
        case T_LEGEND: case T_DETAILS: case T_SUMMARY: case T_CENTER: case T_NOSCRIPT: case T_HGROUP:
        case T_DIALOG:
            block(l, id == T_DIV ? 0 : 4); break;
        case T_LABEL: l->pendSpace = true; break;
        default: break;
    }
}

/* finds "</name" (case-insensitive) from p; returns position of '<' or end */
static const char *find_close(const char *p, const char *end, const char *name) {
    size_t nl = strlen(name);
    while (p < end) {
        const char *lt = (const char *)memchr(p, '<', (size_t)(end - p));
        if (!lt) return end;
        if (lt + 2 + (long)nl <= end && lt[1] == '/' && !strncasecmp(lt + 2, name, nl) &&
            (lt + 2 + nl == end || !isalnum((unsigned char)lt[2 + nl])))
            return lt;
        p = lt + 1;
    }
    return end;
}

static const char *skip_past_gt(const char *p, const char *end) {
    const char *gt = (const char *)memchr(p, '>', (size_t)(end - p));
    return gt ? gt + 1 : end;
}

/* parses a tag starting at p (pointing at '<'). Returns pointer after it. */
static const char *parse_tag(L *l, const char *p, const char *end) {
    Doc *d = l->d;
    const char *s = p + 1;
    bool closing = false;
    if (s < end && *s == '/') { closing = true; s++; }
    char name[16];
    int k = 0;
    while (s < end && (isalnum((unsigned char)*s) || *s == '-' || *s == ':') ) {
        if (k < 15) name[k++] = (char)tolower((unsigned char)*s);
        s++;
    }
    name[k] = 0;
    if (!k) {   /* not a tag: treat '<' as text */
        put_cp(l, '<');
        return p + 1;
    }

    Attrs *asp = &l->as;
    #define as (*asp)
    as.n = 0;
    as.used = 0;
    bool selfClose = false;
    /* attributes */
    while (s < end && *s != '>') {
        if (*s == '/' ) { selfClose = true; s++; continue; }
        if (isspace((unsigned char)*s)) { s++; continue; }
        char an[16];
        int ak = 0;
        while (s < end && !isspace((unsigned char)*s) && *s != '=' && *s != '>' && *s != '/') {
            if (ak < 15) an[ak++] = (char)tolower((unsigned char)*s);
            s++;
        }
        an[ak] = 0;
        if (!ak) { s++; continue; }
        selfClose = false;
        while (s < end && isspace((unsigned char)*s)) s++;
        const char *vs = NULL, *ve = NULL;
        if (s < end && *s == '=') {
            s++;
            while (s < end && isspace((unsigned char)*s)) s++;
            if (s < end && (*s == '"' || *s == '\'')) {
                char q = *s++;
                vs = s;
                const char *qe = (const char *)memchr(s, q, (size_t)(end - s));
                if (!qe) qe = end;
                ve = qe;
                s = qe < end ? qe + 1 : end;
            } else {
                vs = s;
                while (s < end && !isspace((unsigned char)*s) && *s != '>') s++;
                ve = s;
            }
        }
        if (as.n < MAX_ATTRS) {
            Attr *a = &as.a[as.n];
            memcpy(a->name, an, (size_t)ak + 1);
            a->val = as.buf + as.used;
            size_t room = sizeof as.buf - as.used;
            if (room < 2) { a->val = as.buf + sizeof as.buf - 1; as.buf[sizeof as.buf - 1] = 0; }
            else if (vs) { decode_into(vs, ve, a->val, room > 2048 ? 2048 : room); as.used += strlen(a->val) + 1; }
            else { a->val[0] = 0; as.used += 1; }
            as.n++;
        }
    }
    const char *after = s < end ? s + 1 : end;
    int id = tag_id(name);

    /* skip mode for hidden elements */
    if (l->skipDepth) {
        if (!strcmp(name, l->skipTag) && !is_void(id)) {
            if (closing) l->skipDepth--;
            else if (!selfClose) l->skipDepth++;
        }
        /* still must step over raw text elements so their contents can't confuse us */
        if (!closing && (id == T_SCRIPT || id == T_STYLE || id == T_TEXTAREA || id == T_TITLE || id == T_XMP)) {
            const char *c = find_close(after, end, name);
            return skip_past_gt(c, end);
        }
        return after;
    }

    if (!closing) {
        switch (id) {
            case T_SCRIPT: case T_STYLE: case T_TEMPLATE: case T_SVG: case T_MATH: case T_CANVAS:
            case T_OBJECT: case T_IFRAME: case T_VIDEO: case T_AUDIO: {
                if (selfClose) return after;
                const char *c = find_close(after, end, name);
                return skip_past_gt(c, end);
            }
            case T_TITLE: {
                const char *c = find_close(after, end, name);
                if (!d->title[0]) {
                    char tmp[512];
                    const char *te = c - after > 1500 ? after + 1500 : c;
                    decode_into(after, te, tmp, sizeof tmp);
                    /* collapse whitespace */
                    size_t o = 0;
                    bool sp = true;
                    for (const char *q = tmp; *q && o + 1 < sizeof d->title; q++) {
                        bool ws = isspace((unsigned char)*q);
                        if (ws) { if (!sp) d->title[o++] = ' '; sp = true; }
                        else { d->title[o++] = *q; sp = false; }
                    }
                    while (o && d->title[o - 1] == ' ') o--;
                    d->title[o] = 0;
                }
                return skip_past_gt(c, end);
            }
            case T_TEXTAREA: {
                const char *c = find_close(after, end, name);
                if (!has_attr(&as, "hidden") && !css_hidden(attr(&as, "style"))) {
                    int fi = add_field(l, F_TEXTAREA, &as);
                    if (fi >= 0) {
                        size_t len = (size_t)(c - after);
                        if (len > 4000) len = 4000;
                        char *v = (char *)malloc(len * 3 + 1);
                        if (v) {
                            decode_into(after, after + len, v, len * 3 + 1);
                            free(d->fields[fi].value);
                            d->fields[fi].value = v;
                        }
                        inline_box(l, l->right - l->left, field_h(l), fi);
                    }
                }
                return skip_past_gt(c, end);
            }
            default: break;
        }
        if (!is_void(id) && !selfClose &&
            (has_attr(&as, "hidden") || css_hidden(attr(&as, "style")) ||
             ((attr(&as, "aria-hidden") && !strcasecmp(attr(&as, "aria-hidden"), "true")) && id != T_BODY && id != T_HTML))) {
            snprintf(l->skipTag, sizeof l->skipTag, "%s", name);
            l->skipDepth = 1;
            return after;
        }
    }
    handle_tag(l, id, closing, &as);
    if (!closing && selfClose && !is_void(id) && id != T_UNKNOWN) handle_tag(l, id, true, &as);
    return after;
    #undef as
}

void html_layout(Doc *d, const char *src, size_t n, bool plain, const LayoutCfg *cfg) {
    L *lp = (L *)calloc(1, sizeof(L));   /* ~8 KB: heap keeps it off the thread stack */
    if (!lp) { d->oom = true; return; }
    #define l (*lp)
    l.d = d;
    l.c = cfg;
    l.right = cfg->width;
    l.link = -1;
    l.form = -1;
    l.selField = -1;
    l.btnField = -1;
    pool_add(d, NULL, 0);

    const char *p = src, *end = src + n;
    if (plain) {
        l.pre = 1;
        l.plain = true;
        while (p < end && !d->oom) {
            const unsigned char *q = (const unsigned char *)p;
            uint32_t cp = html_u8dec(&q, (const unsigned char *)end);
            p = (const char *)q;
            put_cp(&l, cp);
        }
    } else {
        while (p < end && !d->oom) {
            char c = *p;
            if (c == '<') {
                if (end - p >= 4 && !strncmp(p, "<!--", 4)) {
                    const char *e = p + 4;
                    const char *f = NULL;
                    while (e + 2 < end) {
                        const char *dash = (const char *)memchr(e, '-', (size_t)(end - e));
                        if (!dash || dash + 2 >= end) break;
                        if (dash[1] == '-' && dash[2] == '>') { f = dash; break; }
                        e = dash + 1;
                    }
                    p = f ? f + 3 : end;
                    continue;
                }
                if (end - p >= 2 && (p[1] == '!' || p[1] == '?')) { p = skip_past_gt(p + 1, end); continue; }
                if (end - p >= 2 && (isalpha((unsigned char)p[1]) || p[1] == '/')) { p = parse_tag(&l, p, end); continue; }
                put_cp(&l, '<');
                p++;
                continue;
            }
            if (c == '&') {
                const char *q = p + 1;
                uint32_t cp = entity(&q, end);
                if (cp) { put_cp(&l, cp); p = q; continue; }
                put_cp(&l, '&');
                p++;
                continue;
            }
            const unsigned char *q = (const unsigned char *)p;
            uint32_t cp = html_u8dec(&q, (const unsigned char *)end);
            p = (const char *)q;
            put_cp(&l, cp);
        }
    }
    finish_button(&l);
    finish_select(&l);
    flush_word(&l);
    line_end(&l);
    d->height = l.y + 24;
    #undef l
    free(lp);
}
