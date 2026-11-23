/* 3DBrowse by Meer - built-in pages (about:home, about:bookmarks, ...) */
#ifndef PAGES_H
#define PAGES_H

#define APP_VERSION "1.0.0"

/* Builds the HTML for an about: URL. Returns malloc'd HTML (never NULL unless out of memory). */
char *page_internal(const char *url);
char *page_error(const char *url, const char *title, const char *msg);

#endif
