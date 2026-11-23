/* 3DBrowse by Meer - built-in pages (about:home, about:bookmarks, ...) */
#include "pages.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html.h"
#include "store.h"

typedef struct { char *s; size_t n, cap; } SB;

static void sb_add(SB *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8192;
        while (nc < b->n + l + 1) nc *= 2;
        char *ns = (char *)realloc(b->s, nc);
        if (!ns) return;
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, l + 1);
    b->n += l;
}

static void sb_fmt(SB *b, const char *fmt, ...) {
    char tmp[3072];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_add(b, tmp);
}

static void sb_esc(SB *b, const char *s) {
    char tmp[3072];
    html_escape(s, tmp, sizeof tmp);
    sb_add(b, tmp);
}

static void entry_link(SB *b, const Entry *e) {
    sb_add(b, "<li><a href=\"");
    sb_esc(b, e->url);
    sb_add(b, "\">");
    sb_esc(b, e->title[0] ? e->title : e->url);
    sb_add(b, "</a></li>");
}

static const struct { const char *name, *url, *what; } QUICK[] = {
    {"Wikipedia", "https://en.m.wikipedia.org/", "the free encyclopedia"},
    {"68k.news", "http://68k.news/", "headlines, text only"},
    {"Text-only NPR", "https://text.npr.org/", "news"},
    {"CNN Lite", "https://lite.cnn.com/", "news"},
    {"Hacker News", "https://news.ycombinator.com/", "tech links"},
    {"FrogFind", "http://frogfind.com/", "search that simplifies pages"},
    {"GBAtemp", "https://gbatemp.net/", "homebrew forum"},
};

static char *home(void) {
    SB b = {0};
    sb_add(&b, "<title>Start</title>"
               "<h1>3DBrowse</h1>"
               "<form action=\"https://lite.duckduckgo.com/lite/\" method=\"get\">"
               "<input type=\"search\" name=\"q\" placeholder=\"Search DuckDuckGo\"> "
               "<input type=\"submit\" value=\"Search\"></form>");
    if (g_nbm) {
        sb_add(&b, "<h3>Bookmarks</h3><ul>");
        for (int i = 0; i < g_nbm && i < 8; i++) entry_link(&b, &g_bm[i]);
        sb_add(&b, "</ul>");
        if (g_nbm > 8) sb_fmt(&b, "<p><a href=\"about:bookmarks\">All %d bookmarks</a></p>", g_nbm);
    }
    sb_add(&b, "<h3>Start here</h3><ul>");
    for (size_t i = 0; i < sizeof QUICK / sizeof QUICK[0]; i++)
        sb_fmt(&b, "<li><a href=\"%s\">%s</a> <small>%s</small></li>", QUICK[i].url, QUICK[i].name, QUICK[i].what);
    sb_add(&b, "</ul>");
    if (g_nhist) {
        sb_add(&b, "<h3>Recently visited</h3><ul>");
        for (int i = 0; i < g_nhist && i < 5; i++) entry_link(&b, &g_hist[i]);
        sb_add(&b, "</ul><p><a href=\"about:history\">Full history</a></p>");
    }
    sb_add(&b, "<hr><p><small>Y address bar &middot; X menu &middot; L back &middot; R forward &middot; "
               "A open link &middot; D-pad pick a link</small></p>");
    return b.s;
}

static char *bookmarks(void) {
    SB b = {0};
    sb_add(&b, "<title>Bookmarks</title><h2>Bookmarks</h2>");
    if (!g_nbm) sb_add(&b, "<p>No bookmarks yet. Tap the star in the toolbar to save the page you're on.</p>");
    for (int i = 0; i < g_nbm; i++) {
        sb_add(&b, "<p><a href=\"");
        sb_esc(&b, g_bm[i].url);
        sb_add(&b, "\"><b>");
        sb_esc(&b, g_bm[i].title);
        sb_add(&b, "</b></a><br><small>");
        sb_esc(&b, g_bm[i].url);
        sb_fmt(&b, "</small> &nbsp;<a href=\"about:bookmarks?del=%d\"><small>remove</small></a></p>", i);
    }
    sb_add(&b, "<hr><p><a href=\"about:home\">Back to start</a></p>");
    return b.s;
}

