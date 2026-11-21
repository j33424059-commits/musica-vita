/*
 * Música Vita - player de músicas para PS Vita (MP3, OGG, FLAC, WAV)
 * Pasta das músicas: ux0:/data/music (também procura em ux0:/music e em uma0:)
 * Controles: botões físicos e tela de toque.
 *
 * Áudio: o arquivo inteiro é lido para a RAM (sem acesso ao cartão durante a música),
 * uma thread DECODIFICA para um buffer circular e outra thread só TOCA esse buffer.
 * A saída de áudio usa a taxa original da música sempre que o Vita aceita (sem reamostrar).
 */
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
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
#include <math.h>

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

#define GRAIN 1024              /* quadros enviados por vez à saída de áudio */
#define MAX_TRACKS 5000
#define SRC_CHUNK 1152          /* quadros lidos por vez do decodificador */
#define RING_FRAMES 131072      /* buffer circular (~3 s), potência de 2 */
#define RING_MASK (RING_FRAMES - 1)
#define FILL_BLOCK 2048         /* quadros produzidos por passada */
#define PREBUF 24000            /* quadros acumulados antes de começar a tocar (~0,5 s) */
#define MAX_RAM_FILE (48u * 1024u * 1024u)

#ifndef SCE_KERNEL_CPU_MASK_USER_1
#define SCE_KERNEL_CPU_MASK_USER_1 0x20000
#endif
#ifndef SCE_KERNEL_CPU_MASK_USER_2
#define SCE_KERNEL_CPU_MASK_USER_2 0x40000
#endif

/* ------------------------------------------------------------------ */
/* Lista de músicas                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    char *path;
    const char *name;
    char *disp; /* nome já cortado para caber na tela (calculado uma vez) */
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
    tracks[ntracks].disp = NULL;
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
/* MP3: duração e posição aproximadas a partir dos cabeçalhos           */
/* ------------------------------------------------------------------ */
static int mp3_hdr(const unsigned char *p, int *rate, int *spf, int *br, int *ch, int *pad, int *mpeg1, int *crc) {
    static const int br1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int br2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int sr1[3] = {44100, 48000, 32000};
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    int vb = (p[1] >> 3) & 3;     /* 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5 */
    int layer = (p[1] >> 1) & 3;  /* 1 = camada III */
    if (vb == 1 || layer != 1) return 0;
    int bri = (p[2] >> 4) & 15, sri = (p[2] >> 2) & 3;
    if (bri == 0 || bri == 15 || sri == 3) return 0;
    *mpeg1 = (vb == 3);
    *br = (*mpeg1 ? br1 : br2)[bri];
    int s = sr1[sri];
    if (vb == 2) s /= 2;
    else if (vb == 0) s /= 4;
    *rate = s;
    *spf = *mpeg1 ? 1152 : 576;
    *pad = (p[2] >> 1) & 1;
    *ch = (((p[3] >> 6) & 3) == 3) ? 1 : 2;
    *crc = !(p[1] & 1);
    return 1;
}

static size_t mp3_audio_start(const unsigned char *d, size_t n) {
    size_t start = 0;
    if (n > 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
        size_t sz = ((size_t)(d[6] & 0x7F) << 21) | ((size_t)(d[7] & 0x7F) << 14) | ((size_t)(d[8] & 0x7F) << 7) | (size_t)(d[9] & 0x7F);
        start = 10 + sz + ((d[5] & 0x10) ? 10 : 0);
    }
    if (start >= n) return 0;
    for (size_t i = start; i + 4 < n && i < start + 65536; i++) {
        int rate, spf, br, ch, pad, m1, crc;
        if (mp3_hdr(d + i, &rate, &spf, &br, &ch, &pad, &m1, &crc)) {
            size_t flen = (size_t)((m1 ? 144000 : 72000) * br / rate + pad);
            int r2, s2, b2, c2, p2, m2, k2;
            if (flen > 4 && i + flen + 4 < n && mp3_hdr(d + i + flen, &r2, &s2, &b2, &c2, &p2, &m2, &k2)) return i;
        }
    }
    return start;
}

