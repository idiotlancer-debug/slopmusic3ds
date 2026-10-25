/*
 * MP3 Player 3DS - reproductor de MP3 local con interfaz tactil y ahorro de bateria.
 *
 * Ahorro de bateria:
 *   - Nada de red: no se inicia Wi-Fi ni ningun servicio de internet.
 *   - La interfaz solo se redibuja cuando algo cambia (1 vez/s como maximo).
 *   - Las pantallas se apagan solas tras un tiempo sin tocar nada (backlight off).
 *   - Con las pantallas apagadas el bucle principal casi no consume (20 sondeos/s).
 *   - El hilo de audio duerme hasta que el DSP libera un buffer y decodifica en
 *     rafagas; lee la SD en bloques grandes de 48 KB para que la tarjeta descanse.
 *   - Sin aceleracion en New 3DS (osSetSpeedupEnable(false)).
 *
 * Requiere devkitARM + libctru + citro2d/citro3d y minimp3.h en include/.
 */
#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define MUSIC_DIR       "sdmc:/music"
#define CFG_DIR         "sdmc:/3ds/mp3player3ds"
#define CFG_FILE        CFG_DIR "/config.txt"

#define MAX_TRACKS      1000
#define MAX_DEPTH       3
#define NUM_WBUF        8
#define FRAMES_PER_BUF  4                          /* tramas MP3 por buffer ~105 ms */
#define BUF_SAMPLES     (1152 * FRAMES_PER_BUF)    /* muestras por canal */
#define IN_SIZE         (48 * 1024)
#define IN_REFILL_BELOW (8 * 1024)
#define LIST_ROWS       8

enum { CMD_NONE = 0, CMD_PLAY, CMD_STOP, CMD_SEEK };
enum { REP_OFF = 0, REP_ALL, REP_ONE };
enum { VIEW_PLAYER = 0, VIEW_LIST };

typedef struct { char *path; char *name; } Track;
typedef struct { int x, y, w, h; } Rect;

/* ------------------------------------------------------------------ colores */
#define COL_BG      C2D_Color32(18, 18, 26, 255)
#define COL_PANEL   C2D_Color32(38, 38, 54, 255)
#define COL_BTN     C2D_Color32(58, 58, 88, 255)
#define COL_BTN_ON  C2D_Color32(60, 120, 220, 255)
#define COL_TXT     C2D_Color32(238, 238, 248, 255)
#define COL_DIM     C2D_Color32(150, 150, 172, 255)
#define COL_ACC     C2D_Color32(80, 170, 255, 255)
#define COL_ERR     C2D_Color32(255, 120, 110, 255)

/* ------------------------------------------------------------- disposicion */
static const Rect R_PROG = {20, 50, 280, 40};
static const Rect R_PREV = {30, 116, 70, 50};
static const Rect R_PLAY = {125, 116, 70, 50};
static const Rect R_NEXT = {220, 116, 70, 50};
static const Rect R_SHUF = {8, 174, 70, 30};
static const Rect R_REP  = {84, 174, 70, 30};
static const Rect R_VOLM = {160, 174, 36, 30};
static const Rect R_VOLP = {200, 174, 36, 30};
static const Rect R_LIST = {242, 174, 70, 30};
static const Rect R_OFF  = {8, 208, 150, 26};
static const Rect R_AUTO = {164, 208, 148, 26};
static const Rect R_BACK = {236, 2, 80, 22};
static const Rect R_UP   = {278, 30, 40, 90};
static const Rect R_DOWN = {278, 124, 40, 90};

/* -------------------------------------------------------------------- estado */
static Track g_tracks[MAX_TRACKS];
static int   g_ntracks = 0;
static int   g_order[MAX_TRACKS];
static int   g_pos = 0;                 /* posicion actual dentro de g_order */
static int   g_cur = -1, g_sel = 0, g_scroll = 0;

static int   g_volume = 80, g_shuffle = 0, g_repeat = REP_OFF, g_tmo_idx = 1;
static const int g_tmo_secs[4] = {10, 30, 60, 0};

static int   g_view = VIEW_PLAYER;
static bool  g_paused = false, g_dirty = true, g_scr_on = true, g_audio_ok = false;
static bool  g_drag = false, g_ignore_touch = false;
static float g_drag_ratio = 0.0f;
static int   g_tx = 0, g_ty = 0, g_fail_streak = 0, g_last_sec = -1;
static u64   g_last_input = 0, g_last_batt = 0;
static int   g_batt_level = 0;
static bool  g_charging = false;

