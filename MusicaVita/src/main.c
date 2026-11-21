/*
 * Música Vita - player de músicas para PS Vita (MP3, OGG, FLAC, WAV)
 * Pasta das músicas: ux0:/data/music (também procura em ux0:/music e uma memória uma0:)
 */
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/power.h>
#include <vita2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#define DR_MP3_NO_SIMD
#define DR_MP3_IMPLEMENTATION
#include "deps/dr_mp3.h"
#define DR_FLAC_NO_SIMD
#define DR_FLAC_IMPLEMENTATION
#include "deps/dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "deps/dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#include "deps/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#define OUT_RATE 48000
#define GRAIN 1024
#define MAX_TRACKS 5000
#define SRC_CHUNK 4096

/* ------------------------------------------------------------------ */
/* Lista de músicas                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    char *path;
    const char *name;
} Track;

static Track *tracks = NULL;
static int ntracks = 0;

static int has_audio_ext(const char *n) {
    const char *e = strrchr(n, '.');
    if (!e) return 0;
    return !strcasecmp(e, ".mp3") || !strcasecmp(e, ".ogg") || !strcasecmp(e, ".flac") || !strcasecmp(e, ".wav");
}

static void add_track(const char *path) {
    if (ntracks >= MAX_TRACKS) return;
    Track *t = realloc(tracks, sizeof(Track) * (ntracks + 1));
    if (!t) return;
    tracks = t;
    tracks[ntracks].path = strdup(path);
    if (!tracks[ntracks].path) return;
    const char *s = strrchr(tracks[ntracks].path, '/');
    tracks[ntracks].name = s ? s + 1 : tracks[ntracks].path;
    ntracks++;
}

static void scan_dir(const char *dir, int depth) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (e.d_name[0] != '.') {
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", dir, e.d_name);
            if (SCE_S_ISDIR(e.d_stat.st_mode)) {
                if (depth < 4) scan_dir(p, depth + 1);
            } else if (has_audio_ext(e.d_name)) {
                add_track(p);
            }
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}

static int cmp_track(const void *a, const void *b) {
    return strcasecmp(((const Track *)a)->path, ((const Track *)b)->path);
}

/* ------------------------------------------------------------------ */
/* Decodificadores                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    int kind; /* 0 nenhum, 1 mp3, 2 flac, 3 ogg, 4 wav */
    drmp3 mp3;
    drflac *flac;
    stb_vorbis *ogg;
    drwav wav;
    unsigned int rate, ch;
    uint64_t total; /* 0 = desconhecido */
} Dec;

static int16_t tmpbuf[SRC_CHUNK * 8];

static int dec_open(Dec *d, const char *path) {
    memset(d, 0, sizeof(*d));
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    if (!strcasecmp(ext, ".mp3")) {
        if (!drmp3_init_file(&d->mp3, path, NULL)) return 0;
        d->kind = 1;
        d->rate = d->mp3.sampleRate;
        d->ch = d->mp3.channels;
        d->total = 0;
    } else if (!strcasecmp(ext, ".flac")) {
        d->flac = drflac_open_file(path, NULL);
        if (!d->flac) return 0;
        d->kind = 2;
        d->rate = d->flac->sampleRate;
        d->ch = d->flac->channels;
        d->total = d->flac->totalPCMFrameCount;
    } else if (!strcasecmp(ext, ".ogg")) {
        int err = 0;
        d->ogg = stb_vorbis_open_filename(path, &err, NULL);
        if (!d->ogg) return 0;
        stb_vorbis_info vi = stb_vorbis_get_info(d->ogg);
        d->kind = 3;
        d->rate = vi.sample_rate;
        d->ch = (unsigned int)vi.channels;
        d->total = stb_vorbis_stream_length_in_samples(d->ogg);
    } else if (!strcasecmp(ext, ".wav")) {
        if (!drwav_init_file(&d->wav, path, NULL)) return 0;
        d->kind = 4;
        d->rate = d->wav.sampleRate;
        d->ch = d->wav.channels;
        d->total = d->wav.totalPCMFrameCount;
    } else {
        return 0;
    }
    if (d->rate == 0 || d->ch < 1 || d->ch > 8) {
        return 0;
    }
    return 1;
}