static char *history(void) {
    SB b = {0};
    sb_add(&b, "<title>History</title><h2>History</h2>");
    if (!g_nhist) sb_add(&b, "<p>Nothing here yet.</p>");
    else sb_add(&b, "<p><a href=\"about:history?clear=1\">Clear all history</a></p><ul>");
    for (int i = 0; i < g_nhist; i++) entry_link(&b, &g_hist[i]);
    if (g_nhist) sb_add(&b, "</ul>");
    sb_add(&b, "<hr><p><a href=\"about:home\">Back to start</a></p>");
    return b.s;
}

static char *about(void) {
    SB b = {0};
    sb_add(&b, "<title>About</title><h2>3DBrowse</h2>");
    sb_fmt(&b, "<p>Version %s &middot; made by <b>Meer</b></p>", APP_VERSION);
    sb_add(&b, "<p>A fast, text-first web browser for the Nintendo 3DS. Pages are read straight from the "
               "HTML and laid out as clean text, so they load quickly and the app stays stable. "
               "JavaScript, CSS layouts and images aren't supported; image descriptions are shown instead.</p>"
               "<h3>Controls</h3><table>"
               "<tr><td><b>Circle Pad</b></td><td>Scroll</td></tr>"
               "<tr><td><b>D-Pad Up/Down</b></td><td>Pick a link</td></tr>"
               "<tr><td><b>D-Pad Left/Right</b></td><td>Page up / down</td></tr>"
               "<tr><td><b>A</b></td><td>Open link / edit field</td></tr>"
               "<tr><td><b>B</b></td><td>Stop loading, or back</td></tr>"
               "<tr><td><b>L / R</b></td><td>Back / forward</td></tr>"
               "<tr><td><b>ZL / ZR</b></td><td>Top / bottom of page</td></tr>"
               "<tr><td><b>Y</b></td><td>Address bar</td></tr>"
               "<tr><td><b>X</b></td><td>Menu</td></tr>"
               "<tr><td><b>SELECT</b></td><td>Bookmarks</td></tr>"
               "<tr><td><b>START</b></td><td>Exit</td></tr></table>"
               "<h3>Files</h3><p>Bookmarks, history, cookies and settings live in <code>sdmc:/3ds/3DBrowse/</code>.</p>"
               "<h3>Source</h3><p><a href=\"https://github.com/meerok/Meer_3DBrowse\">github.com/meerok/Meer_3DBrowse</a></p>"
               "<p><small>Built with libctru, citro2d, libcurl and mbedTLS. Search by DuckDuckGo Lite. "
               "Reader view by FrogFind.</small></p>");
    return b.s;
}

char *page_error(const char *url, const char *title, const char *msg) {
    SB b = {0};
    sb_add(&b, "<title>");
    sb_esc(&b, title);
    sb_add(&b, "</title><h2>");
    sb_esc(&b, title);
    sb_add(&b, "</h2><p>");
    sb_esc(&b, msg);
    sb_add(&b, "</p><p><small>");
    sb_esc(&b, url);
    sb_add(&b, "</small></p><p><a href=\"");
    sb_esc(&b, url);
    sb_add(&b, "\">Try again</a> &nbsp;&middot;&nbsp; <a href=\"about:home\">Start page</a></p>");
    return b.s;
}

char *page_internal(const char *url) {
    const char *p = url + 6;   /* after "about:" */
    if (!strncmp(p, "bookmarks", 9)) {
        const char *del = strstr(p, "del=");
        if (del) bm_remove(atoi(del + 4));
        return bookmarks();
    }
    if (!strncmp(p, "history", 7)) {
        if (strstr(p, "clear=1")) hist_clear();
        return history();
    }
    if (!strncmp(p, "about", 5)) return about();
    return home();
}