static volatile bool g_quit = false, g_loaded = false, g_eof = false, g_failed = false;
static volatile int  g_cmd = CMD_NONE, g_cmd_arg = 0, g_rate = 44100;
static volatile float g_seek_ratio = 0.0f, g_duration = 0.0f;
static volatile u32  g_dec_frames = 0, g_base_frames = 0;

static LightEvent  g_event;
static Thread      g_thr;
static ndspWaveBuf g_wbuf[NUM_WBUF];
static int16_t    *g_pcm;
static C2D_TextBuf g_tbuf;

/* ------------------------------------------------------------------ utilidades */
static bool hit(const Rect *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static void trunc_utf8(char *dst, size_t cap, const char *src, int maxchars) {
    size_t o = 0;
    int n = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && n < maxchars && o + 6 < cap) {
        int len = (*p < 0x80) ? 1 : (*p < 0xE0) ? 2 : (*p < 0xF0) ? 3 : 4;
        int i;
        for (i = 0; i < len && p[i]; i++) dst[o++] = (char)p[i];
        p += i;
        n++;
    }
    if (*p) { dst[o++] = '.'; dst[o++] = '.'; }
    dst[o] = 0;
}

static void fmt_time(char *out, size_t cap, int s) {
    if (s < 0) s = 0;
    snprintf(out, cap, "%02d:%02d", s / 60, s % 60);
}

static int cmp_tracks(const void *a, const void *b) {
    return strcasecmp(((const Track *)a)->name, ((const Track *)b)->name);
}

/* ------------------------------------------------------------ biblioteca SD */
static void add_track(const char *path, const char *file) {
    if (g_ntracks >= MAX_TRACKS) return;
    char *p = strdup(path);
    char *n = strdup(file);
    if (!p || !n) { free(p); free(n); return; }
    char *dot = strrchr(n, '.');
    if (dot) *dot = 0;
    g_tracks[g_ntracks].path = p;
    g_tracks[g_ntracks].name = n;
    g_ntracks++;
}

static void scan_dir(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_ntracks < MAX_TRACKS) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            if (depth < MAX_DEPTH) scan_dir(path, depth + 1);
        } else {
            size_t n = strlen(e->d_name);
            if (n > 4 && strcasecmp(e->d_name + n - 4, ".mp3") == 0) add_track(path, e->d_name);
        }
    }
    closedir(d);
}

static void build_order(int first) {
    for (int i = 0; i < g_ntracks; i++) g_order[i] = i;
    if (g_shuffle && g_ntracks > 1) {
        for (int i = g_ntracks - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int t = g_order[i]; g_order[i] = g_order[j]; g_order[j] = t;
        }
        if (first >= 0) {
            for (int k = 0; k < g_ntracks; k++) {
                if (g_order[k] == first) {
                    int t = g_order[0]; g_order[0] = g_order[k]; g_order[k] = t;
                    break;
                }
            }
        }
        g_pos = 0;
    } else {
        g_pos = (first >= 0) ? first : 0;
    }
}

/* ------------------------------------------------------------- configuracion */
static void load_config(void) {
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return;
    int v, s, r, t;
    if (fscanf(f, "%d %d %d %d", &v, &s, &r, &t) == 4) {
        if (v >= 0 && v <= 100) g_volume = v;
        g_shuffle = s ? 1 : 0;
        if (r >= 0 && r <= 2) g_repeat = r;
        if (t >= 0 && t <= 3) g_tmo_idx = t;
    }
    fclose(f);
}

