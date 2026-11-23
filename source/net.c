/*
 * 3DBrowse by Meer - page loading on a background thread
 *
 * The UI thread never touches the network. It hands a job to the worker,
 * keeps drawing at 60 fps, and picks up a finished, already laid out Doc.
 * One curl handle is reused so keep-alive connections, DNS and cookies
 * carry over between pages, which makes following links on a site fast.
 */
#include "net.h"

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "store.h"

#define SOC_ALIGN     0x1000
#define SOC_BUFSIZE   0x100000
#define MAX_BODY      (3u << 20)
#define COOKIE_FILE   APP_DIR "/cookies.txt"
#define CA_FILE       "romfs:/cacert.pem"
#define USER_AGENT    "Mozilla/5.0 (Nintendo 3DS; U; ; en) Version/1.7630.US 3DBrowse/1.0"

typedef struct {
    char url[2048];
    char referer[1024];
    char *post;
    LayoutCfg cfg;
    bool retryHttp;
    int tag;
} Job;

typedef struct { char *d; size_t n, cap; bool truncated; } Buf;

static u32 *s_socBuf;
static Thread s_thread;
static LightLock s_lock;
static LightEvent s_event;
static volatile bool s_quit, s_cancel, s_busy;
static volatile int s_phase;
static volatile size_t s_now, s_total;
static volatile bool s_verify = true;
static bool s_haveCA;

static Job s_pending;
static bool s_hasPending;
static NetResult s_result;
static bool s_hasResult;

static CURL *s_curl;
static bool s_cookiesLoaded;

/* ------------------------------------------------------------ callbacks */

static size_t write_cb(char *p, size_t sz, size_t nm, void *u) {
    Buf *b = (Buf *)u;
    size_t l = sz * nm;
    if (s_cancel) return 0;
    if (b->n + l + 1 > MAX_BODY) { b->truncated = true; return 0; }
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64 * 1024;
        while (nc < b->n + l + 1) nc *= 2;
        if (nc > MAX_BODY) nc = MAX_BODY;
        char *nd = (char *)realloc(b->d, nc);
        if (!nd) { b->truncated = true; return 0; }
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->n, p, l);
    b->n += l;
    b->d[b->n] = 0;
    return l;
}

static int progress_cb(void *u, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)u; (void)ultotal; (void)ulnow;
    s_total = (size_t)(dltotal > 0 ? dltotal : 0);
    s_now = (size_t)(dlnow > 0 ? dlnow : 0);
    if (dlnow > 0) s_phase = NP_DOWNLOAD;
    return s_cancel ? 1 : 0;
}

/* -------------------------------------------------------------- results */

static void post_result(NetResult *r) {
    LightLock_Lock(&s_lock);
    if (s_cancel || s_hasPending) {
        /* a newer request replaced this one */
        LightLock_Unlock(&s_lock);
        doc_free(r->doc);
        return;
    }
    if (s_hasResult) doc_free(s_result.doc);
    s_result = *r;
    s_hasResult = true;
    LightLock_Unlock(&s_lock);
}

static void post_error(const Job *j, const char *title, const char *msg) {
    NetResult r;
    memset(&r, 0, sizeof r);
    snprintf(r.url, sizeof r.url, "%s", j->url);
    snprintf(r.errTitle, sizeof r.errTitle, "%s", title);
    snprintf(r.err, sizeof r.err, "%s", msg);
    r.tag = j->tag;
    post_result(&r);
}

/* ----------------------------------------------------------------- fetch */

static CURLcode perform(const Job *j, const char *url, Buf *b, long *code, char *ctype, size_t ctn, char *final, size_t fn) {
    CURL *c = s_curl;
    curl_easy_reset(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 32L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 65536L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    if (!s_cookiesLoaded) { curl_easy_setopt(c, CURLOPT_COOKIEFILE, COOKIE_FILE); s_cookiesLoaded = true; }
    else curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(c, CURLOPT_COOKIEJAR, COOKIE_FILE);

    if (s_verify && s_haveCA) {
        curl_easy_setopt(c, CURLOPT_CAINFO, CA_FILE);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    struct curl_slist *hdr = NULL;
    hdr = curl_slist_append(hdr, "Accept: text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.5");
    hdr = curl_slist_append(hdr, "Accept-Language: en-US,en;q=0.8");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    if (j->referer[0]) curl_easy_setopt(c, CURLOPT_REFERER, j->referer);
    if (j->post) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, j->post);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(j->post));
    }

    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, code);
    char *ct = NULL;
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
    snprintf(ctype, ctn, "%s", ct ? ct : "");
    char *eff = NULL;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
    snprintf(final, fn, "%s", eff ? eff : url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(hdr);
    return rc;
}

