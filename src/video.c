// legacy vgba-emu ppu
// the new and maintained rewrite is in /src/ppu
// this is kept just because i want too
// there isnt a build option to use this file, if you are that desperate just patch it yourself
#include "arm.h"
#include "arm_mem.h"
#include "dma.h"
#include "io.h"
#include "sdl.h"
#include "sound.h"
#include "ppu/ppu.h"

#define LINES_VISIBLE   160
#define LINES_TOTAL     228
#define CYC_LINE_TOTAL  1232
#define CYC_LINE_HBLK0  1006
#define CYC_LINE_HBLK1  (CYC_LINE_TOTAL - CYC_LINE_HBLK0)

void *screen;

static PPU ppu;

static void ppu_sync(void) {
    ppu.dispcnt = disp_cnt.w & 0xffff;
    ppu.dispstat = disp_stat.w & 0xffff;
    ppu.vcount = v_count.w;

    for (int i = 0; i < 4; i++) {
        ppu.bg_cnt[i] = bg[i].ctrl.w & 0xffff;
        ppu.bg_hofs[i] = bg[i].xofs.w & 0x1ff;
        ppu.bg_vofs[i] = bg[i].yofs.w & 0x1ff;
    }

    ppu.bg_pa[0] = (int16_t)bg_pa[2].w;
    ppu.bg_pb[0] = (int16_t)bg_pb[2].w;
    ppu.bg_pc[0] = (int16_t)bg_pc[2].w;
    ppu.bg_pd[0] = (int16_t)bg_pd[2].w;
    ppu.bg_ref_x[0] = (int32_t)bg_refxi[2].w;
    ppu.bg_ref_y[0] = (int32_t)bg_refyi[2].w;
    ppu.bg_ref_x_latch[0] = (int32_t)bg_refxe[2].w;
    ppu.bg_ref_y_latch[0] = (int32_t)bg_refye[2].w;

    ppu.bg_pa[1] = (int16_t)bg_pa[3].w;
    ppu.bg_pb[1] = (int16_t)bg_pb[3].w;
    ppu.bg_pc[1] = (int16_t)bg_pc[3].w;
    ppu.bg_pd[1] = (int16_t)bg_pd[3].w;
    ppu.bg_ref_x[1] = (int32_t)bg_refxi[3].w;
    ppu.bg_ref_y[1] = (int32_t)bg_refyi[3].w;
    ppu.bg_ref_x_latch[1] = (int32_t)bg_refxe[3].w;
    ppu.bg_ref_y_latch[1] = (int32_t)bg_refye[3].w;

    ppu.win_h[0] = win0_h.w & 0xffff;
    ppu.win_v[0] = win0_v.w & 0xffff;
    ppu.win_h[1] = win1_h.w & 0xffff;
    ppu.win_v[1] = win1_v.w & 0xffff;

    ppu.winin = win_in.w & 0xffff;
    ppu.winout = win_out.w & 0xffff;

    ppu.bldcnt = bld_cnt.w & 0xffff;
    ppu.bldalpha = bld_alpha.w & 0xffff;
    ppu.bldy = bld_bright.w & 0xffff;

    ppu.mosaic = mosaic.w & 0xffff;

    ppu.palette_ram = pram;
    ppu.vram = vram;
    ppu.oam = oam;
}

static void ppu_sync_back(void) {
    bg_refxi[2].w = (uint32_t)ppu.bg_ref_x[0];
    bg_refyi[2].w = (uint32_t)ppu.bg_ref_y[0];
    bg_refxi[3].w = (uint32_t)ppu.bg_ref_x[1];
    bg_refyi[3].w = (uint32_t)ppu.bg_ref_y[1];
}

static uint32_t rgb555_to_bgra(uint16_t c) {
    uint32_t r = ((c >>  0) & 0x1f) << 3;
    uint32_t g = ((c >>  5) & 0x1f) << 3;
    uint32_t b = ((c >> 10) & 0x1f) << 3;
    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    return 0xff | (r << 8) | (g << 16) | (b << 24);
}

static void vblank_start(void) {
    if (disp_stat.w & VBLK_IRQ) trigger_irq(VBLK_FLAG);
    disp_stat.w |= VBLK_FLAG;
}

static void hblank_start(void) {
    if (disp_stat.w & HBLK_IRQ) trigger_irq(HBLK_FLAG);
    disp_stat.w |= HBLK_FLAG;
}

static void vcount_match(void) {
    if (disp_stat.w & VCNT_IRQ) trigger_irq(VCNT_FLAG);
    disp_stat.w |= VCNT_FLAG;
}

void run_frame(void) {
    disp_stat.w &= ~VBLK_FLAG;

    SDL_LockTexture(texture, NULL, &screen, &tex_pitch);

    for (v_count.w = 0; v_count.w < LINES_TOTAL; v_count.w++) {
        disp_stat.w &= ~(HBLK_FLAG | VCNT_FLAG);

        if (v_count.w == disp_stat.b.b1) vcount_match();

        if (v_count.w == LINES_VISIBLE) {
            bg_refxi[2].w = bg_refxe[2].w;
            bg_refyi[2].w = bg_refye[2].w;
            bg_refxi[3].w = bg_refxe[3].w;
            bg_refyi[3].w = bg_refye[3].w;

            vblank_start();
            dma_transfer(VBLANK);
        }

        if (v_count.w < LINES_VISIBLE) {
            arm_exec(CYC_LINE_HBLK0);

            ppu_sync();
            ppu_render_scanline(&ppu);
            ppu_sync_back();

            uint32_t *out = (uint32_t *)((uint8_t *)screen + v_count.w * tex_pitch);
            for (int x = 0; x < 240; x++)
                out[x] = rgb555_to_bgra(ppu.framebuffer[v_count.w * 240 + x]);
        } else {
            arm_exec(CYC_LINE_HBLK0);
        }

        dma_transfer(HBLANK);

        hblank_start();

        arm_exec(CYC_LINE_HBLK1);

        sound_clock(CYC_LINE_TOTAL);
    }

    SDL_UnlockTexture(texture);

    SDL_Rect src = {0, 0, WIDTH, HEIGHT};
    int dst_w, dst_h;
    if (win_w * HEIGHT > win_h * WIDTH) {
        dst_h = win_h;
        dst_w = win_h * WIDTH / HEIGHT;
    } else {
        dst_w = win_w;
        dst_h = win_w * HEIGHT / WIDTH;
    }
    SDL_Rect dst = {(win_w - dst_w) / 2, (win_h - dst_h) / 2, dst_w, dst_h};
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, &src, &dst);
    SDL_RenderPresent(renderer);

    sound_buffer_wrap();
}