static void save_config(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(CFG_DIR, 0777);
    FILE *f = fopen(CFG_FILE, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d\n", g_volume, g_shuffle, g_repeat, g_tmo_idx);
    fclose(f);
}

/* --------------------------------------------------------------------- audio */
static void apply_volume(void) {
    float v = (float)g_volume / 100.0f;
    float mix[12] = {0};
    mix[0] = mix[1] = v * v;
    ndspChnSetMix(0, mix);
}

static bool buf_busy(int i) {
    u8 s = *(volatile u8 *)&g_wbuf[i].status;
    return s == NDSP_WBUF_QUEUED || s == NDSP_WBUF_PLAYING;
}

static bool all_idle(void) {
    for (int i = 0; i < NUM_WBUF; i++) if (buf_busy(i)) return false;
    return true;
}

static double elapsed_sec(void) {
    u32 inflight = 0;
    for (int i = 0; i < NUM_WBUF; i++) if (buf_busy(i)) inflight += g_wbuf[i].nsamples;
    double fr = (double)g_base_frames + (double)g_dec_frames - (double)inflight;
    if (fr < 0) fr = 0;
    return fr / (double)g_rate;
}

/* Llamado por el DSP en cada trama: despierta al decodificador solo si hay hueco. */
static void audio_cb(void *data) {
    (void)data;
    if (!g_loaded || g_eof) return;
    for (int i = 0; i < NUM_WBUF; i++) {
        if (!buf_busy(i)) { LightEvent_Signal(&g_event); return; }
    }
}

static void flush_audio(void) {
    ndspChnWaveBufClear(0);
    svcSleepThread(30LL * 1000000LL);
    memset(g_wbuf, 0, sizeof g_wbuf);
}

static void audio_thread(void *arg) {
    (void)arg;
    static mp3dec_t dec;
    static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info;
    FILE *fp = NULL;
    u8 *in = malloc(IN_SIZE);
    size_t inlen = 0;
    bool active = false, file_eof = false, got_rate = false;
    long audio_start = 0, audio_bytes = 0;
    int wi = 0;

    if (!in) return;

    while (!g_quit) {
        LightEvent_Wait(&g_event);
        if (g_quit) break;

        int cmd = __atomic_exchange_n(&g_cmd, CMD_NONE, __ATOMIC_SEQ_CST);

        if (cmd == CMD_STOP || cmd == CMD_PLAY) {
            active = false;
            if (fp) { fclose(fp); fp = NULL; }
            flush_audio();
        }

        if (cmd == CMD_PLAY) {
            int idx = g_cmd_arg;
            g_eof = false;
            g_failed = false;
            fp = fopen(g_tracks[idx].path, "rb");
            if (!fp) {
                g_failed = true;
                g_eof = true;
            } else {
                /* saltar etiqueta ID3v2 y calcular zona de audio (para duracion y busqueda) */
                u8 hdr[10];
                audio_start = 0;
                if (fread(hdr, 1, 10, fp) == 10 && memcmp(hdr, "ID3", 3) == 0) {
                    long sz = ((hdr[6] & 0x7f) << 21) | ((hdr[7] & 0x7f) << 14) |
                              ((hdr[8] & 0x7f) << 7) | (hdr[9] & 0x7f);
                    audio_start = 10 + sz + ((hdr[5] & 0x10) ? 10 : 0);
                }
                fseek(fp, 0, SEEK_END);
                long fsize = ftell(fp);
                long end = fsize;
                if (fsize > 128) {
                    char tag[3];
                    fseek(fp, fsize - 128, SEEK_SET);
                    if (fread(tag, 1, 3, fp) == 3 && memcmp(tag, "TAG", 3) == 0) end -= 128;
                }
                if (audio_start >= end) audio_start = 0;
                audio_bytes = end - audio_start;
                fseek(fp, audio_start, SEEK_SET);

                ndspChnReset(0);
                ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
                ndspChnSetRate(0, 44100.0f);
                ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
                apply_volume();

                inlen = 0;
                file_eof = false;
                got_rate = false;
                wi = 0;
                mp3dec_init(&dec);
                g_dec_frames = 0;
                g_base_frames = 0;
                g_duration = 0.0f;
                g_rate = 44100;
                active = true;
            }
        }

        if (cmd == CMD_SEEK && fp) {
            float r = g_seek_ratio;
            if (r < 0.0f) r = 0.0f;
            if (r > 0.995f) r = 0.995f;
            flush_audio();
            fseek(fp, audio_start + (long)(r * (float)audio_bytes), SEEK_SET);
            inlen = 0;
            file_eof = false;
            wi = 0;
            mp3dec_init(&dec);
            g_dec_frames = 0;
            g_base_frames = (u32)(r * g_duration * (float)g_rate);
            g_eof = false;
            active = true;
        }

        if (!active) continue;

        /* rafaga: rellena todos los buffers libres y vuelve a dormir */
        while (!g_quit) {
            if (buf_busy(wi)) break;
            ndspWaveBuf *wb = &g_wbuf[wi];
            int16_t *dst = g_pcm + (size_t)wi * BUF_SAMPLES * 2;
            int total = 0;

            while (total + 1152 <= BUF_SAMPLES) {
                if (inlen < IN_REFILL_BELOW && !file_eof) {
                    size_t want = IN_SIZE - inlen;
                    size_t n = fread(in + inlen, 1, want, fp);
                    inlen += n;
                    if (n < want) file_eof = true;
                }
                if (inlen == 0) break;

                int s = mp3dec_decode_frame(&dec, in, (int)inlen, pcm, &info);
                if (info.frame_bytes == 0) { inlen = 0; break; }   /* sin mas datos */
                memmove(in, in + info.frame_bytes, inlen - (size_t)info.frame_bytes);
                inlen -= (size_t)info.frame_bytes;
                if (s <= 0) continue;

                if (!got_rate) {
                    got_rate = true;
                    if (info.hz > 0) {
                        g_rate = info.hz;
                        ndspChnSetRate(0, (float)info.hz);
                    }
                    if (g_duration <= 0.0f && info.bitrate_kbps > 0) {
                        g_duration = (float)((double)audio_bytes * 8.0 / ((double)info.bitrate_kbps * 1000.0));
                    }
                }

                if (info.channels == 2) {
                    memcpy(dst + total * 2, pcm, (size_t)s * 4);
                } else {
                    for (int i = 0; i < s; i++) {
                        dst[(total + i) * 2] = pcm[i];
                        dst[(total + i) * 2 + 1] = pcm[i];
                    }
                }
                total += s;
            }

            if (total == 0) { g_eof = true; break; }

            wb->data_pcm16 = dst;
            wb->nsamples = (u32)total;
            wb->looping = false;
            DSP_FlushDataCache(dst, (u32)total * 4);
            ndspChnWaveBufAdd(0, wb);
            g_dec_frames += (u32)total;
            wi = (wi + 1) % NUM_WBUF;

            if (inlen == 0 && file_eof) { g_eof = true; break; }
        }
        if (g_eof) active = false;
    }

    if (fp) fclose(fp);
    free(in);
}

/* ------------------------------------------------------------------ control */
static void send_cmd(int cmd) {
    __atomic_store_n(&g_cmd, cmd, __ATOMIC_SEQ_CST);
    LightEvent_Signal(&g_event);
}

static void send_seek(float ratio) {
    if (!g_loaded || g_duration <= 0.0f) return;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    g_seek_ratio = ratio;
    send_cmd(CMD_SEEK);
    g_dirty = true;
}

static void seek_rel(float delta_sec) {
    if (!g_loaded || g_duration <= 0.0f) return;
    send_seek((float)(elapsed_sec() + delta_sec) / g_duration);
}

static void play_index(int idx) {
    if (!g_audio_ok || idx < 0 || idx >= g_ntracks) return;
    g_cur = g_sel = idx;
    for (int k = 0; k < g_ntracks; k++) if (g_order[k] == idx) { g_pos = k; break; }
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + LIST_ROWS) g_scroll = g_sel - LIST_ROWS + 1;
    g_paused = false;
    ndspChnSetPaused(0, false);
    g_eof = false;
    g_failed = false;
    g_loaded = true;
    g_cmd_arg = idx;
    send_cmd(CMD_PLAY);
    g_last_sec = -1;
    g_dirty = true;
}

