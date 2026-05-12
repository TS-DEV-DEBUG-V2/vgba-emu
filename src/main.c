#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "arm.h"
#include "arm_mem.h"
#include "io.h"
#include "sdl.h"
#include "sound.h"
#include "video.h"

SDL_Window   *window;
SDL_Renderer *renderer;
SDL_Texture  *texture;
int           tex_pitch;
int           win_w;
int           win_h;

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/fetch.h>
#endif

#define SCALE 3

/* Embedded BIOS bytes (generated from gba_bios.bin via `xxd -i`). */
extern unsigned char gba_bios[];
extern unsigned int  gba_bios_len;

/* ---------- Helpers ---------- */

static int load_file_into(const char *path, uint8_t *dst, size_t max, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    if ((size_t)sz > max) sz = (long)max;
    size_t n = fread(dst, 1, (size_t)sz, f);
    fclose(f);
    if (out_size) *out_size = n;
    return 0;
}

static uint32_t round_up_pow2(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >>  1; v |= v >>  2; v |= v >>  4;
    v |= v >>  8; v |= v >> 16;
    return v + 1;
}

/* ---------- Save / backup ----------
 *
 * Detect backup type by scanning ROM for the standard ID strings:
 *   SRAM_V       -> 32KB SRAM
 *   FLASH_V      -> 64KB Flash
 *   FLASH512_V   -> 64KB Flash
 *   FLASH1M_V    -> 128KB Flash
 *   EEPROM_V     -> EEPROM (size varies, default to 8KB)
 */
enum { SAVE_SRAM, SAVE_FLASH64, SAVE_FLASH128, SAVE_EEPROM4K, SAVE_EEPROM64K, SAVE_NONE };
static int  save_type = SAVE_NONE;
static char save_path[1024];

static int  save_size_for_type(int t) {
    switch (t) {
        case SAVE_SRAM:      return 0x8000;
        case SAVE_FLASH64:   return 0x10000;
        case SAVE_FLASH128:  return 0x20000;
        case SAVE_EEPROM4K:  return 0x200;
        case SAVE_EEPROM64K: return 0x2000;
    }
    return 0;
}
static uint8_t *save_buf_for_type(int t) {
    switch (t) {
        case SAVE_SRAM:      return sram;
        case SAVE_FLASH64:
        case SAVE_FLASH128:  return flash;
        case SAVE_EEPROM4K:
        case SAVE_EEPROM64K: return eeprom;
    }
    return NULL;
}

static void detect_save_type(size_t rom_size) {
    static const struct { const char *id; size_t len; int type; } ids[] = {
        { "EEPROM_V",   8, SAVE_EEPROM64K },
        { "SRAM_V",     6, SAVE_SRAM      },
        { "SRAM_F_V",   8, SAVE_SRAM      },
        { "FLASH1M_V", 9, SAVE_FLASH128  },
        { "FLASH512_V",10, SAVE_FLASH64   },
        { "FLASH_V",    7, SAVE_FLASH64   },
        { NULL, 0, 0 }
    };
    for (size_t i = 0; i + 16 < rom_size; i += 4) {
        for (int j = 0; ids[j].id; j++) {
            if (memcmp(rom + i, ids[j].id, ids[j].len) == 0) {
                save_type = ids[j].type;
                return;
            }
        }
    }
    save_type = SAVE_SRAM; /* common default */
}

static void load_save_native(void) {
    if (save_type == SAVE_NONE) return;
    FILE *f = fopen(save_path, "rb");
    if (!f) return;
    fread(save_buf_for_type(save_type), 1, save_size_for_type(save_type), f);
    fclose(f);
    fprintf(stderr, "[save] loaded %s (%d bytes)\n", save_path, save_size_for_type(save_type));
}

static void write_save_native(void) {
    if (save_type == SAVE_NONE) return;
    FILE *f = fopen(save_path, "wb");
    if (!f) { fprintf(stderr, "[save] failed to write %s\n", save_path); return; }
    fwrite(save_buf_for_type(save_type), 1, save_size_for_type(save_type), f);
    fclose(f);
}