static const char *friendly(CURLcode rc) {
    switch (rc) {
        case CURLE_COULDNT_RESOLVE_HOST: return "Couldn't find that site. Check the address and your Wi-Fi connection.";
        case CURLE_COULDNT_CONNECT: return "The site didn't accept the connection. It may be down, or your 3DS may be offline.";
        case CURLE_OPERATION_TIMEDOUT: return "The site took too long to answer.";
        case CURLE_PEER_FAILED_VERIFICATION: return "The site's security certificate couldn't be verified. You can turn off certificate checks in the menu.";
        case CURLE_SSL_CONNECT_ERROR: return "A secure connection couldn't be set up. The site may need a newer TLS version than the 3DS supports.";
        case CURLE_TOO_MANY_REDIRECTS: return "The site kept redirecting in a loop.";
        case CURLE_URL_MALFORMAT: case CURLE_UNSUPPORTED_PROTOCOL: return "That address isn't valid.";
        default: return NULL;
    }
}

static void run_job(Job *j) {
    s_now = s_total = 0;
    s_phase = NP_CONNECT;
    Buf b = {0};
    long code = 0;
    char ctype[128], final[2048];
    char url[2048];
    snprintf(url, sizeof url, "%s", j->url);

    CURLcode rc = perform(j, url, &b, &code, ctype, sizeof ctype, final, sizeof final);
    if (rc == CURLE_WRITE_ERROR && b.truncated && b.n) rc = CURLE_OK;   /* page too big: show what we have */
    if (s_cancel) { free(b.d); return; }

    if (rc != CURLE_OK && j->retryHttp && !strncasecmp(url, "https://", 8) &&
        (rc == CURLE_COULDNT_CONNECT || rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_PEER_FAILED_VERIFICATION ||
         rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR)) {
        snprintf(url, sizeof url, "http://%s", j->url + 8);
        free(b.d);
        memset(&b, 0, sizeof b);
        s_phase = NP_CONNECT;
        rc = perform(j, url, &b, &code, ctype, sizeof ctype, final, sizeof final);
        if (rc == CURLE_WRITE_ERROR && b.truncated && b.n) rc = CURLE_OK;
        if (s_cancel) { free(b.d); return; }
    }

    if (rc != CURLE_OK) {
        const char *f = friendly(rc);
        char msg[200];
        if (f) snprintf(msg, sizeof msg, "%s", f);
        else snprintf(msg, sizeof msg, "%s (error %d)", curl_easy_strerror(rc), (int)rc);
        free(b.d);
        post_error(j, "Can't open this page", msg);
        return;
    }

    bool html = true, plain = false;
    if (ctype[0]) {
        if (strcasestr(ctype, "html") || strcasestr(ctype, "xml")) html = true;
        else if (!strncasecmp(ctype, "text/", 5) || strcasestr(ctype, "json") || strcasestr(ctype, "javascript")) plain = true;
        else html = false;
    }
    if (!ctype[0] && b.n) {
        /* sniff */
        size_t i = 0;
        while (i < b.n && i < 512 && (b.d[i] == ' ' || b.d[i] == '\n' || b.d[i] == '\r' || b.d[i] == '\t')) i++;
        plain = !(i < b.n && b.d[i] == '<');
    }
    if (!html && !plain) {
        char msg[200];
        char t[64];
        snprintf(t, sizeof t, "%s", ctype);
        char *semi = strchr(t, ';');
        if (semi) *semi = 0;
        snprintf(msg, sizeof msg, "This link points to a file (%s) that 3DBrowse can't show.", t);
        free(b.d);
        post_error(j, "Can't display this file", msg);
        return;
    }
    if (!b.n) {
        free(b.d);
        char msg[96];
        snprintf(msg, sizeof msg, "The site sent an empty page (HTTP %ld).", code);
        post_error(j, "Empty page", msg);
        return;
    }

    s_phase = NP_RENDER;
    Doc *d = doc_new();
    if (!d) { free(b.d); post_error(j, "Out of memory", "The page is too large for the 3DS's memory."); return; }
    size_t ulen = 0;
    d->raw = html_to_utf8(b.d, b.n, ctype, &ulen);
    d->rawLen = ulen;
    bool truncated = b.truncated;
    free(b.d);
    if (!d->raw) { doc_free(d); post_error(j, "Out of memory", "The page is too large for the 3DS's memory."); return; }
    if (s_cancel) { doc_free(d); return; }
    d->plain = plain;
    html_layout(d, d->raw, d->rawLen, plain, &j->cfg);
    snprintf(d->url, sizeof d->url, "%s", final);
    d->status = (int)code;
    d->truncated = truncated;

    NetResult r;
    memset(&r, 0, sizeof r);
    r.doc = d;
    snprintf(r.url, sizeof r.url, "%s", final);
    r.tag = j->tag;
    post_result(&r);
}