static void stop_playback(void) {
    g_loaded = false;
    g_paused = false;
    ndspChnSetPaused(0, false);
    send_cmd(CMD_STOP);
    g_dirty = true;
}

static void toggle_pause(void) {
    if (!g_loaded) { play_index(g_sel); return; }
    g_paused = !g_paused;
    ndspChnSetPaused(0, g_paused);
    g_dirty = true;
}

static void next_track(int dir) {
    if (!g_ntracks) return;
    if (!g_loaded) { play_index(g_sel); return; }
    int p = g_pos + dir;
    if (p < 0) p = g_ntracks - 1;
    if (p >= g_ntracks) p = 0;
    play_index(g_order[p]);
}

static void do_prev(void) {
    if (g_loaded && elapsed_sec() > 3.0) send_seek(0.0f);
    else next_track(-1);
}

static void track_ended(void) {
    g_fail_streak = g_failed ? g_fail_streak + 1 : 0;
    if (g_fail_streak >= g_ntracks) { g_fail_streak = 0; stop_playback(); return; }
    if (g_repeat == REP_ONE && !g_failed) { play_index(g_cur); return; }
    int next = g_pos + 1;
    if (next >= g_ntracks) {
        if (g_repeat == REP_ALL) {
            if (g_shuffle) build_order(-1);
            next = 0;
        } else {
            stop_playback();
            return;
        }
    }
    play_index(g_order[next]);
}

static void change_volume(int delta) {
    g_volume += delta;
    if (g_volume < 0) g_volume = 0;
    if (g_volume > 100) g_volume = 100;
    if (g_audio_ok) apply_volume();
    g_dirty = true;
}

static void toggle_shuffle(void) {
    g_shuffle = !g_shuffle;
    build_order(g_loaded ? g_cur : -1);
    g_dirty = true;
}

static void cycle_repeat(void) { g_repeat = (g_repeat + 1) % 3; g_dirty = true; }
static void cycle_timeout(void) { g_tmo_idx = (g_tmo_idx + 1) % 4; g_dirty = true; }

static void screens_set(bool on) {
    if (on == g_scr_on) return;
    g_scr_on = on;
    if (on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    else GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH);
    g_dirty = true;
}