static void dec_close(Dec *d) {
    switch (d->kind) {
        case 1: drmp3_uninit(&d->mp3); break;
        case 2: drflac_close(d->flac); break;
        case 3: stb_vorbis_close(d->ogg); break;
        case 4: drwav_uninit(&d->wav); break;
        default: break;
    }
    d->kind = 0;
}

/* lê até "frames" quadros e devolve sempre estéreo intercalado */
static size_t dec_read(Dec *d, int16_t *out, size_t frames) {
    unsigned int ch = d->ch;
    size_t got = 0;
    if (frames > SRC_CHUNK) frames = SRC_CHUNK;
    switch (d->kind) {
        case 1: got = (size_t)drmp3_read_pcm_frames_s16(&d->mp3, frames, tmpbuf); break;
        case 2: got = (size_t)drflac_read_pcm_frames_s16(d->flac, frames, tmpbuf); break;
        case 3: got = (size_t)stb_vorbis_get_samples_short_interleaved(d->ogg, (int)ch, tmpbuf, (int)(frames * ch)); break;
        case 4: got = (size_t)drwav_read_pcm_frames_s16(&d->wav, frames, tmpbuf); break;
        default: return 0;
    }
    for (size_t i = 0; i < got; i++) {
        out[i * 2] = tmpbuf[i * ch];
        out[i * 2 + 1] = (ch > 1) ? tmpbuf[i * ch + 1] : tmpbuf[i * ch];
    }
    return got;
}

