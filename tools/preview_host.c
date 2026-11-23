/*
 * Runs the 3DBrowse layout engine on a PC and prints the result, so the
 * engine can be tested without a 3DS and tools/make_assets.py can draw the
 * README preview from real engine output.
 *
 *   cc -O2 -Isource tools/preview_host.c source/html.c source/url.c -lm -o preview_host
 *   ./preview_host widths.txt page.html 300 0.5
 *
 * widths.txt: first line = line height at scale 1, then 95 advance widths
 * (at scale 1) for ASCII 32..126. Other characters use the '0' width.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "html.h"

static float W[128];

static float measure(const char *s, int n, float scale) {
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    float w = 0;
    while (p < e) {
        uint32_t cp = html_u8dec(&p, e);
        w += (cp >= 32 && cp < 127) ? W[cp] : W['0'];
    }
    return w * scale;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s widths.txt page.html width scale [plain]\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "r");
    if (!f) return 1;
    float lf = 30;
    if (fscanf(f, "%f", &lf) != 1) return 1;
    for (int c = 32; c < 127; c++) if (fscanf(f, "%f", &W[c]) != 1) return 1;
    fclose(f);

    f = fopen(argv[2], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    size_t un;
    char *u = html_to_utf8(buf, (size_t)n, NULL, &un);

    LayoutCfg cfg = {measure, lf, (float)atof(argv[3]), (float)atof(argv[4])};
    Doc *d = doc_new();
    html_layout(d, u, un, argc > 5, &cfg);
    printf("T\t%s\n", d->title);
    printf("H\t%.1f\n", d->height);
    for (int i = 0; i < d->nruns; i++) {
        Run *r = &d->runs[i];
        printf("R\t%.1f\t%.1f\t%.1f\t%.1f\t%.3f\t%d\t%d\t%s\n", r->x, r->y, r->w, r->h, r->scale, r->style, r->link, doc_text(d, r));
    }
    for (int i = 0; i < d->nboxes; i++) {
        Box *b = &d->boxes[i];
        const char *label = "";
        if (b->kind == BX_FIELD && b->ref >= 0) {
            Field *fl = &d->fields[b->ref];
            if (fl->type == F_SUBMIT || fl->type == F_BUTTON) label = doc_str(d, fl->label);
            else if (fl->type == F_SELECT && fl->nopt) label = doc_str(d, fl->optl[fl->sel]);
            else label = fl->value[0] ? fl->value : doc_str(d, fl->placeholder);
            printf("B\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%d\t%s\n", b->kind, b->x, b->y, b->w, b->h, fl->type, label);
        } else printf("B\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%d\t\n", b->kind, b->x, b->y, b->w, b->h, b->ref);
    }
    for (int i = 0; i < d->nlinks; i++) printf("L\t%d\t%s\n", i, doc_str(d, d->links[i]));
    fprintf(stderr, "runs=%d boxes=%d links=%d fields=%d forms=%d text=%zu oom=%d refresh=%s\n", d->nruns, d->nboxes,
            d->nlinks, d->nfields, d->nforms, d->tlen, d->oom, d->refresh);
    doc_free(d);
    free(u);
    free(buf);
    return 0;
}