/* total de quadros PCM (0 = desconhecido) */
static uint64_t mp3_estimate_total(const unsigned char *d, size_t n, size_t start) {
    if (start + 64 >= n) return 0;
    int rate, spf, br, ch, pad, m1, crc;
    if (!mp3_hdr(d + start, &rate, &spf, &br, &ch, &pad, &m1, &crc)) return 0;
    int side = m1 ? (ch == 1 ? 17 : 32) : (ch == 1 ? 9 : 17);
    const unsigned char *x = d + start + 4 + (crc ? 2 : 0) + side;
    if (x + 12 < d + n && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4))) {
        uint32_t flags = ((uint32_t)x[4] << 24) | ((uint32_t)x[5] << 16) | ((uint32_t)x[6] << 8) | x[7];
        if (flags & 1) {
            uint32_t frames = ((uint32_t)x[8] << 24) | ((uint32_t)x[9] << 16) | ((uint32_t)x[10] << 8) | x[11];
            return (uint64_t)frames * (uint64_t)spf;
        }
    }
    if (br <= 0) return 0;
    double secs = (double)(n - start) * 8.0 / ((double)br * 1000.0);
    return (uint64_t)(secs * rate);
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
    unsigned char *mem;   /* arquivo inteiro na RAM (NULL = lê do cartão) */
    size_t mem_size, mp3_start;
    unsigned int rate, ch;
    uint64_t total; /* 0 = desconhecido */
} Dec;

static int16_t tmpbuf[SRC_CHUNK * 8];

static unsigned char *load_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (unsigned long)len > MAX_RAM_FILE) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = 0;
    while (got < (size_t)len) {
        size_t want = (size_t)len - got;
        if (want > (1u << 20)) want = 1u << 20;
        size_t r = fread(buf + got, 1, want, f);
        if (r == 0) break;
        got += r;
    }
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return NULL;
    }
    *size = got;
    return buf;
}

static void dec_close(Dec *d) {
    switch (d->kind) {
        case 1: drmp3_uninit(&d->mp3); break;
        case 2: drflac_close(d->flac); break;
        case 3: stb_vorbis_close(d->ogg); break;
        case 4: drwav_uninit(&d->wav); break;
        default: break;
    }
    if (d->mem) free(d->mem);
    d->mem = NULL;
    d->kind = 0;
}

