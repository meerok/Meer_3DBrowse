/* 3DBrowse by Meer - URL helpers */
#include "url.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SEARCH_URL "https://lite.duckduckgo.com/lite/?q="

bool url_is_https(const char *u) { return !strncasecmp(u, "https://", 8); }
bool url_is_internal(const char *u) { return !strncasecmp(u, "about:", 6); }

static bool has_scheme(const char *s) {
    if (!isalpha((unsigned char)s[0])) return false;
    for (const char *p = s + 1; *p; p++) {
        if (*p == ':') return true;
        if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.') return false;
    }
    return false;
}

/* removes "." and ".." segments from an absolute path (in place) */
static void remove_dots(char *path) {
    char *segs[256];
    int n = 0;
    bool trailing = false;
    char *p = path;
    if (*p == '/') p++;
    while (*p && n < 256) {
        char *s = p;
        char *slash = strchr(p, '/');
        if (slash) { *slash = 0; p = slash + 1; trailing = true; }
        else { p += strlen(p); trailing = false; }
        if (!strcmp(s, ".")) { trailing = true; continue; }
        if (!strcmp(s, "..")) { if (n) n--; trailing = true; continue; }
        segs[n++] = s;
    }
    char tmp[2048];
    size_t o = 0;
    tmp[o++] = '/';
    for (int i = 0; i < n; i++) {
        size_t l = strlen(segs[i]);
        if (o + l + 2 >= sizeof tmp) break;
        memcpy(tmp + o, segs[i], l);
        o += l;
        if (i < n - 1 || trailing) tmp[o++] = '/';
    }
    if (n == 0) o = 1;
    tmp[o] = 0;
    strcpy(path, tmp);
}

/* encodes characters that are not valid in a URL */
static void sanitize(const char *in, char *out, size_t n) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        unsigned char c = *p;
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '`' || c == '{' || c == '}' || c == '|' || c == '\\' || c == '^') {
            out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15];
        } else out[o++] = (char)c;
    }
    out[o] = 0;
}

bool url_resolve(const char *base, const char *relIn, char *out, size_t n) {
    char rel[2048];
    /* trim */
    while (*relIn && isspace((unsigned char)*relIn)) relIn++;
    snprintf(rel, sizeof rel, "%s", relIn);
    size_t rl = strlen(rel);
    while (rl && isspace((unsigned char)rel[rl - 1])) rel[--rl] = 0;

    char res[2048];
    if (has_scheme(rel)) {
        snprintf(res, sizeof res, "%s", rel);
        /* make sure "http://host" gets a path */
        char *ss = strstr(res, "://");
        if (ss && !strchr(ss + 3, '/') && strlen(res) + 2 < sizeof res) {
            char *q = strpbrk(ss + 3, "?#");
            if (q) { memmove(q + 1, q, strlen(q) + 1); *q = '/'; }
            else strcat(res, "/");
        }
    } else {
        /* split base */
        char b[2048];
        snprintf(b, sizeof b, "%s", base ? base : "");
        char *h = strchr(b, '#');
        if (h) *h = 0;
        char *ss = strstr(b, "://");
        if (!ss) { snprintf(out, n, "%s", rel); return false; }
        char scheme[16];
        size_t sl = (size_t)(ss - b);
        if (sl >= sizeof scheme) sl = sizeof scheme - 1;
        memcpy(scheme, b, sl);
        scheme[sl] = 0;
        char *auth = ss + 3;
        char *path = strchr(auth, '/');
        char *qm = strchr(auth, '?');
        if (qm && (!path || qm < path)) path = NULL;
        char authority[512];
        size_t al = path ? (size_t)(path - auth) : (qm ? (size_t)(qm - auth) : strlen(auth));
        if (al >= sizeof authority) al = sizeof authority - 1;
        memcpy(authority, auth, al);
        authority[al] = 0;
        char bpath[2048] = "/";
        if (path) {
            snprintf(bpath, sizeof bpath, "%s", path);
        } else if (qm) {
            snprintf(bpath, sizeof bpath, "/%s", qm);
        }
        char *bq = strchr(bpath, '?');

        if (rel[0] == 0) {
            snprintf(res, sizeof res, "%s://%s%s", scheme, authority, bpath);
        } else if (rel[0] == '/' && rel[1] == '/') {
            snprintf(res, sizeof res, "%s:%s", scheme, rel);
            char *s2 = res + strlen(scheme) + 3;
            if (!strchr(s2, '/') && strlen(res) + 2 < sizeof res) strcat(res, "/");
        } else if (rel[0] == '#') {
            snprintf(res, sizeof res, "%s://%s%s", scheme, authority, bpath);
        } else if (rel[0] == '?') {
            if (bq) *bq = 0;
            snprintf(res, sizeof res, "%s://%s%s%s", scheme, authority, bpath, rel);
        } else {
            char p[2048];
            if (rel[0] == '/') {
                snprintf(p, sizeof p, "%s", rel);
            } else {
                if (bq) *bq = 0;
                char *last = strrchr(bpath, '/');
                if (last) last[1] = 0;
                snprintf(p, sizeof p, "%s%s", bpath, rel);
            }
            char *pq = strpbrk(p, "?#");
            char qpart[2048] = "";
            if (pq) { snprintf(qpart, sizeof qpart, "%s", pq); *pq = 0; }
            remove_dots(p);
            snprintf(res, sizeof res, "%s://%s%s%s", scheme, authority, p, qpart);
        }
    }
    char *h = strchr(res, '#');
    if (h) *h = 0;
    sanitize(res, out, n);
    return true;
}