#ifdef __EMSCRIPTEN__
/* Web save: localStorage keyed by ROM URL. Base64 encoded. */
EM_JS(void, em_save_store, (const char *key, const uint8_t *data, int len), {
    var k = UTF8ToString(key);
    var bin = "";
    for (var i = 0; i < len; i++) bin += String.fromCharCode(HEAPU8[data + i]);
    try { localStorage.setItem("vgba:" + k, btoa(bin)); } catch(e) { console.warn(e); }
});
EM_JS(int, em_save_load, (const char *key, uint8_t *out, int max), {
    var k = UTF8ToString(key);
    var s; try { s = localStorage.getItem("vgba:" + k); } catch(e) { return 0; }
    if (!s) return 0;
    var bin; try { bin = atob(s); } catch(e) { return 0; }
    var n = Math.min(bin.length, max);
    for (var i = 0; i < n; i++) HEAPU8[out + i] = bin.charCodeAt(i);
    return n;
});

static char save_key[512];
static void load_save_web(void) {
    if (save_type == SAVE_NONE) return;
    em_save_load(save_key, save_buf_for_type(save_type), save_size_for_type(save_type));
}
static void write_save_web(void) {
    if (save_type == SAVE_NONE) return;
    em_save_store(save_key, save_buf_for_type(save_type), save_size_for_type(save_type));
}
#endif

static void write_save(void) {
#ifdef __EMSCRIPTEN__
    write_save_web();
#else
    write_save_native();
#endif
}

/* ---------- Input ---------- */

static void key_set(SDL_Keycode k, bool down) {
    uint16_t mask = 0;
    switch (k) {
        case SDLK_z:         mask = BTN_A;   break;
        case SDLK_x:         mask = BTN_B;   break;
        case SDLK_BACKSPACE: mask = BTN_SEL; break;
        case SDLK_RETURN:    mask = BTN_STA; break;
        case SDLK_RIGHT:     mask = BTN_RT;  break;
        case SDLK_LEFT:      mask = BTN_LT;  break;
        case SDLK_UP:        mask = BTN_U;   break;
        case SDLK_DOWN:      mask = BTN_D;   break;
        case SDLK_n:         mask = BTN_R;   break;
        case SDLK_m:         mask = BTN_L;   break;
        default: return;
    }
    if (down) key_input.w &= ~mask;
    else      key_input.w |=  mask;
}

/* ---------- SDL setup ---------- */

static SDL_AudioDeviceID audio_dev;

static void audio_callback(void *ud, Uint8 *stream, int len) {
    sound_mix(ud, stream, len);
}

static int sdl_setup(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    win_w = WIDTH * SCALE;
    win_h = HEIGHT * SCALE;
    window = SDL_CreateWindow("vgba-emu by TS (plz dont @) copyright 2026",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return -1; }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return -1; }

    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_BGRA8888,
        SDL_TEXTUREACCESS_STREAMING,
        WIDTH, HEIGHT);
    if (!texture) { fprintf(stderr, "CreateTexture: %s\n", SDL_GetError()); return -1; }

    SDL_AudioSpec want, got;
    SDL_zero(want);
    want.freq     = SND_FREQUENCY;
    want.format   = AUDIO_S16SYS;
    want.channels = SND_CHANNELS;
    want.samples  = SND_SAMPLES;
    want.callback = audio_callback;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (audio_dev == 0)
        fprintf(stderr, "OpenAudio failed (%s) - running silently\n", SDL_GetError());
    else
        SDL_PauseAudioDevice(audio_dev, 0);
    return 0;
}

static void sdl_teardown(void) {
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    if (texture)   SDL_DestroyTexture(texture);
    if (renderer)  SDL_DestroyRenderer(renderer);
    if (window)    SDL_DestroyWindow(window);
    SDL_Quit();
}

/* ---------- Boot ---------- */

static void boot(size_t rom_size) {
    cart_rom_size = (int64_t)rom_size;
    cart_rom_mask = round_up_pow2((uint32_t)rom_size) - 1;
    if (cart_rom_mask == 0) cart_rom_mask = 0x01FFFFFF;

    /* Embedded BIOS - always present, baked into the binary. */
    memcpy(bios, gba_bios, gba_bios_len < 0x4000 ? gba_bios_len : 0x4000);

    detect_save_type(rom_size);
    arm_reset();
}

/* ---------- Event pump ---------- */

