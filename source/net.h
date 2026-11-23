/* 3DBrowse by Meer - page loading on a background thread */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>
#include "html.h"

typedef struct {
    Doc *doc;           /* laid out page, or NULL on error */
    char url[2048];     /* final URL after redirects (or the requested URL on error) */
    char err[200];      /* error message when doc == NULL */
    char errTitle[64];
    int tag;            /* caller's value from net_fetch */
} NetResult;

enum { NP_IDLE, NP_CONNECT, NP_DOWNLOAD, NP_RENDER };

bool net_init(void);
void net_exit(void);

/* Starts loading url (cancels a load in progress). post may be NULL.
 * retryHttp: fall back to http:// if https:// can't connect (for typed addresses). */
void net_fetch(const char *url, const char *post, const char *referer, const LayoutCfg *cfg, bool retryHttp, int tag);
void net_cancel(void);
bool net_busy(void);
bool net_take(NetResult *out);

int net_phase(void);
size_t net_bytes(void);
size_t net_total(void);

void net_set_verify(bool on);
void net_clear_cookies(void);

#endif
