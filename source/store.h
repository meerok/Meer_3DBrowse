/* 3DBrowse by Meer - bookmarks, history and settings on the SD card */
#ifndef STORE_H
#define STORE_H

#include <stdbool.h>

#define APP_DIR   "sdmc:/3ds/3DBrowse"
#define MAX_BM    200
#define MAX_HIST  300

typedef struct {
    char title[96];
    char url[1024];
} Entry;

typedef struct {
    int textSize;   /* 0 small, 1 medium, 2 large */
    int theme;      /* 0 light, 1 dark */
    int verify;     /* verify HTTPS certificates */
} Settings;

extern Entry g_bm[MAX_BM];
extern int g_nbm;
extern Entry g_hist[MAX_HIST];   /* newest first */
extern int g_nhist;
extern Settings g_cfg;

void store_init(void);
void store_save(void);
void settings_save(void);

int  bm_find(const char *url);
bool bm_toggle(const char *url, const char *title);   /* returns true if now bookmarked */
void bm_remove(int i);
void bm_save(void);

void hist_add(const char *url, const char *title);
void hist_remove(int i);
void hist_clear(void);

#endif
