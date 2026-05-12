#include <stdint.h>

#include "io.h"
#include "sound.h"

int8_t fifo_a[0x20];
int8_t fifo_b[0x20];

uint8_t fifo_a_len;
uint8_t fifo_b_len;

snd_ch_state_t snd_ch_state[4];

uint8_t wave_position;
uint8_t wave_samples;

#define PSG_MAX   0x7f
#define PSG_MIN  -0x80

#define SAMP_MAX   0x1ff
#define SAMP_MIN  -0x200

static const uint16_t duty_thresh[4] = { 8192, 16384, 32768, 49152 };

static int8_t scale_psg(int32_t val, int32_t num, int32_t den) {
    return (int8_t)((val * num + (den >> 1)) / den);
}

static uint32_t sqr_phase_inc(uint16_t freq_hz) {
    uint16_t denominator = 2048 - (freq_hz & 0x7ff);
    if (denominator == 0) return 0;
    return 262144 / denominator;
}

static uint32_t wave_phase_inc(uint16_t freq_hz) {
    uint16_t denominator = 2048 - (freq_hz & 0x7ff);
    if (denominator == 0) return 0;
    return 4194304 / denominator;
}

static uint32_t noise_cycle_samples(uint8_t freq_div, uint8_t freq_rsh) {
    uint32_t base = freq_div ? 524288u : 1048576u;
    uint32_t divisor = freq_div ? (uint32_t)freq_div : 1u;
    uint32_t freq = (base / divisor) >> (freq_rsh + 1);
    if (freq == 0) freq = 1;
    return SND_FREQUENCY / freq;
}

static int8_t square_sample(uint8_t ch) {
    if (!(snd_psg_enb.w & (CH_SQR1 << ch))) return 0;

    snd_ch_state_t *st = &snd_ch_state[ch];

    uint8_t  sweep_time = (sqr_ch[ch].sweep.w >>  4) & 0x7;
    uint8_t  duty       = (sqr_ch[ch].tone.w  >>  6) & 0x3;
    uint8_t  env_step   = (sqr_ch[ch].tone.w  >>  8) & 0x7;
    uint16_t freq_hz    = (sqr_ch[ch].ctrl.w  >>  0) & 0x7ff;

    if (sqr_ch[ch].ctrl.w & CH_LEN) {
        if (st->length_cnt == 0) {
            snd_psg_enb.w &= ~(CH_SQR1 << ch);
            return 0;
        }
        st->length_cnt--;
    }

    if (ch == 0) {
        uint8_t sweep_shift = sqr_ch[0].sweep.w & 7;
        if (sweep_shift && sweep_time) {
            if (st->sweep_cnt == 0) {
                st->sweep_cnt = (sweep_time + 1) * 256;
                uint32_t disp = freq_hz >> sweep_shift;
                if (sqr_ch[0].sweep.w & SWEEP_DEC)
                    freq_hz -= disp;
                else
                    freq_hz += disp;
                if (freq_hz < 0x7ff) {
                    sqr_ch[0].ctrl.w &= ~0x7ff;
                    sqr_ch[0].ctrl.w |= freq_hz;
                } else {
                    snd_psg_enb.w &= ~CH_SQR1;
                    return 0;
                }
            }
            st->sweep_cnt--;
        }
    }

    uint8_t envelope = st->envelope;
    if (env_step) {
        if (st->env_cnt == 0) {
            st->env_cnt = env_step * 512;
            if (sqr_ch[ch].tone.w & ENV_INC) {
                if (envelope < 0xf) envelope++;
            } else {
                if (envelope > 0x0) envelope--;
            }
            st->envelope = envelope;
            sqr_ch[ch].tone.w = (sqr_ch[ch].tone.w & ~0xf000) | (envelope << 12);
        }
        st->env_cnt--;
    }

    uint32_t inc = sqr_phase_inc(freq_hz);
    st->phase_acc += inc;
    if (st->phase_acc >= PHASE_ONE)
        st->phase_acc -= PHASE_ONE;
    bool high = st->phase_acc < duty_thresh[duty];

    return high
        ? scale_psg(PSG_MAX, envelope, 15)
        : scale_psg(PSG_MIN, envelope, 15);
}