static void move_sel(int d) {
    if (!g_ntracks) return;
    g_sel += d;
    if (g_sel < 0) g_sel = 0;
    if (g_sel >= g_ntracks) g_sel = g_ntracks - 1;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + LIST_ROWS) g_scroll = g_sel - LIST_ROWS + 1;
    g_dirty = true;
}

/* ------------------------------------------------------------------- entrada */
static void handle_buttons(u32 kd) {
    if (kd & KEY_A) {
        if (g_loaded && g_sel == g_cur) toggle_pause();
        else play_index(g_sel);
    }
    if (kd & KEY_B) stop_playback();
    if (kd & KEY_L) do_prev();
    if (kd & KEY_R) next_track(+1);
    if (kd & KEY_DLEFT) seek_rel(-10.0f);
    if (kd & KEY_DRIGHT) seek_rel(+10.0f);
    if (kd & KEY_DUP) move_sel(-1);
    if (kd & KEY_DDOWN) move_sel(+1);
    if (kd & KEY_X) toggle_shuffle();
    if (kd & KEY_Y) cycle_repeat();
    if (kd & KEY_CPAD_UP) change_volume(+5);
    if (kd & KEY_CPAD_DOWN) change_volume(-5);
}

static void tap_player(int x, int y) {
    if (hit(&R_PREV, x, y)) do_prev();
    else if (hit(&R_PLAY, x, y)) toggle_pause();
    else if (hit(&R_NEXT, x, y)) next_track(+1);
    else if (hit(&R_SHUF, x, y)) toggle_shuffle();
    else if (hit(&R_REP, x, y)) cycle_repeat();
    else if (hit(&R_VOLM, x, y)) change_volume(-5);
    else if (hit(&R_VOLP, x, y)) change_volume(+5);
    else if (hit(&R_LIST, x, y)) { g_view = VIEW_LIST; g_dirty = true; }
    else if (hit(&R_OFF, x, y)) screens_set(false);
    else if (hit(&R_AUTO, x, y)) cycle_timeout();
}

static void tap_list(int x, int y) {
    if (hit(&R_BACK, x, y)) { g_view = VIEW_PLAYER; g_dirty = true; return; }
    if (hit(&R_UP, x, y)) {
        g_scroll -= LIST_ROWS;
        if (g_scroll < 0) g_scroll = 0;
        g_dirty = true;
        return;
    }
    if (hit(&R_DOWN, x, y)) {
        if (g_scroll + LIST_ROWS < g_ntracks) g_scroll += LIST_ROWS;
        g_dirty = true;
        return;
    }
    if (x < 276 && y >= 28 && y < 28 + LIST_ROWS * 24) {
        int i = g_scroll + (y - 28) / 24;
        if (i < g_ntracks) { play_index(i); g_view = VIEW_PLAYER; }
    }
}

static void handle_touch(u32 kd, u32 kh, u32 ku, const touchPosition *tp) {
    if (g_ignore_touch) return;
    if (kh & KEY_TOUCH) { g_tx = tp->px; g_ty = tp->py; }

    if (g_view == VIEW_PLAYER) {
        if (kd & KEY_TOUCH) {
            if (hit(&R_PROG, g_tx, g_ty)) g_drag = true;
            else tap_player(g_tx, g_ty);
        }
        if (g_drag) {
            float r = (float)(g_tx - R_PROG.x) / (float)R_PROG.w;
            if (r < 0.0f) r = 0.0f;
            if (r > 1.0f) r = 1.0f;
            g_drag_ratio = r;
            g_dirty = true;
            if (ku & KEY_TOUCH) { g_drag = false; send_seek(r); }
        }
    } else if (kd & KEY_TOUCH) {
        tap_list(g_tx, g_ty);
    }
}

