/* 3DBrowse by Meer - bookmarks, history and settings on the SD card */
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BM_FILE   APP_DIR "/bookmarks.txt"
#define HIST_FILE APP_DIR "/history.txt"
#define CFG_FILE  APP_DIR "/settings.txt"

Entry g_bm[MAX_BM];
int g_nbm = 0;
Entry g_hist[MAX_HIST];
int g_nhist = 0;
Settings g_cfg = {1, 0, 1};

static int s_histDirty = 0;

static void clean(char *s) {
    for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static int load_list(const char *path, Entry *list, int max) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1200];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l || line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (tab) *tab = 0;
        snprintf(list[n].url, sizeof list[n].url, "%s", line);
        snprintf(list[n].title, sizeof list[n].title, "%s", tab ? tab + 1 : line);
        if (list[n].url[0]) n++;
    }
    fclose(f);
    return n;
}

static void save_list(const char *path, const Entry *list, int n) {
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s\t%s\n", list[i].url, list[i].title);
    fclose(f);
    remove(path);
    rename(tmp, path);
}

void settings_save(void) {
    FILE *f = fopen(CFG_FILE, "w");
    if (!f) return;
    fprintf(f, "textsize=%d\ntheme=%d\nverify=%d\n", g_cfg.textSize, g_cfg.theme, g_cfg.verify);
    fclose(f);
}

void store_init(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(APP_DIR, 0777);
    g_nbm = load_list(BM_FILE, g_bm, MAX_BM);
    g_nhist = load_list(HIST_FILE, g_hist, MAX_HIST);
    FILE *f = fopen(CFG_FILE, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            int v;
            if (sscanf(line, "textsize=%d", &v) == 1) g_cfg.textSize = v;
            else if (sscanf(line, "theme=%d", &v) == 1) g_cfg.theme = v;
            else if (sscanf(line, "verify=%d", &v) == 1) g_cfg.verify = v;
        }
        fclose(f);
    }
    if (g_cfg.textSize < 0 || g_cfg.textSize > 2) g_cfg.textSize = 1;
    if (g_cfg.theme < 0 || g_cfg.theme > 1) g_cfg.theme = 0;
}

void bm_save(void) { save_list(BM_FILE, g_bm, g_nbm); }

void store_save(void) {
    bm_save();
    save_list(HIST_FILE, g_hist, g_nhist);
    s_histDirty = 0;
    settings_save();
}

int bm_find(const char *url) {
    for (int i = 0; i < g_nbm; i++)
        if (!strcmp(g_bm[i].url, url)) return i;
    return -1;
}

void bm_remove(int i) {
    if (i < 0 || i >= g_nbm) return;
    memmove(&g_bm[i], &g_bm[i + 1], sizeof(Entry) * (size_t)(g_nbm - i - 1));
    g_nbm--;
    bm_save();
}

bool bm_toggle(const char *url, const char *title) {
    int i = bm_find(url);
    if (i >= 0) { bm_remove(i); return false; }
    if (g_nbm >= MAX_BM) return false;
    memmove(&g_bm[1], &g_bm[0], sizeof(Entry) * (size_t)g_nbm);
    snprintf(g_bm[0].url, sizeof g_bm[0].url, "%s", url);
    snprintf(g_bm[0].title, sizeof g_bm[0].title, "%s", title && title[0] ? title : url);
    clean(g_bm[0].title);
    g_nbm++;
    bm_save();
    return true;
}

void hist_add(const char *url, const char *title) {
    if (!url[0]) return;
    for (int i = 0; i < g_nhist; i++) {
        if (!strcmp(g_hist[i].url, url)) {
            memmove(&g_hist[i], &g_hist[i + 1], sizeof(Entry) * (size_t)(g_nhist - i - 1));
            g_nhist--;
            break;
        }
    }
    if (g_nhist >= MAX_HIST) g_nhist = MAX_HIST - 1;
    memmove(&g_hist[1], &g_hist[0], sizeof(Entry) * (size_t)g_nhist);
    snprintf(g_hist[0].url, sizeof g_hist[0].url, "%s", url);
    snprintf(g_hist[0].title, sizeof g_hist[0].title, "%s", title && title[0] ? title : url);
    clean(g_hist[0].title);
    g_nhist++;
    if (++s_histDirty >= 8) { save_list(HIST_FILE, g_hist, g_nhist); s_histDirty = 0; }
}

void hist_remove(int i) {
    if (i < 0 || i >= g_nhist) return;
    memmove(&g_hist[i], &g_hist[i + 1], sizeof(Entry) * (size_t)(g_nhist - i - 1));
    g_nhist--;
    save_list(HIST_FILE, g_hist, g_nhist);
}

void hist_clear(void) {
    g_nhist = 0;
    remove(HIST_FILE);
}
