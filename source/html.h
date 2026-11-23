/*
 * 3DBrowse by Meer - HTML parser and text layout engine
 *
 * Turns an HTML document into a flat list of positioned text runs and boxes
 * that the renderer can draw directly. It has no 3DS dependencies so it can
 * be compiled and tested on a PC (see tools/preview_host.c).
 */
#ifndef HTML_H
#define HTML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* run style flags */
enum {
    RS_BOLD   = 1 << 0,
    RS_ITALIC = 1 << 1,
    RS_MONO   = 1 << 2,
    RS_LINK   = 1 << 3,
    RS_MUTED  = 1 << 4,
    RS_HEAD   = 1 << 5,
    RS_STRIKE = 1 << 6,
    RS_UNDER  = 1 << 7,
};

/* box kinds */
enum { BX_HR = 1, BX_QUOTE, BX_BULLET, BX_FIELD };

/* form field types */
enum { F_TEXT = 1, F_PASSWORD, F_HIDDEN, F_SUBMIT, F_CHECK, F_RADIO, F_SELECT, F_TEXTAREA, F_BUTTON };

typedef struct {
    float x, y, w, h, scale;
    uint32_t text;   /* offset into Doc.text (NUL terminated) */
    uint16_t len;
    uint16_t style;
    int32_t link;    /* index into Doc.links, -1 if none */
} Run;

typedef struct {
    float x, y, w, h;
    uint8_t kind;
    uint8_t inl;     /* 0 = absolute, 1 = inline item, 2 = list bullet */
    int32_t ref;     /* field index for BX_FIELD */
} Box;

typedef struct {
    int form;
    uint8_t type;
    bool checked;
    uint32_t name, label, placeholder;   /* pool offsets */
    char *value;                         /* malloc'd, editable */
    uint32_t *optv, *optl;               /* pool offsets of option values / labels */
    int nopt, sel;
    int box;                             /* box index, -1 for hidden fields */
} Field;

typedef struct {
    uint32_t action;  /* pool offset */
    bool post;
} Form;

typedef struct {
    float (*measure)(const char *s, int n, float scale);
    float lineFeed;   /* line height in px at scale 1 */
    float width;      /* layout width in px */
    float base;       /* base text scale */
} LayoutCfg;

typedef struct Doc {
    char url[2048];
    char title[128];
    char base[1024];
    char refresh[1024];
    int refreshDelay;

    char *text;  size_t tlen, tcap;
    char *pool;  size_t plen, pcap;
    Run *runs;   int nruns, cruns;
    Box *boxes;  int nboxes, cboxes;
    uint32_t *links; int nlinks, clinks;
    Field *fields; int nfields, cfields;
    Form *forms; int nforms, cforms;

    float height;
    char *raw; size_t rawLen; bool plain;   /* kept so the page can be re-flowed */
    bool truncated, oom;
    int status;
} Doc;

Doc *doc_new(void);
void doc_free(Doc *d);
void doc_clear_layout(Doc *d);
static inline const char *doc_str(const Doc *d, uint32_t off) { return d->pool ? d->pool + off : ""; }
static inline const char *doc_text(const Doc *d, const Run *r) { return d->text + r->text; }

/* Lays out src (UTF-8). plain = treat as text/plain. */
void html_layout(Doc *d, const char *src, size_t n, bool plain, const LayoutCfg *cfg);

/* Converts a downloaded body to UTF-8 using the Content-Type charset or a
 * <meta charset>. Returns a malloc'd NUL-terminated buffer. */
char *html_to_utf8(const char *src, size_t n, const char *ctype, size_t *outLen);

/* Decodes one UTF-8 code point (invalid bytes give U+FFFD). */
uint32_t html_u8dec(const unsigned char **p, const unsigned char *end);
int html_u8enc(uint32_t cp, char *out);

/* Escapes text for use inside generated HTML. */
void html_escape(const char *in, char *out, size_t n);

#endif