static int dec_open(Dec *d, const char *path) {
    memset(d, 0, sizeof(*d));
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    size_t sz = 0;
    d->mem = load_file(path, &sz);
    d->mem_size = sz;
    if (!strcasecmp(ext, ".mp3")) {
        int ok = d->mem ? drmp3_init_memory(&d->mp3, d->mem, sz, NULL) : drmp3_init_file(&d->mp3, path, NULL);
        if (!ok) goto fail;
        d->kind = 1;
        d->rate = d->mp3.sampleRate;
        d->ch = d->mp3.channels;
        if (d->mem) {
            d->mp3_start = mp3_audio_start(d->mem, sz);
            d->total = mp3_estimate_total(d->mem, sz, d->mp3_start);
        }
    } else if (!strcasecmp(ext, ".flac")) {
        d->flac = d->mem ? drflac_open_memory(d->mem, sz, NULL) : drflac_open_file(path, NULL);
        if (!d->flac) goto fail;
        d->kind = 2;
        d->rate = d->flac->sampleRate;
        d->ch = d->flac->channels;
        d->total = d->flac->totalPCMFrameCount;
    } else if (!strcasecmp(ext, ".ogg")) {
        int err = 0;
        d->ogg = d->mem ? stb_vorbis_open_memory(d->mem, (int)sz, &err, NULL) : stb_vorbis_open_filename(path, &err, NULL);
        if (!d->ogg) goto fail;
        stb_vorbis_info vi = stb_vorbis_get_info(d->ogg);
        d->kind = 3;
        d->rate = vi.sample_rate;
        d->ch = (unsigned int)vi.channels;
        d->total = stb_vorbis_stream_length_in_samples(d->ogg);
    } else if (!strcasecmp(ext, ".wav")) {
        int ok = d->mem ? drwav_init_memory(&d->wav, d->mem, sz, NULL) : drwav_init_file(&d->wav, path, NULL);
        if (!ok) goto fail;
        d->kind = 4;
        d->rate = d->wav.sampleRate;
        d->ch = d->wav.channels;
        d->total = d->wav.totalPCMFrameCount;
    } else {
        goto fail;
    }
    if (d->rate == 0 || d->ch < 1 || d->ch > 8) {
        dec_close(d);
        return 0;
    }
    return 1;
fail:
    if (d->mem) free(d->mem);
    memset(d, 0, sizeof(*d));
    return 0;
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
        case 1:
            if (d->mem && d->total) {
                /* MP3: salta pelos bytes (rápido) em vez de decodificar desde o início */
                double f = (double)frame / (double)d->total;
                if (f < 0) f = 0;
                if (f > 0.999) f = 0.999;
                size_t span = d->mem_size - d->mp3_start;
                size_t off = d->mp3_start + (size_t)(f * (double)span);
                drmp3_uninit(&d->mp3);
                if (!drmp3_init_memory(&d->mp3, d->mem + off, d->mem_size - off, NULL))
                    drmp3_init_memory(&d->mp3, d->mem, d->mem_size, NULL);
            } else {
                drmp3_seek_to_pcm_frame(&d->mp3, frame);
            }
            break;
        case 2: drflac_seek_to_pcm_frame(d->flac, frame); break;
        case 3: stb_vorbis_seek(d->ogg, (unsigned int)frame); break;
        case 4: drwav_seek_to_pcm_frame(&d->wav, frame); break;
        default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Áudio: thread decodificadora + thread de saída                       */
/* ------------------------------------------------------------------ */
static volatile int req_load = -1;    /* índice da faixa a carregar */
static volatile int req_seek_s = -1;  /* posição (segundos) para ir */
static volatile int paused = 0;
static volatile int quit_flag = 0;
static volatile int a_playing = 0;    /* há faixa aberta */
static volatile int a_ended = 0;      /* terminou sozinha */
static volatile int a_fail = 0;       /* não conseguiu abrir */
static volatile int a_port_err = 0;
static volatile uint32_t a_rate = 0;  /* taxa da música */
static volatile uint64_t a_played = 0;
static volatile uint64_t a_total = 0;

static volatile uint64_t pos_base = 0;   /* posição (quadros da fonte) onde o buffer recomeçou */
static volatile int flush_req = 0;       /* decodificadora pede para esvaziar o buffer */
static volatile int dec_eof = 0;         /* decodificadora terminou a faixa */
static volatile int req_out_rate = 48000;/* taxa de saída pedida */
static volatile int port_rate = 48000;   /* taxa de saída realmente aberta */

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t rd = 0, wr = 0; /* em quadros, só crescem */

static int16_t sbuf[(SRC_CHUNK + 16) * 2];
static int16_t outbuf[GRAIN * 2] __attribute__((aligned(64)));

static int pick_port_rate(unsigned int r) {
    static const unsigned int ok[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    for (unsigned int i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (r == ok[i]) return (int)r;
    return 48000; /* taxa não suportada: reamostra para 48 kHz */
}

/* decodificadora pede para a thread de saída esvaziar o buffer (e trocar a taxa) e espera */
static void flush_ring(void) {
    flush_req = 1;
    while (flush_req && !quit_flag && !a_port_err) sceKernelDelayThread(1000);
    flush_req = 0;
}

static int decoder_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    Dec dec;
    memset(&dec, 0, sizeof(dec));
    int open = 0, eof = 0, same = 1;
    size_t sn = 0;
    double spos = 0.0, ratio = 1.0;

    while (!quit_flag) {
        int idx = req_load;
        if (idx >= 0) {
            req_load = -1;
            req_seek_s = -1;
            if (open) {
                dec_close(&dec);
                open = 0;
            }
            a_playing = 0;
            a_ended = 0;
            a_fail = 0;
            dec_eof = 0;
            if (idx < ntracks && dec_open(&dec, tracks[idx].path)) {
                open = 1;
                eof = 0;
                sn = 0;
                spos = 0.0;
                a_rate = dec.rate;
                a_total = dec.total;
                pos_base = 0;
                req_out_rate = pick_port_rate(dec.rate);
                flush_ring();
                same = ((unsigned int)port_rate == dec.rate);
                ratio = (double)dec.rate / (double)port_rate;
                a_playing = 1;
            } else {
                a_fail = 1;
            }
        }

        if (!open) {
            req_seek_s = -1;
            sceKernelDelayThread(10000);
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
            dec_eof = 0;
            pos_base = target;
            req_out_rate = port_rate;
            flush_ring();
        }

        uint32_t used = wr - rd;
        if (used > RING_FRAMES) used = RING_FRAMES;
        if (RING_FRAMES - used < FILL_BLOCK) {
            sceKernelDelayThread(4000); /* buffer cheio: espera */
            continue;
        }

        uint32_t w = wr;
        int i = 0;
        size_t need = same ? 1 : 2;
        for (; i < FILL_BLOCK; i++) {
            size_t ix = (size_t)spos;
            while (ix + need > sn) {
                if (eof) break;
                size_t keep = (sn > ix) ? (sn - ix) : 0;
                if (keep) memmove(sbuf, sbuf + ix * 2, keep * 2 * sizeof(int16_t));
                spos -= (double)ix;
                sn = keep;
                size_t r = dec_read(&dec, sbuf + sn * 2, SRC_CHUNK);
                if (r == 0) eof = 1;
                else sn += r;
                ix = (size_t)spos;
            }
            if (ix + need > sn) break; /* fim do arquivo */
            int16_t *o = &ring[((w + (uint32_t)i) & RING_MASK) * 2];
            if (same) {
                o[0] = sbuf[ix * 2];
                o[1] = sbuf[ix * 2 + 1];
                spos += 1.0;
            } else {
                double f = spos - (double)ix;
                o[0] = (int16_t)(sbuf[ix * 2] * (1.0 - f) + sbuf[(ix + 1) * 2] * f);
                o[1] = (int16_t)(sbuf[ix * 2 + 1] * (1.0 - f) + sbuf[(ix + 1) * 2 + 1] * f);
                spos += ratio;
            }
        }
        __sync_synchronize();
        wr = w + (uint32_t)i;
        if (i < FILL_BLOCK) {
            dec_close(&dec);
            open = 0;
            __sync_synchronize();
            dec_eof = 1;
        }
    }
    if (open) dec_close(&dec);
    return 0;
}

static int open_port(int rate) {
    int p = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, rate, SCE_AUDIO_OUT_MODE_STEREO);
    if (p >= 0) {
        int vols[2] = {32768, 32768};
        sceAudioOutSetVolume(p, 3, vols);
    }
    return p;
}

static int audio_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int rate_cur = 48000;
    int port = open_port(rate_cur);
    if (port < 0) {
        a_port_err = 1;
        return 0;
    }
    port_rate = rate_cur;
    uint64_t out_played = 0;
    int buffering = 1;

    while (!quit_flag) {
        if (flush_req) {
            int want = req_out_rate;
            if (want != rate_cur) {
                sceAudioOutReleasePort(port);
                port = open_port(want);
                if (port < 0) {
                    want = 48000;
                    port = open_port(want);
                }
                if (port < 0) {
                    a_port_err = 1;
                    flush_req = 0;
                    return 0;
                }
                rate_cur = want;
                port_rate = want;
            }
            rd = wr;
            out_played = 0;
            a_played = pos_base;
            buffering = 1;
            __sync_synchronize();
            flush_req = 0;
        }

        int n = 0, finished = 0;
        if (a_playing && !paused) {
            int eofnow = dec_eof;
            __sync_synchronize();
            int32_t avail = (int32_t)(wr - rd);
            if (avail < 0) {
                rd = wr;
                avail = 0;
            }
            if (buffering && (avail >= PREBUF || eofnow)) buffering = 0;
            if (!buffering) {
                if (avail >= GRAIN) {
                    n = GRAIN;
                } else if (eofnow) {
                    n = avail;
                    finished = 1;
                } else {
                    buffering = 1; /* faltou dado: junta um pouco antes de continuar */
                }
            }
        }

        if (n > 0) {
            uint32_t r = rd & RING_MASK;
            int first = RING_FRAMES - (int)r;
            if (first > n) first = n;
            memcpy(outbuf, ring + (size_t)r * 2, (size_t)first * 4);
            if (first < n) memcpy(outbuf + (size_t)first * 2, ring, (size_t)(n - first) * 4);
            if (n < GRAIN) memset(outbuf + (size_t)n * 2, 0, (size_t)(GRAIN - n) * 4);
            rd += (uint32_t)n;
            out_played += (uint64_t)n;
            if (a_rate) a_played = pos_base + (uint64_t)((double)out_played * (double)a_rate / (double)rate_cur);
        } else if (!finished) {
            memset(outbuf, 0, sizeof(outbuf));
        } else {
            memset(outbuf, 0, sizeof(outbuf));
        }

        sceAudioOutOutput(port, outbuf);

        if (finished) {
            a_playing = 0;
            dec_eof = 0;
            a_ended = 1;
        }
    }
    sceAudioOutReleasePort(port);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Interface                                                            */
/* ------------------------------------------------------------------ */
static vita2d_pgf *pgf = NULL;

#define C_BG      RGBA8(18, 18, 20, 255)
#define C_PANEL   RGBA8(36, 36, 40, 255)
#define C_SEL     RGBA8(52, 52, 58, 255)
#define C_TEXT    RGBA8(245, 245, 247, 255)
#define C_DIM     RGBA8(142, 142, 150, 255)
#define C_ACCENT  RGBA8(10, 132, 255, 255)
#define C_COVER   RGBA8(0, 86, 190, 255)
#define C_TRACK   RGBA8(78, 78, 84, 255)
#define C_BTN     RGBA8(58, 58, 64, 255)

static void draw_text(int x, int y, unsigned int color, float scale, const char *s) {
    if (!pgf) return;
    vita2d_pgf_draw_text(pgf, x, y, color, scale, s);
}

static int text_w(float scale, const char *s) {
    if (!pgf) return 0;
    return vita2d_pgf_text_width(pgf, scale, s);
}

static void draw_text_center(int cx, int y, unsigned int color, float scale, const char *s) {
    draw_text(cx - text_w(scale, s) / 2, y, color, scale, s);
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

static void rrect(int x, int y, int w, int h, int r, unsigned int c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    vita2d_draw_rectangle(x + r, y, w - 2 * r, h, c);
    vita2d_draw_rectangle(x, y + r, w, h - 2 * r, c);
    vita2d_draw_fill_circle(x + r, y + r, r, c);
    vita2d_draw_fill_circle(x + w - r - 1, y + r, r, c);
    vita2d_draw_fill_circle(x + r, y + h - r - 1, r, c);
    vita2d_draw_fill_circle(x + w - r - 1, y + h - r - 1, r, c);
}

static void tri_right(int x, int y, int w, int h, unsigned int c) {
    int half = h / 2;
    if (half < 1) return;
    for (int i = 0; i < h; i++) {
        int k = (i < half) ? i : (h - 1 - i);
        int ww = w * k / half;
        if (ww > 0) vita2d_draw_rectangle(x, y + i, ww, 1, c);
    }
}

static void tri_left(int x, int y, int w, int h, unsigned int c) {
    int half = h / 2;
    if (half < 1) return;
    for (int i = 0; i < h; i++) {
        int k = (i < half) ? i : (h - 1 - i);
        int ww = w * k / half;
        if (ww > 0) vita2d_draw_rectangle(x + w - ww, y + i, ww, 1, c);
    }
}

static int sel = 0, top = 0, cur = -1;
static int shuffle_on = 0, repeat_mode = 0; /* 0 desligado, 1 todas, 2 uma */
static int screen_off = 0;
static int fail_streak = 0;
static unsigned int frame_no = 0;

/* --- geometria --- */
#define CARD_X 16
#define CARD_Y 16
#define CARD_W 380
#define CARD_H 512
#define CX 206                 /* centro do cartão */
#define PB_X 36
#define PB_W 340
#define PB_Y 332
#define CTRL_Y 420
#define PREV_X 106
#define PLAY_X 206
#define NEXT_X 306
#define PILL_Y 468
#define PILL_H 30
#define SHUF_X 36
#define SHUF_W 164
#define REP_X 212
#define REP_W 164

#define LIST_X 424
#define LIST_Y 66
#define ROW_H 40
#define ROWS 11
#define LIST_TEXT_W 440

static void draw_ui(void) {
    char buf[320], tmp[320];
    static int title_cur = -2;
    static char title_buf[320];

    vita2d_start_drawing();
    vita2d_clear_screen();

    /* ---------- cartão "tocando agora" ---------- */
    rrect(CARD_X, CARD_Y, CARD_W, CARD_H, 22, C_PANEL);

    /* capa com barras animadas */
    rrect(CX - 100, 40, 200, 200, 22, C_COVER);
    int playing_now = (a_playing && !paused);
    for (int i = 0; i < 5; i++) {
        float amp = playing_now ? (0.30f + 0.70f * fabsf(sinf((float)frame_no * 0.09f + (float)i * 1.35f))) : 0.30f;
        int bh = (int)(110.0f * amp);
        int bx = CX - 60 + i * 26;
        rrect(bx, 140 - bh / 2 + 10, 14, bh, 7, C_TEXT);
    }

    if (a_port_err) {
        draw_text(36, 290, RGBA8(255, 69, 58, 255), 0.95f, "Falha ao abrir a saída de áudio.");
    } else if (cur >= 0 && cur < ntracks) {
        if (title_cur != cur) {
            fit_name(tracks[cur].name, 1.15f, 340, title_buf, sizeof(title_buf));
            title_cur = cur;
        }
        draw_text(36, 284, C_TEXT, 1.15f, title_buf);
        snprintf(tmp, sizeof(tmp), "Faixa %d de %d  |  %s", cur + 1, ntracks,
                 paused ? "Pausado" : (a_playing ? "Tocando" : "Parado"));
        draw_text(36, 310, C_DIM, 0.8f, tmp);
    } else {
        draw_text(36, 284, C_TEXT, 1.15f, "Nada tocando");
        draw_text(36, 310, C_DIM, 0.8f, "Toque numa música da lista");
    }

    /* barra de progresso */
    uint32_t rate = a_rate ? a_rate : 1;
    uint64_t played = a_played;
    uint64_t total = a_total;
    rrect(PB_X, PB_Y, PB_W, 6, 3, C_TRACK);
    int el = (cur >= 0) ? (int)(played / rate) : 0;
    if (cur >= 0 && total > 0) {
        int w = (int)((double)PB_W * (double)played / (double)total);
        if (w < 0) w = 0;
        if (w > PB_W) w = PB_W;
        if (w > 6) rrect(PB_X, PB_Y, w, 6, 3, C_ACCENT);
        vita2d_draw_fill_circle(PB_X + w, PB_Y + 3, 8, C_TEXT);
    }
    fmt_time(el, buf, sizeof(buf));
    draw_text(PB_X, 366, C_DIM, 0.8f, buf);
    if (cur >= 0 && total > 0) {
        fmt_time((int)(total / rate), buf, sizeof(buf));
        draw_text(PB_X + PB_W - text_w(0.8f, buf), 366, C_DIM, 0.8f, buf);
    }

    /* botões: anterior / tocar-pausar / próxima */
    vita2d_draw_fill_circle(PREV_X, CTRL_Y, 27, C_BTN);
    vita2d_draw_rectangle(PREV_X - 12, CTRL_Y - 10, 4, 20, C_TEXT);
    tri_left(PREV_X - 7, CTRL_Y - 10, 18, 20, C_TEXT);

    vita2d_draw_fill_circle(NEXT_X, CTRL_Y, 27, C_BTN);
    tri_right(NEXT_X - 11, CTRL_Y - 10, 18, 20, C_TEXT);
    vita2d_draw_rectangle(NEXT_X + 8, CTRL_Y - 10, 4, 20, C_TEXT);

    vita2d_draw_fill_circle(PLAY_X, CTRL_Y, 34, C_TEXT);
    if (playing_now) {
        vita2d_draw_rectangle(PLAY_X - 10, CTRL_Y - 13, 7, 26, C_BG);
        vita2d_draw_rectangle(PLAY_X + 3, CTRL_Y - 13, 7, 26, C_BG);
    } else {
        tri_right(PLAY_X - 8, CTRL_Y - 14, 24, 28, C_BG);
    }

    /* aleatório / repetir */
    rrect(SHUF_X, PILL_Y, SHUF_W, PILL_H, 15, shuffle_on ? C_ACCENT : C_BTN);
    draw_text_center(SHUF_X + SHUF_W / 2, PILL_Y + 21, C_TEXT, 0.8f, shuffle_on ? "Aleatório: sim" : "Aleatório: não");
    rrect(REP_X, PILL_Y, REP_W, PILL_H, 15, repeat_mode ? C_ACCENT : C_BTN);
    draw_text_center(REP_X + REP_W / 2, PILL_Y + 21, C_TEXT, 0.8f,
                     repeat_mode == 0 ? "Repetir: não" : (repeat_mode == 1 ? "Repetir: todas" : "Repetir: uma"));

    draw_text(36, 520, C_DIM, 0.62f, "SELECT apaga a tela   START sai");

    /* ---------- biblioteca ---------- */
    draw_text(LIST_X + 8, 46, C_TEXT, 1.3f, "Biblioteca");
    snprintf(buf, sizeof(buf), "%d músicas", ntracks);
    draw_text(930 - text_w(0.9f, buf), 44, C_DIM, 0.9f, buf);

    if (ntracks == 0) {
        draw_text(LIST_X + 8, 150, C_TEXT, 1.05f, "Nenhuma música encontrada.");
        draw_text(LIST_X + 8, 186, C_DIM, 0.85f, "Copie MP3, OGG, FLAC ou WAV para:");
        draw_text(LIST_X + 8, 216, C_ACCENT, 0.95f, "ux0:/data/music");
        draw_text(LIST_X + 8, 250, C_DIM, 0.85f, "e abra o aplicativo de novo.");
    } else {
        for (int r = 0; r < ROWS; r++) {
            int i = top + r;
            if (i >= ntracks) break;
            int ry = LIST_Y + r * ROW_H;
            if (i == sel) rrect(LIST_X, ry, 508, ROW_H - 4, 10, C_SEL);
            if (i == cur) vita2d_draw_fill_circle(LIST_X + 18, ry + (ROW_H - 4) / 2, 5, C_ACCENT);
            if (!tracks[i].disp) {
                fit_name(tracks[i].name, 0.95f, LIST_TEXT_W, buf, sizeof(buf));
                tracks[i].disp = strdup(buf);
            }
            draw_text(LIST_X + 36, ry + 26, (i == cur) ? C_ACCENT : C_TEXT, 0.95f, tracks[i].disp ? tracks[i].disp : "");
        }
        if (ntracks > ROWS) {
            int h = ROWS * ROW_H;
            int bh = h * ROWS / ntracks;
            if (bh < 28) bh = 28;
            int by = LIST_Y + (h - bh) * top / (ntracks - ROWS);
            rrect(942, by, 5, bh, 2, C_TRACK);
        }
    }

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* ------------------------------------------------------------------ */
/* Controle                                                             */
/* ------------------------------------------------------------------ */
static void keep_sel_visible(void) {
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    if (top < 0) top = 0;
}

static void clamp_top(void) {
    int maxtop = ntracks - ROWS;
    if (maxtop < 0) maxtop = 0;
    if (top > maxtop) top = maxtop;
    if (top < 0) top = 0;
}

static void play_index(int i) {
    if (i < 0 || i >= ntracks) return;
    cur = i;
    sel = i;
    keep_sel_visible();
    paused = 0;
    a_ended = 0;
    a_fail = 0;
    a_played = 0;
    a_total = 0;
    req_load = i;
}

/* automatic = 1 quando a faixa acabou sozinha; devolve -1 para parar */
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

static void do_next(void) {
    int n = next_index(1, 0);
    if (n >= 0) play_index(n);
}

static void do_prev(void) {
    /* se já passou de 3 s, volta ao início; senão, faixa anterior */
    if (cur >= 0 && a_rate && (a_played / a_rate) > 3) {
        req_seek_s = 0;
    } else {
        int n = next_index(-1, 0);
        if (n >= 0) play_index(n);
    }
}

static void do_play_pause(void) {
    if (cur < 0) {
        play_index(sel);
    } else if (a_playing || paused) {
        paused = !paused;
    } else {
        play_index(cur); /* parado: toca de novo */
    }
}

static void do_seek_rel(int secs) {
    if (cur < 0 || !a_rate) return;
    int el = (int)(a_played / a_rate);
    int target = el + secs;
    if (target < 0) target = 0;
    req_seek_s = target;
}

static int in_circle(int x, int y, int cx, int cy, int r) {
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static void handle_tap(int x, int y) {
    /* lista */
    if (x >= LIST_X && y >= LIST_Y && y < LIST_Y + ROWS * ROW_H && x < 940) {
        int i = top + (y - LIST_Y) / ROW_H;
        if (i >= 0 && i < ntracks) play_index(i);
        return;
    }
    /* barra de progresso */
    if (x >= PB_X - 8 && x <= PB_X + PB_W + 8 && y >= PB_Y - 18 && y <= PB_Y + 26) {
        if (cur >= 0 && a_total > 0 && a_rate) {
            double frac = (double)(x - PB_X) / (double)PB_W;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            req_seek_s = (int)(frac * (double)a_total / (double)a_rate);
        }
        return;
    }
    if (in_circle(x, y, PLAY_X, CTRL_Y, 40)) { do_play_pause(); return; }
    if (in_circle(x, y, PREV_X, CTRL_Y, 34)) { do_prev(); return; }
    if (in_circle(x, y, NEXT_X, CTRL_Y, 34)) { do_next(); return; }
    if (y >= PILL_Y - 4 && y < PILL_Y + PILL_H + 4) {
        if (x >= SHUF_X && x < SHUF_X + SHUF_W) shuffle_on = !shuffle_on;
        else if (x >= REP_X && x < REP_X + REP_W) repeat_mode = (repeat_mode + 1) % 3;
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    srand((unsigned)sceKernelGetProcessTimeLow());

    /* clock maior na CPU = decodificação folgada; GPU baixa (a tela é simples) */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(111);
    scePowerSetGpuXbarClockFrequency(111);

    sceIoMkdir("ux0:/data/music", 0777);
    scan_dir("ux0:/data/music", 0);
    scan_dir("ux0:/music", 0);
    scan_dir("uma0:/data/music", 0);
    scan_dir("uma0:/music", 0);
    if (ntracks > 1) qsort(tracks, ntracks, sizeof(Track), cmp_track);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    vita2d_init();
    vita2d_set_clear_color(C_BG);
    pgf = vita2d_load_default_pgf();

    /* áudio em núcleos próprios; se a afinidade falhar, tenta sem ela */
    SceUID th_dec = sceKernelCreateThread("decoder", decoder_thread, 0x60, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (th_dec < 0) th_dec = sceKernelCreateThread("decoder", decoder_thread, 0x60, 0x40000, 0, 0, NULL);
    SceUID th = sceKernelCreateThread("audio", audio_thread, 0x40, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th < 0) th = sceKernelCreateThread("audio", audio_thread, 0x40, 0x10000, 0, 0, NULL);
    if (th_dec >= 0) sceKernelStartThread(th_dec, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);

    SceCtrlData pad, old;
    memset(&pad, 0, sizeof(pad));
    memset(&old, 0, sizeof(old));
    SceTouchData touch;
    memset(&touch, 0, sizeof(touch));
    int hold_up = 0, hold_down = 0;
    int t_down = 0, t_moved = 0, t_sx = 0, t_sy = 0, t_ly = 0, t_acc = 0;

    while (!quit_flag) {
        frame_no++;
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
        if (pressed & SCE_CTRL_RTRIGGER) do_next();
        if (pressed & SCE_CTRL_LTRIGGER) do_prev();
        if (pressed & SCE_CTRL_RIGHT) do_seek_rel(10);
        if (pressed & SCE_CTRL_LEFT) do_seek_rel(-10);

        /* navegação na lista, com repetição ao segurar */
        int prev_sel = sel;
        if (pad.buttons & SCE_CTRL_UP) hold_up++; else hold_up = 0;
        if (pad.buttons & SCE_CTRL_DOWN) hold_down++; else hold_down = 0;
        if (hold_up == 1 || (hold_up > 15 && hold_up % 3 == 0)) { if (sel > 0) sel--; }
        if (hold_down == 1 || (hold_down > 15 && hold_down % 3 == 0)) { if (sel < ntracks - 1) sel++; }
        if (sel != prev_sel) keep_sel_visible();

        /* tela de toque (ignorada com a tela apagada, para evitar toques acidentais) */
        if (!screen_off) {
            sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
            if (touch.reportNum > 0) {
                int tx = touch.report[0].x / 2;
                int ty = touch.report[0].y / 2;
                if (!t_down) {
                    t_down = 1;
                    t_moved = 0;
                    t_sx = tx;
                    t_sy = ty;
                    t_ly = ty;
                    t_acc = 0;
                } else {
                    if (abs(tx - t_sx) > 14 || abs(ty - t_sy) > 14) t_moved = 1;
                    if (t_moved && t_sx >= LIST_X && t_sy >= LIST_Y && t_sy < LIST_Y + ROWS * ROW_H) {
                        t_acc += ty - t_ly;
                        while (t_acc >= ROW_H) { top--; t_acc -= ROW_H; }
                        while (t_acc <= -ROW_H) { top++; t_acc += ROW_H; }
                        clamp_top();
                    }
                    t_ly = ty;
                }
            } else if (t_down) {
                t_down = 0;
                if (!t_moved) handle_tap(t_sx, t_sy);
            }
        } else {
            t_down = 0;
        }

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

        if (screen_off) {
            sceKernelDelayThread(60000);
        } else {
            draw_ui();
            sceKernelDelayThread(10000); /* ~30 quadros/s: sobra CPU para o áudio */
        }
    }

    quit_flag = 1;
    if (th_dec >= 0) sceKernelWaitThreadEnd(th_dec, NULL, NULL);
    if (th >= 0) sceKernelWaitThreadEnd(th, NULL, NULL);
    if (screen_off) scePowerRequestDisplayOn();
    if (pgf) vita2d_free_pgf(pgf);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
