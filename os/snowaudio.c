/* snowaudio.c - SnowAudio: biblioteca da SnowPaula (vozes PCM/ADPCM, canais legados, sequenciador, utilitários) */
#include "snowos.h"

void snd_init(void) { REG(PAULA_BASE + PA_MASTER) = 256; }
void snd_master(int vol) { REG(PAULA_BASE + PA_MASTER) = (unsigned)vol; }
void snd_load(unsigned aram_off, const void *src, unsigned len) { dma_copy(ARAM_BASE + aram_off, (unsigned)src, len, 0); }

void snd_voice_play(int v, unsigned aram_off, unsigned nsamples, int fmt, unsigned step_q16, int vol_l, int vol_r, int loop) {
  unsigned b = PAULA_BASE + PA_VOICE0 + (unsigned)v * 0x40;
  REG(b + PV_CTRL) = 0;
  REG(b + PV_ADDR) = aram_off; REG(b + PV_LEN) = nsamples; REG(b + PV_LOOP_START) = 0; REG(b + PV_LOOP_LEN) = nsamples;
  REG(b + PV_STEP) = step_q16; REG(b + PV_VOL_L) = (unsigned)vol_l; REG(b + PV_VOL_R) = (unsigned)vol_r;
  REG(b + PV_CTRL) = 1 | (loop ? 2 : 0) | ((unsigned)fmt << 2);
}
void snd_voice_stop(int v) { REG(PAULA_BASE + PA_VOICE0 + (unsigned)v * 0x40 + PV_CTRL) = 0; }

void snd_legacy_play(int ch, unsigned addr_aram, unsigned len_words, unsigned period, int vol, int loop) {
  unsigned b = PAULA_BASE + PA_LEG0 + (unsigned)ch * 0x10;
  REG(b) = addr_aram; REG(b + 4) = len_words; REG(b + 8) = period; REG(b + 12) = (unsigned)vol | 0x80000000u | (loop ? 0x40000000u : 0);
}
void snd_seq_play(unsigned ddr_addr, int nevents, int loop) {
  os_cache_flush(ddr_addr, (unsigned)nevents * 16);
  REG(PAULA_BASE + PA_SEQ_ADDR) = ddr_addr; REG(PAULA_BASE + PA_SEQ_LEN) = (unsigned)nevents; REG(PAULA_BASE + PA_SEQ_CTRL) = 1 | (loop ? 2 : 0);
}

/* senoide em inteiros: parábola (precisão ~1%) suficiente para tons de teste */
int snd_sine(short *dst, int n, int period, int amp) {
  int i;
  for (i = 0; i < n; i++) {
    int ph = (i % period) * 1024 / period;               /* 0..1023 */
    int h = ph & 511, v;                                 /* meia onda */
    v = (h * (512 - h)) / 64;                            /* 0..1024 */
    if (ph >= 512) v = -v;
    dst[i] = (short)(v * amp / 1024);
  }
  return n;
}
