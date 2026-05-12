#include <stdint.h>

#define TMR_PRSC  0x3
#define TMR_CAS   (1 << 2)
#define TMR_IRQ   (1 << 6)
#define TMR_ENB   (1 << 7)

extern uint8_t  tmr_enb;
extern uint32_t tmr_icnt[4];

void timers_clock(uint32_t cycles);
