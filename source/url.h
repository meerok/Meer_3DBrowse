/* 3DBrowse by Meer - URL helpers */
#ifndef URL_H
#define URL_H

#include <stdbool.h>
#include <stddef.h>

/* Resolves rel against base into out (fragment removed, spaces encoded). */
bool url_resolve(const char *base, const char *rel, char *out, size_t n);

/* application/x-www-form-urlencoded encoding of one component */
void url_encode(const char *s, char *out, size_t n);

/* Extracts the host ("en.m.wikipedia.org") */
void url_host(const char *url, char *out, size_t n);

/* Turns what the user typed in the address bar into a URL or a search. */
void url_from_input(const char *in, char *out, size_t n, bool *typedScheme);

bool url_is_https(const char *u);
bool url_is_internal(const char *u);

#endif