static int8_t wave_sample() {
    if (!((snd_psg_enb.w & CH_WAVE) && (wave_ch.wave.w & WAVE_PLAY))) return 0;

    snd_ch_state_t *st = &snd_ch_state[2];

    uint8_t  volume = (wave_ch.volume.w >> 13) & 0x7;
    uint16_t freq_hz = (wave_ch.ctrl.w   >>  0) & 0x7ff;

    if (wave_ch.ctrl.w & CH_LEN) {
        if (st->length_cnt == 0) {
            snd_psg_enb.w &= ~CH_WAVE;
            return 0;
        }
        st->length_cnt--;
    }

    uint32_t inc = wave_phase_inc(freq_hz);
    st->phase_acc += inc;
    if (st->phase_acc >= PHASE_ONE) {
        st->phase_acc -= PHASE_ONE;
        wave_position = (wave_position + 1) & 0x3f;
        if (--wave_samples == 0)
            wave_reset();
    }

    int8_t samp = wave_position & 1
        ? ((wave_ram[(wave_position >> 1) & 0x1f] >> 0) & 0xf) - 8
        : ((wave_ram[(wave_position >> 1) & 0x1f] >> 4) & 0xf) - 8;

    switch (volume) {
        case 0: samp = 0; break;
        case 2: samp >>= 1; break;
        case 3: samp >>= 2; break;
        case 4: case 5: case 6: case 7: samp = (samp >> 2) * 3; break;
    }

    return samp >= 0
        ? scale_psg(PSG_MAX, samp, 7)
        : scale_psg(PSG_MIN, samp, -8);
}

static int8_t noise_sample() {
    if (!(snd_psg_enb.w & CH_NOISE)) return 0;

    snd_ch_state_t *st = &snd_ch_state[3];

    uint8_t env_step = (noise_ch.env.w  >>  8) & 0x7;
    uint8_t freq_div = (noise_ch.ctrl.w >>  0) & 0x7;
    uint8_t freq_rsh = (noise_ch.ctrl.w >>  4) & 0xf;

    if (noise_ch.ctrl.w & CH_LEN) {
        if (st->length_cnt == 0) {
            snd_psg_enb.w &= ~CH_NOISE;
            return 0;
        }
        st->length_cnt--;
    }

    uint8_t envelope = st->envelope;
    if (env_step) {
        if (st->env_cnt == 0) {
            st->env_cnt = env_step * 512;
            if (noise_ch.env.w & ENV_INC) {
                if (envelope < 0xf) envelope++;
            } else {
                if (envelope > 0x0) envelope--;
            }
            st->envelope = envelope;
            noise_ch.env.w = (noise_ch.env.w & ~0xf000) | (envelope << 12);
        }
        st->env_cnt--;
    }

    st->phase_acc++;
    uint32_t period = noise_cycle_samples(freq_div, freq_rsh);
    if (st->phase_acc >= period) {
        st->phase_acc -= period;
        uint8_t carry = st->lfsr & 1;
        st->lfsr >>= 1;
        uint8_t high = (st->lfsr & 1) ^ carry;
        if (noise_ch.ctrl.w & NOISE_7)
            st->lfsr |= (uint16_t)high << 6;
        else
            st->lfsr |= (uint16_t)high << 14;
    }

    return (st->lfsr & 1)
        ? scale_psg(PSG_MAX, envelope, 15)
        : scale_psg(PSG_MIN, envelope, 15);
}

int16_t snd_buffer[BUFF_SAMPLES];

uint32_t snd_cur_play  = 0;
uint32_t snd_cur_write = 0x200;

void wave_reset() {
    if (wave_ch.wave.w & WAVE_64) {
        wave_position = 0;
        wave_samples  = 64;
    } else {
        wave_position = (wave_ch.wave.w >> 1) & 0x20;
        wave_samples  = 32;
    }
}

void sound_buffer_wrap() {
    if ((snd_cur_play / BUFF_SAMPLES) == (snd_cur_write / BUFF_SAMPLES)) {
        snd_cur_play  &= BUFF_SAMPLES_MSK;
        snd_cur_write &= BUFF_SAMPLES_MSK;
    }
}

void sound_mix(void *data, uint8_t *stream, int32_t len) {
    int32_t gap = (int32_t)(snd_cur_write - snd_cur_play);
    int32_t needed = len / 2;
    if (gap < needed) {
        for (int32_t i = 0; i < len; i += 4) {
            *(int16_t *)(stream + i)     = 0;
            *(int16_t *)(stream + i + 2) = 0;
        }
        return;
    }
    for (int32_t i = 0; i < len; i += 4) {
        *(int16_t *)(stream + i)     = snd_buffer[snd_cur_play++ & BUFF_SAMPLES_MSK] << 6;
        *(int16_t *)(stream + i + 2) = snd_buffer[snd_cur_play++ & BUFF_SAMPLES_MSK] << 6;
    }
}

void fifo_a_copy() {
    if (fifo_a_len + 4 > 0x20) return;
    fifo_a[fifo_a_len++] = snd_fifo_a_0;
    fifo_a[fifo_a_len++] = snd_fifo_a_1;
    fifo_a[fifo_a_len++] = snd_fifo_a_2;
    fifo_a[fifo_a_len++] = snd_fifo_a_3;
}

