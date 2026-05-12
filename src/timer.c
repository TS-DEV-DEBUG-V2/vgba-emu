#include <stdint.h>
#include <stdbool.h>

#include "arm.h"
#include "dma.h"
#include "io.h"
#include "sound.h"
#include "timer.h"

uint8_t  tmr_enb;
uint32_t tmr_icnt[4];

/* Prescaler shift values: 1, 64, 256, 1024 => 0, 6, 8, 10 */
static const uint8_t presc_shift[4] = { 0, 6, 8, 10 };

void timers_clock(uint32_t cycles) {
    bool prev_overflow = false;

    for (uint8_t i = 0; i < 4; i++) {
        if (!(tmr_enb & (1 << i))) {
            prev_overflow = false;
            continue;
        }

        uint8_t  ctrl = tmr[i].ctrl.b.b0;
        uint32_t inc  = 0;

        if (ctrl & TMR_CAS) {
            inc = prev_overflow ? 1 : 0;
        } else {
            uint32_t shift = presc_shift[ctrl & TMR_PRSC];
            tmr_icnt[i] += cycles;
            inc = tmr_icnt[i] >> shift;
            tmr_icnt[i] -= inc << shift;
        }

        prev_overflow = false;

        if (inc == 0) continue;

        uint32_t cnt = tmr[i].count.w + inc;

        while (cnt > 0xffff) {
            uint32_t reload = tmr[i].reload.w & 0xffff;
            uint32_t span   = 0x10000 - reload;
            cnt = reload + ((cnt - 0x10000) % span);

            if (ctrl & TMR_IRQ) trigger_irq(TMR0_FLAG << i);

            prev_overflow = true;

            /* Timers 0 and 1 drive the DMA sound FIFOs.
             * On each overflow of the selected timer:
             *   - pull one sample out of the FIFO into fifo_X_samp
             *   - if FIFO is half-empty (<=16 of 32 bytes), trigger DMA refill */
            if (i < 2) {
                uint8_t pv = snd_pcm_vol.b.b1; /* high byte of SOUNDCNT_H */
                if (((pv >> 2) & 1) == i) {
                    fifo_a_load();
                    if (fifo_a_len <= 16) dma_transfer_fifo(1);
                }
                if (((pv >> 6) & 1) == i) {
                    fifo_b_load();
                    if (fifo_b_len <= 16) dma_transfer_fifo(2);
                }
            }
        }

        tmr[i].count.w = cnt;
    }
}