void url_encode(const char *s, char *out, size_t n) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < n; p++) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '*') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else if (c == '\n') { if (o + 7 < n) { memcpy(out + o, "%0D%0A", 6); o += 6; } }
        else if (c == '\r') continue;
        else { out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15]; }
    }
    out[o] = 0;
}

void url_host(const char *url, char *out, size_t n) {
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    const char *at = NULL;
    for (const char *q = s; *q && *q != '/' && *q != '?' && *q != '#'; q++) if (*q == '@') at = q;
    if (at) s = at + 1;
    size_t o = 0;
    while (s[o] && s[o] != '/' && s[o] != '?' && s[o] != '#' && s[o] != ':' && o + 1 < n) { out[o] = s[o]; o++; }
    out[o] = 0;
    if (o > 4 && !strncasecmp(out, "www.", 4)) memmove(out, out + 4, strlen(out + 4) + 1);
}

void url_from_input(const char *inRaw, char *out, size_t n, bool *typedScheme) {
    char in[1024];
    while (*inRaw && isspace((unsigned char)*inRaw)) inRaw++;
    snprintf(in, sizeof in, "%s", inRaw);
    size_t l = strlen(in);
    while (l && isspace((unsigned char)in[l - 1])) in[--l] = 0;
    if (typedScheme) *typedScheme = true;

    if (!strncasecmp(in, "about:", 6)) { snprintf(out, n, "%s", in); return; }
    if (!strncasecmp(in, "http://", 7) || !strncasecmp(in, "https://", 8)) { url_resolve(NULL, in, out, n); return; }

    bool hasSpace = strchr(in, ' ') != NULL;
    bool hasDot = strchr(in, '.') != NULL;
    bool localhost = !strncasecmp(in, "localhost", 9);
    if (!hasSpace && (hasDot || localhost) && in[0] != '.' && in[l ? l - 1 : 0] != '.') {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "https://%s", in);
        url_resolve(NULL, tmp, out, n);
        if (typedScheme) *typedScheme = false;
        return;
    }
    char enc[3072];
    url_encode(in, enc, sizeof enc);
    snprintf(out, n, "%s%s", SEARCH_URL, enc);
}