void fifo_b_copy() {
    if (fifo_b_len + 4 > 0x20) return;
    fifo_b[fifo_b_len++] = snd_fifo_b_0;
    fifo_b[fifo_b_len++] = snd_fifo_b_1;
    fifo_b[fifo_b_len++] = snd_fifo_b_2;
    fifo_b[fifo_b_len++] = snd_fifo_b_3;
}

int8_t fifo_a_samp;
int8_t fifo_b_samp;

static void fifo_shift(int8_t *buf, uint8_t *len) {
    if (*len) {
        for (uint8_t i = 0; i < *len - 1; i++)
            buf[i] = buf[i + 1];
        (*len)--;
    }
}

void fifo_a_load() {
    if (fifo_a_len) {
        fifo_a_samp = fifo_a[0];
        fifo_shift(fifo_a, &fifo_a_len);
    }
}

void fifo_b_load() {
    if (fifo_b_len) {
        fifo_b_samp = fifo_b[0];
        fifo_shift(fifo_b, &fifo_b_len);
    }
}

uint32_t snd_cycles = 0;

static const int32_t psg_vol_lut[8] = { 0x000, 0x024, 0x049, 0x06d, 0x092, 0x0b6, 0x0db, 0x100 };
static const int32_t psg_rsh_lut[4] = { 0xa, 0x9, 0x8, 0x7 };

static int16_t clip(int32_t value) {
    if (value > SAMP_MAX) value = SAMP_MAX;
    if (value < SAMP_MIN) value = SAMP_MIN;
    return value;
}

void sound_clock(uint32_t cycles) {
    snd_cycles += cycles;

    int16_t samp_ch4 = (fifo_a_samp << 1) >> !(snd_pcm_vol.w & 4);
    int16_t samp_ch5 = (fifo_b_samp << 1) >> !(snd_pcm_vol.w & 8);

    int16_t samp_pcm_l = 0;
    int16_t samp_pcm_r = 0;

    if (snd_pcm_vol.w & CH_DMAA_L) samp_pcm_l = clip(samp_pcm_l + samp_ch4);
    if (snd_pcm_vol.w & CH_DMAB_L) samp_pcm_l = clip(samp_pcm_l + samp_ch5);
    if (snd_pcm_vol.w & CH_DMAA_R) samp_pcm_r = clip(samp_pcm_r + samp_ch4);
    if (snd_pcm_vol.w & CH_DMAB_R) samp_pcm_r = clip(samp_pcm_r + samp_ch5);

    while (snd_cycles >= SAMP_CYCLES) {
        int16_t samp_ch0 = square_sample(0);
        int16_t samp_ch1 = square_sample(1);
        int16_t samp_ch2 = wave_sample();
        int16_t samp_ch3 = noise_sample();

        int32_t samp_psg_l = 0;
        int32_t samp_psg_r = 0;

        if (snd_psg_vol.w & CH_SQR1_L)  samp_psg_l = clip(samp_psg_l + samp_ch0);
        if (snd_psg_vol.w & CH_SQR2_L)  samp_psg_l = clip(samp_psg_l + samp_ch1);
        if (snd_psg_vol.w & CH_WAVE_L)  samp_psg_l = clip(samp_psg_l + samp_ch2);
        if (snd_psg_vol.w & CH_NOISE_L) samp_psg_l = clip(samp_psg_l + samp_ch3);

        if (snd_psg_vol.w & CH_SQR1_R)  samp_psg_r = clip(samp_psg_r + samp_ch0);
        if (snd_psg_vol.w & CH_SQR2_R)  samp_psg_r = clip(samp_psg_r + samp_ch1);
        if (snd_psg_vol.w & CH_WAVE_R)  samp_psg_r = clip(samp_psg_r + samp_ch2);
        if (snd_psg_vol.w & CH_NOISE_R) samp_psg_r = clip(samp_psg_r + samp_ch3);

        samp_psg_l  *= psg_vol_lut[(snd_psg_vol.w >> 4) & 7];
        samp_psg_r  *= psg_vol_lut[(snd_psg_vol.w >> 0) & 7];

        samp_psg_l >>= psg_rsh_lut[(snd_pcm_vol.w >> 0) & 3];
        samp_psg_r >>= psg_rsh_lut[(snd_pcm_vol.w >> 0) & 3];

        snd_buffer[snd_cur_write++ & BUFF_SAMPLES_MSK] = clip(samp_psg_l + samp_pcm_l);
        snd_buffer[snd_cur_write++ & BUFF_SAMPLES_MSK] = clip(samp_psg_r + samp_pcm_r);

        snd_cycles -= SAMP_CYCLES;
    }
}