static bool quit_flag = false;
static uint32_t save_tick = 0;

static void pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit_flag = true;
        else if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_ESCAPE) quit_flag = true;
            else key_set(e.key.keysym.sym, true);
        } else if (e.type == SDL_KEYUP) {
            key_set(e.key.keysym.sym, false);
        } else if (e.type == SDL_WINDOWEVENT) {
            if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
                win_w = e.window.data1;
                win_h = e.window.data2;
            }
        }
    }
}

/* ================================================================ */
#ifdef __EMSCRIPTEN__

static double web_last_t = 0;
static const double web_frame_s = 1.0 / 59.7275;

static void web_frame(void) {
    pump_events();
    run_frame();
    if (++save_tick >= 300) { save_tick = 0; write_save(); }

    double now = emscripten_get_now() / 1000.0;
    double elapsed = now - web_last_t;
    if (elapsed < web_frame_s) {
        double ms = (web_frame_s - elapsed) * 1000.0;
        if (ms > 1) emscripten_sleep(ms);
    }
    web_last_t = emscripten_get_now() / 1000.0;
}

static void rom_loaded_cb(emscripten_fetch_t *f) {
    if (f->numBytes > 0x2000000) {
        fprintf(stderr, "ROM too large (%llu bytes)\n", (unsigned long long)f->numBytes);
        emscripten_fetch_close(f);
        return;
    }
    size_t sz = (size_t)f->numBytes;
    memcpy(rom, f->data, sz);
    /* Make save key based on URL */
    strncpy(save_key, f->url, sizeof(save_key) - 1);
    emscripten_fetch_close(f);

    boot(sz);
    load_save_web();

    /* Save on tab close */
    EM_ASM({
        window.addEventListener('beforeunload', function() {
            Module.ccall('write_save', 'void', [], []);
        });
    });

    emscripten_set_main_loop(web_frame, 0, 1);
}
static void rom_failed_cb(emscripten_fetch_t *f) {
    fprintf(stderr, "Failed to fetch ROM (HTTP %d)\n", f->status);
    emscripten_fetch_close(f);
}

int main(void) {
    arm_init();
    if (sdl_setup() != 0) return 1;

    char *url = (char *)EM_ASM_INT({
        var u = (window.gbaromurl || "rom.gba");
        var l = lengthBytesUTF8(u) + 1;
        var p = _malloc(l);
        stringToUTF8(u, p, l);
        return p;
    });
    emscripten_fetch_attr_t a;
    emscripten_fetch_attr_init(&a);
    strcpy(a.requestMethod, "GET");
    a.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    a.onsuccess  = rom_loaded_cb;
    a.onerror    = rom_failed_cb;
    emscripten_fetch(&a, url);
    free(url);
    return 0;
}

/* Exported so beforeunload can call it. */
void __attribute__((used)) write_save_export(void) { write_save(); }

/* ================================================================ */
#else /* native */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s ROM.gba\n", argv[0]);
        return 1;
    }

    arm_init();

    size_t rom_sz = 0;
    if (load_file_into(argv[1], rom, 0x2000000, &rom_sz) != 0 || rom_sz == 0) {
        fprintf(stderr, "Failed to read ROM: %s\n", argv[1]);
        return 1;
    }

    /* Compute save file path: <rom>.sav */
    snprintf(save_path, sizeof(save_path), "%s.sav", argv[1]);

    if (sdl_setup() != 0) return 1;
    boot(rom_sz);
    load_save_native();

    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 last = SDL_GetPerformanceCounter();
    const double frame_s = 1.0 / 59.7275;

    while (!quit_flag) {
        pump_events();
        run_frame();

        /* Auto-save every ~5 seconds */
        if (++save_tick >= 300) { save_tick = 0; write_save(); }

        Uint64 now = SDL_GetPerformanceCounter();
        double elapsed = (double)(now - last) / (double)freq;
        if (elapsed < frame_s) {
            Uint32 ms = (Uint32)((frame_s - elapsed) * 1000.0);
            if (ms > 0) SDL_Delay(ms);
        }
        last = SDL_GetPerformanceCounter();
    }

    /* Save on exit */
    write_save();

    sdl_teardown();
    arm_uninit();
    return 0;
}

#endif