static void dec_seek(Dec *d, uint64_t frame) {
    switch (d->kind) {
        case 1: drmp3_seek_to_pcm_frame(&d->mp3, frame); break;
        case 2: drflac_seek_to_pcm_frame(d->flac, frame); break;
        case 3: stb_vorbis_seek(d->ogg, (unsigned int)frame); break;
        case 4: drwav_seek_to_pcm_frame(&d->wav, frame); break;
        default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Thread de áudio                                                      */
/* ------------------------------------------------------------------ */
static volatile int req_load = -1;    /* índice da faixa a carregar */
static volatile int req_seek_s = -1;  /* posição (segundos) para ir */
static volatile int paused = 0;
static volatile int quit_flag = 0;
static volatile int a_playing = 0;    /* há faixa aberta */
static volatile int a_ended = 0;      /* terminou sozinha */
static volatile int a_fail = 0;       /* não conseguiu abrir */
static volatile int a_port_err = 0;
static volatile uint32_t a_rate = 0;
static volatile uint64_t a_played = 0;
static volatile uint64_t a_total = 0;

static int16_t sbuf[(SRC_CHUNK + 16) * 2];
static int16_t outbuf[GRAIN * 2] __attribute__((aligned(64)));

static int audio_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        a_port_err = 1;
        return 0;
    }
    int vols[2] = {32768, 32768};
    sceAudioOutSetVolume(port, 3, vols);

    Dec dec;
    memset(&dec, 0, sizeof(dec));
    int open = 0, eof = 0;
    size_t sn = 0;
    double spos = 0.0;

    while (!quit_flag) {
        int idx = req_load;
        if (idx >= 0) {
            req_load = -1;
            if (open) {
                dec_close(&dec);
                open = 0;
            }
            a_playing = 0;
            a_ended = 0;
            a_fail = 0;
            if (idx < ntracks && dec_open(&dec, tracks[idx].path)) {
                open = 1;
                eof = 0;
                sn = 0;
                spos = 0.0;
                a_played = 0;
                a_rate = dec.rate;
                a_total = dec.total;
                a_playing = 1;
            } else {
                a_fail = 1;
            }
        }

        if (!open || paused) {
            sceKernelDelayThread(15000);
            continue;
        }

        int seek = req_seek_s;
        if (seek >= 0) {
            req_seek_s = -1;
            uint64_t target = (uint64_t)seek * dec.rate;
            if (dec.total && target >= dec.total) target = dec.total - 1;
            dec_seek(&dec, target);
            sn = 0;
            spos = 0.0;
            eof = 0;
            a_played = target;
        }

        double ratio = (double)dec.rate / (double)OUT_RATE;
        int i = 0;
        for (; i < GRAIN; i++) {
            size_t ix = (size_t)spos;
            if (ix + 1 >= sn) {
                size_t keep = (sn > ix) ? (sn - ix) : 0;
                if (keep) memmove(sbuf, sbuf + ix * 2, keep * 2 * sizeof(int16_t));
                spos -= (double)ix;
                sn = keep;
                if (!eof) {
                    size_t r = dec_read(&dec, sbuf + sn * 2, SRC_CHUNK);
                    if (r == 0) {
                        eof = 1;
                    } else {
                        sn += r;
                        a_played += r;
                    }
                }
                ix = (size_t)spos;
                if (ix + 1 >= sn) break;
            }
            double f = spos - (double)ix;
            outbuf[i * 2] = (int16_t)(sbuf[ix * 2] * (1.0 - f) + sbuf[(ix + 1) * 2] * f);
            outbuf[i * 2 + 1] = (int16_t)(sbuf[ix * 2 + 1] * (1.0 - f) + sbuf[(ix + 1) * 2 + 1] * f);
            spos += ratio;
        }
        int ended = (i < GRAIN);
        for (int k = i; k < GRAIN; k++) {
            outbuf[k * 2] = 0;
            outbuf[k * 2 + 1] = 0;
        }
        sceAudioOutOutput(port, outbuf);
        if (ended) {
            dec_close(&dec);
            open = 0;
            a_playing = 0;
            a_ended = 1;
        }
    }
    if (open) dec_close(&dec);
    sceAudioOutReleasePort(port);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Interface                                                            */
/* ------------------------------------------------------------------ */
static vita2d_pgf *pgf = NULL;

#define C_BG      RGBA8(28, 28, 30, 255)
#define C_PANEL   RGBA8(44, 44, 46, 255)
#define C_SEL     RGBA8(58, 58, 60, 255)
#define C_TEXT    RGBA8(245, 245, 247, 255)
#define C_DIM     RGBA8(142, 142, 147, 255)
#define C_ACCENT  RGBA8(10, 132, 255, 255)
#define C_TRACK   RGBA8(72, 72, 74, 255)

static void draw_text(int x, int y, unsigned int color, float scale, const char *s) {
    vita2d_pgf_draw_text(pgf, x, y, color, scale, s);
}

static int text_w(float scale, const char *s) {
    return vita2d_pgf_text_width(pgf, scale, s);
}

/* copia o nome sem extensão e corta com "..." para caber em maxw */
static void fit_name(const char *name, float scale, int maxw, char *out, size_t outsz) {
    snprintf(out, outsz, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    if (text_w(scale, out) <= maxw) return;
    size_t len = strlen(out);
    while (len > 1) {
        len--;
        while (len > 1 && (((unsigned char)out[len]) & 0xC0) == 0x80) len--; /* não cortar no meio de um caractere UTF-8 */
        out[len] = '\0';
        char tmp[300];
        snprintf(tmp, sizeof(tmp), "%s...", out);
        if (text_w(scale, tmp) <= maxw) {
            snprintf(out, outsz, "%s", tmp);
            return;
        }
    }
}

static void fmt_time(int s, char *out, size_t n) {
    if (s < 0) s = 0;
    if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static int sel = 0, top = 0, cur = -1;
static int shuffle_on = 0, repeat_mode = 0; /* 0 desligado, 1 todas, 2 uma */
static int screen_off = 0;
static int fail_streak = 0;

#define ROWS 11
#define ROW_H 32
#define LIST_Y 72

static void draw_ui(void) {
    char buf[320], tmp[320];
    vita2d_start_drawing();
    vita2d_clear_screen();

    draw_text(32, 44, C_TEXT, 1.4f, "Música Vita");
    snprintf(buf, sizeof(buf), "%d músicas", ntracks);
    draw_text(928 - text_w(1.0f, buf), 42, C_DIM, 1.0f, buf);

    if (ntracks == 0) {
        draw_text(32, 150, C_TEXT, 1.1f, "Nenhuma música encontrada.");
        draw_text(32, 190, C_DIM, 0.95f, "Copie arquivos MP3, OGG, FLAC ou WAV para a pasta:");
        draw_text(32, 222, C_ACCENT, 1.0f, "ux0:/data/music");
        draw_text(32, 262, C_DIM, 0.95f, "e abra o aplicativo de novo.");
    } else {
        for (int r = 0; r < ROWS; r++) {
            int i = top + r;
            if (i >= ntracks) break;
            int ry = LIST_Y + r * ROW_H;
            if (i == sel) vita2d_draw_rectangle(24, ry, 912, ROW_H - 2, C_SEL);
            if (i == cur) vita2d_draw_fill_circle(44, ry + ROW_H / 2 - 1, 5, C_ACCENT);
            fit_name(tracks[i].name, 0.95f, 830, buf, sizeof(buf));
            draw_text(62, ry + 22, (i == cur) ? C_ACCENT : C_TEXT, 0.95f, buf);
        }
        /* barra de rolagem */
        if (ntracks > ROWS) {
            int h = ROWS * ROW_H;
            int bh = h * ROWS / ntracks;
            if (bh < 24) bh = 24;
            int by = LIST_Y + (h - bh) * top / (ntracks - ROWS);
            vita2d_draw_rectangle(942, by, 4, bh, C_TRACK);
        }
    }

    /* painel "tocando agora" */
    vita2d_draw_rectangle(0, 436, 960, 108, C_PANEL);
    if (a_port_err) {
        draw_text(32, 478, RGBA8(255, 69, 58, 255), 1.0f, "Não foi possível abrir a saída de áudio.");
    } else if (cur >= 0 && cur < ntracks) {
        fit_name(tracks[cur].name, 1.05f, 640, buf, sizeof(buf));
        draw_text(32, 466, C_TEXT, 1.05f, buf);
        snprintf(tmp, sizeof(tmp), "%s  |  Aleatório: %s  |  Repetir: %s",
                 paused ? "Pausado" : (a_playing ? "Tocando" : "Parado"),
                 shuffle_on ? "sim" : "não",
                 repeat_mode == 0 ? "não" : (repeat_mode == 1 ? "todas" : "uma"));
        draw_text(928 - text_w(0.8f, tmp), 462, C_DIM, 0.8f, tmp);

        uint32_t rate = a_rate ? a_rate : 1;
        int el = (int)(a_played / rate);
        uint64_t total = a_total;
        vita2d_draw_rectangle(32, 478, 896, 6, C_TRACK);
        if (total > 0) {
            int w = (int)(896.0 * (double)a_played / (double)total);
            if (w < 0) w = 0;
            if (w > 896) w = 896;
            vita2d_draw_rectangle(32, 478, w, 6, C_ACCENT);
            char t1[16], t2[16];
            fmt_time(el, t1, sizeof(t1));
            fmt_time((int)(total / rate), t2, sizeof(t2));
            snprintf(buf, sizeof(buf), "%s / %s", t1, t2);
        } else {
            fmt_time(el, buf, sizeof(buf));
        }
        draw_text(32, 506, C_DIM, 0.85f, buf);
    } else {
        draw_text(32, 470, C_DIM, 1.0f, "Escolha uma música e aperte X.");
    }
    draw_text(32, 532, C_DIM, 0.72f,
              "X Tocar   O Pausar   L/R Faixa   Esq/Dir 10s   Quadrado Aleatório   Triângulo Repetir   SELECT Tela   START Sair");

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* ------------------------------------------------------------------ */
/* Controle                                                             */
/* ------------------------------------------------------------------ */
static void play_index(int i) {
    if (i < 0 || i >= ntracks) return;
    cur = i;
    sel = i;
    paused = 0;
    a_ended = 0;
    a_fail = 0;
    a_played = 0;
    a_total = 0;
    req_load = i;
}

/* auto = 1 quando a faixa acabou sozinha; devolve -1 para parar */
static int next_index(int dir, int automatic) {
    if (ntracks == 0) return -1;
    if (automatic && repeat_mode == 2 && cur >= 0) return cur;
    if (shuffle_on && ntracks > 1) {
        int n;
        do { n = rand() % ntracks; } while (n == cur);
        return n;
    }
    int n = cur + dir;
    if (n >= ntracks) {
        if (repeat_mode == 1 || !automatic) return 0;
        return -1;
    }
    if (n < 0) return ntracks - 1;
    return n;
}

static void keep_sel_visible(void) {
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    if (top < 0) top = 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    srand((unsigned)sceKernelGetProcessTimeLow());

    sceIoMkdir("ux0:/data/music", 0777);
    scan_dir("ux0:/data/music", 0);
    scan_dir("ux0:/music", 0);
    scan_dir("uma0:/data/music", 0);
    scan_dir("uma0:/music", 0);
    if (ntracks > 1) qsort(tracks, ntracks, sizeof(Track), cmp_track);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    vita2d_init();
    vita2d_set_clear_color(C_BG);
    pgf = vita2d_load_default_pgf();

    SceUID th = sceKernelCreateThread("audio", audio_thread, 0x10000100, 0x40000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);

    SceCtrlData pad, old;
    memset(&pad, 0, sizeof(pad));
    memset(&old, 0, sizeof(old));
    int hold_up = 0, hold_down = 0;

    while (!quit_flag) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old.buttons;
        old = pad;

        /* evita que o Vita entre em suspensão automática (a música pararia) */
        sceKernelPowerTick(1);

        if (pressed & SCE_CTRL_START) quit_flag = 1;
        if (pressed & SCE_CTRL_SELECT) {
            screen_off = !screen_off;
            if (screen_off) scePowerRequestDisplayOff();
            else scePowerRequestDisplayOn();
        }

        if (pressed & SCE_CTRL_CROSS) play_index(sel);
        if ((pressed & SCE_CTRL_CIRCLE) && cur >= 0 && (a_playing || paused)) paused = !paused;
        if (pressed & SCE_CTRL_SQUARE) shuffle_on = !shuffle_on;
        if (pressed & SCE_CTRL_TRIANGLE) repeat_mode = (repeat_mode + 1) % 3;
        if (pressed & SCE_CTRL_RTRIGGER) {
            int n = next_index(1, 0);
            if (n >= 0) play_index(n);
        }
        if (pressed & SCE_CTRL_LTRIGGER) {
            /* se já passou de 3 s, volta ao início; senão, faixa anterior */
            if (cur >= 0 && a_rate && (a_played / a_rate) > 3) {
                req_seek_s = 0;
            } else {
                int n = next_index(-1, 0);
                if (n >= 0) play_index(n);
            }
        }
        if ((pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) && cur >= 0 && a_rate) {
            int el = (int)(a_played / a_rate);
            int target = el + ((pressed & SCE_CTRL_RIGHT) ? 10 : -10);
            if (target < 0) target = 0;
            req_seek_s = target;
        }

        /* navegação na lista, com repetição ao segurar */
        if (pad.buttons & SCE_CTRL_UP) hold_up++; else hold_up = 0;
        if (pad.buttons & SCE_CTRL_DOWN) hold_down++; else hold_down = 0;
        if (hold_up == 1 || (hold_up > 20 && hold_up % 4 == 0)) { if (sel > 0) sel--; }
        if (hold_down == 1 || (hold_down > 20 && hold_down % 4 == 0)) { if (sel < ntracks - 1) sel++; }
        keep_sel_visible();

        /* fim de faixa / falha ao abrir */
        if (a_ended) {
            a_ended = 0;
            fail_streak = 0;
            int n = next_index(1, 1);
            if (n >= 0) play_index(n);
        }
        if (a_fail) {
            a_fail = 0;
            fail_streak++;
            if (fail_streak < ntracks) {
                int n = next_index(1, 1);
                if (n >= 0) play_index(n);
            }
        }
        if (a_playing) fail_streak = 0;

        if (screen_off) sceKernelDelayThread(50000);
        else draw_ui();
    }

    quit_flag = 1;
    if (th >= 0) sceKernelWaitThreadEnd(th, NULL, NULL);
    if (screen_off) scePowerRequestDisplayOn();
    if (pgf) vita2d_free_pgf(pgf);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
