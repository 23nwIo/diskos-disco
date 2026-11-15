/* alsaprobe: show what hw:0,3 accepts and which buffer/period settings work (diagnosis for MPD, 2026-11-15) */
#include <alsa/asoundlib.h>
#include <stdio.h>
static snd_output_t *out;
static int try_(const char *dev, unsigned rate, snd_pcm_format_t fmt, long buf_us, long per_us, int per_first){
    snd_pcm_t *p; int e;
    if((e = snd_pcm_open(&p, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0){ printf("open %s: %s\n", dev, snd_strerror(e)); return e; }
    snd_pcm_hw_params_t *h; snd_pcm_hw_params_alloca(&h);
    snd_pcm_hw_params_any(p, h);
    const char *step = "access";
    if((e = snd_pcm_hw_params_set_access(p, h, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) goto bad;
    step = "format"; if((e = snd_pcm_hw_params_set_format(p, h, fmt)) < 0) goto bad;
    step = "channels"; if((e = snd_pcm_hw_params_set_channels(p, h, 2)) < 0) goto bad;
    step = "rate"; unsigned r = rate; if((e = snd_pcm_hw_params_set_rate_near(p, h, &r, 0)) < 0) goto bad;
    unsigned bt = buf_us, pt = per_us;
    if(per_first && per_us){ step = "period_time"; if((e = snd_pcm_hw_params_set_period_time_near(p, h, &pt, 0)) < 0) goto bad; }
    if(buf_us){ step = "buffer_time"; if((e = snd_pcm_hw_params_set_buffer_time_near(p, h, &bt, 0)) < 0) goto bad; }
    if(!per_first && per_us){ step = "period_time"; if((e = snd_pcm_hw_params_set_period_time_near(p, h, &pt, 0)) < 0) goto bad; }
    step = "hw_params"; if((e = snd_pcm_hw_params(p, h)) < 0) goto bad;
    snd_pcm_uframes_t bs, ps; snd_pcm_hw_params_get_buffer_size(h, &bs); snd_pcm_hw_params_get_period_size(h, &ps, 0);
    printf("OK   %s rate %u fmt %s buf %ld per %ld %s -> buffer %lu period %lu frames\n", dev, r, snd_pcm_format_name(fmt),
           buf_us, per_us, per_first ? "(period first)" : "", (unsigned long)bs, (unsigned long)ps);
    snd_pcm_close(p); return 0;
bad:
    printf("FAIL %s rate %u fmt %s buf %ld per %ld %s: %s: %s\n", dev, rate, snd_pcm_format_name(fmt), buf_us, per_us,
           per_first ? "(period first)" : "", step, snd_strerror(e));
    snd_pcm_close(p); return e;
}
int main(int argc, char **argv){
    const char *dev = argc > 1 ? argv[1] : "hw:0,3";
    snd_output_stdio_attach(&out, stdout, 0);
    snd_pcm_t *p; int e = snd_pcm_open(&p, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if(e < 0){ printf("open %s: %s\n", dev, snd_strerror(e)); return 1; }
    snd_pcm_hw_params_t *h; snd_pcm_hw_params_alloca(&h); snd_pcm_hw_params_any(p, h);
    printf("--- full parameter space of %s\n", dev); snd_pcm_hw_params_dump(h, out); snd_pcm_close(p);
    printf("--- tries\n");
    long bufs[] = { 500000, 200000, 100000, 50000, 0 };
    for(int i = 0; i < 5; i++) try_(dev, 44100, SND_PCM_FORMAT_S16_LE, bufs[i], 0, 0);
    try_(dev, 44100, SND_PCM_FORMAT_S16_LE, 500000, 125000, 0);
    try_(dev, 44100, SND_PCM_FORMAT_S16_LE, 500000, 125000, 1);
    try_(dev, 44100, SND_PCM_FORMAT_S16_LE, 0, 0, 0);
    try_(dev, 44100, SND_PCM_FORMAT_S32_LE, 0, 0, 0);
    try_(dev, 44100, SND_PCM_FORMAT_S24_LE, 0, 0, 0);
    try_(dev, 96000, SND_PCM_FORMAT_S32_LE, 500000, 0, 0);
    return 0;
}