static void worker(void *arg) {
    (void)arg;
    while (!s_quit) {
        LightEvent_Wait(&s_event);
        for (;;) {
            if (s_quit) return;
            Job j;
            bool have = false;
            LightLock_Lock(&s_lock);
            if (s_hasPending) {
                j = s_pending;
                s_pending.post = NULL;
                s_hasPending = false;
                s_cancel = false;
                have = true;
            }
            LightLock_Unlock(&s_lock);
            if (!have) break;
            run_job(&j);
            free(j.post);
        }
        LightLock_Lock(&s_lock);
        if (!s_hasPending) { s_busy = false; s_phase = NP_IDLE; }
        LightLock_Unlock(&s_lock);
    }
}

/* ------------------------------------------------------------------ api */

bool net_init(void) {
    s_socBuf = (u32 *)memalign(SOC_ALIGN, SOC_BUFSIZE);
    if (!s_socBuf || R_FAILED(socInit(s_socBuf, SOC_BUFSIZE))) return false;
    curl_global_init(CURL_GLOBAL_ALL);
    s_curl = curl_easy_init();
    FILE *f = fopen(CA_FILE, "r");
    if (f) { s_haveCA = true; fclose(f); }
    LightLock_Init(&s_lock);
    LightEvent_Init(&s_event, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio += 1;
    if (prio > 0x3F) prio = 0x3F;
    s_thread = threadCreate(worker, NULL, 256 * 1024, prio, -2, false);
    return s_thread != NULL && s_curl != NULL;
}

void net_exit(void) {
    s_cancel = true;
    s_quit = true;
    LightEvent_Signal(&s_event);
    if (s_thread) {
        threadJoin(s_thread, 10ULL * 1000 * 1000 * 1000);
        threadFree(s_thread);
        s_thread = NULL;
    }
    if (s_curl) {
        if (s_cookiesLoaded) curl_easy_setopt(s_curl, CURLOPT_COOKIELIST, "FLUSH");
        curl_easy_cleanup(s_curl);
        s_curl = NULL;
    }
    if (s_hasResult) { doc_free(s_result.doc); s_hasResult = false; }
    free(s_pending.post);
    s_pending.post = NULL;
    curl_global_cleanup();
    socExit();
    free(s_socBuf);
}

void net_fetch(const char *url, const char *post, const char *referer, const LayoutCfg *cfg, bool retryHttp, int tag) {
    LightLock_Lock(&s_lock);
    if (s_busy) s_cancel = true;
    free(s_pending.post);
    memset(&s_pending, 0, sizeof s_pending);
    snprintf(s_pending.url, sizeof s_pending.url, "%s", url);
    if (referer) snprintf(s_pending.referer, sizeof s_pending.referer, "%s", referer);
    s_pending.post = post ? strdup(post) : NULL;
    s_pending.cfg = *cfg;
    s_pending.retryHttp = retryHttp;
    s_pending.tag = tag;
    s_hasPending = true;
    if (s_hasResult) { doc_free(s_result.doc); s_hasResult = false; }
    s_busy = true;
    s_phase = NP_CONNECT;
    s_now = s_total = 0;
    LightLock_Unlock(&s_lock);
    LightEvent_Signal(&s_event);
}

void net_cancel(void) {
    LightLock_Lock(&s_lock);
    s_cancel = true;
    if (s_hasPending) { free(s_pending.post); s_pending.post = NULL; s_hasPending = false; }
    LightLock_Unlock(&s_lock);
}

bool net_busy(void) { return s_busy; }

bool net_take(NetResult *out) {
    bool got = false;
    LightLock_Lock(&s_lock);
    if (s_hasResult) {
        *out = s_result;
        s_hasResult = false;
        got = true;
    }
    LightLock_Unlock(&s_lock);
    return got;
}

int net_phase(void) { return s_phase; }
size_t net_bytes(void) { return s_now; }
size_t net_total(void) { return s_total; }
void net_set_verify(bool on) { s_verify = on; }

void net_clear_cookies(void) {
    if (!s_busy && s_curl) curl_easy_setopt(s_curl, CURLOPT_COOKIELIST, "ALL");
    remove(COOKIE_FILE);
}