/* ---------------------------------------------------------------- dibujado */
static void text(float x, float y, float scale, u32 color, const char *fmt, ...) {
    char s[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    C2D_Text t;
    C2D_TextParse(&t, g_tbuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.7f, scale, scale, color);
}

static void text_in_rect(const Rect *r, float scale, u32 color, const char *fmt, ...) {
    char s[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    C2D_Text t;
    float w, h;
    C2D_TextParse(&t, g_tbuf, s);
    C2D_TextOptimize(&t);
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    C2D_DrawText(&t, C2D_WithColor, r->x + (r->w - w) / 2, r->y + (r->h - h) / 2, 0.7f, scale, scale, color);
}

static void box(const Rect *r, u32 color) {
    C2D_DrawRectSolid((float)r->x, (float)r->y, 0.3f, (float)r->w, (float)r->h, color);
}

static void icon_play(float cx, float cy, float s, u32 c) {
    C2D_DrawTriangle(cx - s * 0.6f, cy - s, c, cx - s * 0.6f, cy + s, c, cx + s, cy, c, 0.5f);
}
static void icon_pause(float cx, float cy, float s, u32 c) {
    C2D_DrawRectSolid(cx - s * 0.7f, cy - s, 0.5f, s * 0.55f, s * 2, c);
    C2D_DrawRectSolid(cx + s * 0.15f, cy - s, 0.5f, s * 0.55f, s * 2, c);
}
static void icon_next(float cx, float cy, float s, u32 c) {
    C2D_DrawTriangle(cx - s, cy - s, c, cx - s, cy + s, c, cx + s * 0.6f, cy, c, 0.5f);
    C2D_DrawRectSolid(cx + s * 0.7f, cy - s, 0.5f, s * 0.35f, s * 2, c);
}
static void icon_prev(float cx, float cy, float s, u32 c) {
    C2D_DrawTriangle(cx + s, cy - s, c, cx + s, cy + s, c, cx - s * 0.6f, cy, c, 0.5f);
    C2D_DrawRectSolid(cx - s * 1.05f, cy - s, 0.5f, s * 0.35f, s * 2, c);
}

static void draw_top(void) {
    char buf[96], t1[16], t2[16];
    C2D_DrawRectSolid(0, 0, 0.1f, 400, 240, COL_BG);
    C2D_DrawRectSolid(0, 0, 0.2f, 400, 26, COL_PANEL);
    text(8, 3, 0.55f, COL_TXT, "MP3 Player 3DS");

    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    if (tmv) text(230, 4, 0.5f, COL_DIM, "%02d:%02d", tmv->tm_hour, tmv->tm_min);
    for (int i = 0; i < 5; i++) {
        u32 c = (i < g_batt_level) ? (g_batt_level <= 1 ? COL_ERR : COL_ACC) : COL_BTN;
        C2D_DrawRectSolid(300.0f + i * 11, 7, 0.4f, 9, 13, c);
    }
    if (g_charging) text(358, 3, 0.55f, COL_ACC, "+");

    if (!g_audio_ok) {
        text(8, 40, 0.5f, COL_ERR, "Falta sdmc:/3ds/dspfirm.cdc");
        text(8, 62, 0.45f, COL_DIM, "Dumpealo con la app DSP1 para tener audio.");
    } else if (g_ntracks == 0) {
        text(8, 40, 0.5f, COL_ERR, "No hay MP3 en sdmc:/music");
    } else if (g_loaded && g_cur >= 0) {
        trunc_utf8(buf, sizeof buf, g_tracks[g_cur].name, 34);
        text(8, 34, 0.7f, COL_TXT, "%s", buf);
        fmt_time(t1, sizeof t1, (int)elapsed_sec());
        fmt_time(t2, sizeof t2, (int)g_duration);
        text(8, 66, 0.5f, COL_ACC, "%s   %s / %s%s", g_paused ? "Pausa" : "Reproduciendo", t1, t2,
             g_duration > 0 ? "~" : "");
    } else {
        text(8, 40, 0.6f, COL_DIM, "Detenido");
    }

    static const char *rep_txt[3] = {"no", "todo", "una"};
    text(8, 90, 0.45f, COL_DIM, "Mezcla: %s    Repetir: %s    Vol: %d%%", g_shuffle ? "si" : "no",
         rep_txt[g_repeat], g_volume);

    if (g_ntracks > 0) {
        int rows = 6;
        int start = g_sel - 2;
        if (start > g_ntracks - rows) start = g_ntracks - rows;
        if (start < 0) start = 0;
        for (int r = 0; r < rows && start + r < g_ntracks; r++) {
            int i = start + r;
            float y = 114.0f + r * 20;
            if (i == g_sel) C2D_DrawRectSolid(4, y - 1, 0.25f, 392, 19, COL_BTN);
            trunc_utf8(buf, sizeof buf, g_tracks[i].name, 48);
            text(10, y, 0.5f, (g_loaded && i == g_cur) ? COL_ACC : COL_TXT, "%s%s", (g_loaded && i == g_cur) ? "* " : "", buf);
        }
    }
}

static void draw_bottom_player(void) {
    char buf[96], t1[16], t2[16];
    C2D_DrawRectSolid(0, 0, 0.1f, 320, 240, COL_BG);

    const char *name = (g_loaded && g_cur >= 0) ? g_tracks[g_cur].name : "Nada en reproduccion";
    trunc_utf8(buf, sizeof buf, name, 30);
    text(10, 6, 0.6f, COL_TXT, "%s", buf);
    text(10, 30, 0.45f, COL_DIM, "%d/%d     Vol %d%%", g_cur >= 0 ? g_cur + 1 : 0, g_ntracks, g_volume);

    float dur = g_duration;
    float ratio = 0.0f;
    if (g_drag) ratio = g_drag_ratio;
    else if (g_loaded && dur > 0) ratio = (float)elapsed_sec() / dur;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    C2D_DrawRectSolid((float)R_PROG.x, 62, 0.3f, (float)R_PROG.w, 8, COL_PANEL);
    C2D_DrawRectSolid((float)R_PROG.x, 62, 0.4f, R_PROG.w * ratio, 8, COL_ACC);
    C2D_DrawRectSolid(R_PROG.x + R_PROG.w * ratio - 4, 57, 0.5f, 8, 18, COL_TXT);
    int shown = g_drag ? (int)(ratio * dur) : (int)elapsed_sec();
    fmt_time(t1, sizeof t1, g_loaded ? shown : 0);
    fmt_time(t2, sizeof t2, (int)dur);
    text(R_PROG.x, 88, 0.45f, COL_DIM, "%s", t1);
    text(R_PROG.x + R_PROG.w - 52, 88, 0.45f, COL_DIM, "%s%s", t2, dur > 0 ? "~" : "");

    box(&R_PREV, COL_BTN);
    box(&R_PLAY, g_loaded && !g_paused ? COL_BTN_ON : COL_BTN);
    box(&R_NEXT, COL_BTN);
    icon_prev(R_PREV.x + R_PREV.w / 2.0f, R_PREV.y + R_PREV.h / 2.0f, 12, COL_TXT);
    if (g_loaded && !g_paused) icon_pause(R_PLAY.x + R_PLAY.w / 2.0f, R_PLAY.y + R_PLAY.h / 2.0f, 13, COL_TXT);
    else icon_play(R_PLAY.x + R_PLAY.w / 2.0f, R_PLAY.y + R_PLAY.h / 2.0f, 13, COL_TXT);
    icon_next(R_NEXT.x + R_NEXT.w / 2.0f, R_NEXT.y + R_NEXT.h / 2.0f, 12, COL_TXT);

    static const char *rep_txt[3] = {"Rep: no", "Rep: todo", "Rep: una"};
    box(&R_SHUF, g_shuffle ? COL_BTN_ON : COL_BTN);
    text_in_rect(&R_SHUF, 0.5f, COL_TXT, "Mezcla");
    box(&R_REP, g_repeat ? COL_BTN_ON : COL_BTN);
    text_in_rect(&R_REP, 0.45f, COL_TXT, "%s", rep_txt[g_repeat]);
    box(&R_VOLM, COL_BTN);
    text_in_rect(&R_VOLM, 0.7f, COL_TXT, "-");
    box(&R_VOLP, COL_BTN);
    text_in_rect(&R_VOLP, 0.7f, COL_TXT, "+");
    box(&R_LIST, COL_BTN);
    text_in_rect(&R_LIST, 0.5f, COL_TXT, "Lista");

    box(&R_OFF, COL_BTN);
    text_in_rect(&R_OFF, 0.45f, COL_TXT, "Apagar pantallas");
    box(&R_AUTO, COL_BTN);
    if (g_tmo_secs[g_tmo_idx] > 0) text_in_rect(&R_AUTO, 0.45f, COL_TXT, "Auto-off: %d s", g_tmo_secs[g_tmo_idx]);
    else text_in_rect(&R_AUTO, 0.45f, COL_TXT, "Auto-off: nunca");
}

static void draw_bottom_list(void) {
    char buf[96];
    C2D_DrawRectSolid(0, 0, 0.1f, 320, 240, COL_BG);
    text(8, 3, 0.55f, COL_TXT, "Lista (%d)", g_ntracks);
    box(&R_BACK, COL_BTN);
    text_in_rect(&R_BACK, 0.45f, COL_TXT, "Volver");

    for (int r = 0; r < LIST_ROWS; r++) {
        int i = g_scroll + r;
        if (i >= g_ntracks) break;
        bool playing = g_loaded && i == g_cur;
        u32 bg = playing ? COL_BTN_ON : (i == g_sel ? COL_BTN : COL_PANEL);
        C2D_DrawRectSolid(4, 28.0f + r * 24, 0.3f, 270, 22, bg);
        trunc_utf8(buf, sizeof buf, g_tracks[i].name, 36);
        text(10, 30.0f + r * 24, 0.5f, COL_TXT, "%s", buf);
    }

    box(&R_UP, COL_BTN);
    box(&R_DOWN, COL_BTN);
    float ux = R_UP.x + R_UP.w / 2.0f, uy = R_UP.y + R_UP.h / 2.0f;
    C2D_DrawTriangle(ux - 12, uy + 8, COL_TXT, ux + 12, uy + 8, COL_TXT, ux, uy - 10, COL_TXT, 0.5f);
    float dx = R_DOWN.x + R_DOWN.w / 2.0f, dy = R_DOWN.y + R_DOWN.h / 2.0f;
    C2D_DrawTriangle(dx - 12, dy - 8, COL_TXT, dx + 12, dy - 8, COL_TXT, dx, dy + 10, COL_TXT, 0.5f);
}

static void render(C3D_RenderTarget *top, C3D_RenderTarget *bot) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(g_tbuf);
    C2D_TargetClear(top, COL_BG);
    C2D_SceneBegin(top);
    draw_top();
    C2D_TargetClear(bot, COL_BG);
    C2D_SceneBegin(bot);
    if (g_view == VIEW_PLAYER) draw_bottom_player();
    else draw_bottom_list();
    C3D_FrameEnd(0);
}

static void update_battery(u64 now) {
    u8 lvl = 0, chg = 0;
    if (R_SUCCEEDED(PTMU_GetBatteryLevel(&lvl)) && R_SUCCEEDED(PTMU_GetBatteryChargeState(&chg))) {
        if ((int)lvl != g_batt_level || (bool)chg != g_charging) g_dirty = true;
        g_batt_level = lvl;
        g_charging = chg != 0;
    }
    g_last_batt = now;
}

/* --------------------------------------------------------------------- main */
int main(void) {
    osSetSpeedupEnable(false);              /* New 3DS: reloj normal, menos consumo */
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_tbuf = C2D_TextBufNew(4096);

    gspLcdInit();
    ptmuInit();
    srand((unsigned)osGetTime());

    load_config();
    mkdir(MUSIC_DIR, 0777);
    scan_dir(MUSIC_DIR, 0);
    qsort(g_tracks, (size_t)g_ntracks, sizeof(Track), cmp_tracks);
    build_order(-1);

    LightEvent_Init(&g_event, RESET_ONESHOT);
    g_pcm = linearAlloc((size_t)NUM_WBUF * BUF_SAMPLES * 2 * sizeof(int16_t));
    g_audio_ok = g_pcm && R_SUCCEEDED(ndspInit());
    if (g_audio_ok) {
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspSetCallback(audio_cb, NULL);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        g_thr = threadCreate(audio_thread, NULL, 128 * 1024, prio - 1, -1, false);
        if (!g_thr) g_audio_ok = false;
    }

    g_last_input = osGetTime();
    update_battery(g_last_input);

    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp();
        touchPosition tp = {0, 0};
        if (kh & KEY_TOUCH) hidTouchRead(&tp);
        u64 now = osGetTime();

        if (kd & KEY_START) break;
        if (!(kh & KEY_TOUCH)) g_ignore_touch = false;

        if (kd & KEY_SELECT) { screens_set(!g_scr_on); g_last_input = now; }
        if (!g_scr_on && (kd & KEY_TOUCH)) {          /* tocar la pantalla apagada solo la enciende */
            screens_set(true);
            g_ignore_touch = true;
            g_last_input = now;
        }
        if (kd || (kh & KEY_TOUCH)) g_last_input = now;

        handle_buttons(kd);
        if (g_scr_on) handle_touch(kd, kh, ku, &tp);

        if (g_loaded && g_eof && all_idle()) track_ended();

        if (g_scr_on) {
            int tmo = g_tmo_secs[g_tmo_idx];
            if (tmo > 0 && now - g_last_input > (u64)tmo * 1000ULL && !g_drag) screens_set(false);
        }

        if (g_scr_on && g_loaded && !g_paused) {
            int s = (int)elapsed_sec();
            if (s != g_last_sec) { g_last_sec = s; g_dirty = true; }
        }
        if (g_scr_on && now - g_last_batt > 30000ULL) update_battery(now);

        if (g_dirty && g_scr_on) {
            render(top, bot);
            g_dirty = false;
        } else {
            svcSleepThread(g_scr_on ? 33LL * 1000000LL : 60LL * 1000000LL);
        }
    }

    /* limpieza */
    save_config();
    g_quit = true;
    if (g_thr) {
        LightEvent_Signal(&g_event);
        threadJoin(g_thr, U64_MAX);
        threadFree(g_thr);
    }
    if (g_audio_ok) { ndspSetCallback(NULL, NULL); ndspExit(); }
    if (!g_scr_on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    for (int i = 0; i < g_ntracks; i++) { free(g_tracks[i].path); free(g_tracks[i].name); }
    if (g_pcm) linearFree(g_pcm);
    C2D_TextBufDelete(g_tbuf);
    ptmuExit();
    gspLcdExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
