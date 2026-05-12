#include <stdbool.h>
#include <stdint.h>

#define CPU_FREQ_HZ       16777216
#define SND_FREQUENCY     32768
#define SND_CHANNELS      2
#define SND_SAMPLES       512
#define SAMP_CYCLES       (CPU_FREQ_HZ / SND_FREQUENCY)
#define BUFF_SAMPLES      ((SND_SAMPLES) * 16 * 2)
#define BUFF_SAMPLES_MSK  ((BUFF_SAMPLES) - 1)

#define PHASE_FRAC 16
#define PHASE_ONE  (1 << PHASE_FRAC)

extern int8_t fifo_a[0x20];
extern int8_t fifo_b[0x20];

extern uint8_t fifo_a_len;
extern uint8_t fifo_b_len;

typedef struct {
    uint32_t phase_acc;       //16.16 fixed-point phase accumulator (all)
    uint32_t length_cnt;      //samples remaining for length counter (all)
    uint32_t sweep_cnt;       //samples remaining for sweep (sqr1 only)
    uint32_t env_cnt;         //samples remaining for envelope step (sqr1/2, noise)
    uint16_t lfsr;            //noise LFSR
    uint8_t  envelope;        //current envelope volume 0-15
} snd_ch_state_t;

extern snd_ch_state_t snd_ch_state[4];

extern uint8_t wave_position;
extern uint8_t wave_samples;

void wave_reset();

void sound_buffer_wrap();

void sound_mix(void *data, uint8_t *stream, int32_t len);
void sound_clock(uint32_t cycles);

void fifo_a_copy();
void fifo_b_copy();

void fifo_a_load();
void fifo_b_load();