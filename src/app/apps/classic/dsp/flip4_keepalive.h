#ifndef _FLIP4_KEEPALIVE_H
#define _FLIP4_KEEPALIVE_H

#include <stdint.h>

#if defined(ENA_FLIP4_KEEPALIVE)

/*
 * JBL Flip4 auto-mute workaround.
 *
 * This is not a generic tone generator.
 * It continuously injects a very small keep-alive signal.
 *
 * Target signal:
 *   - about 24 kHz
 *   - 48 kHz Fs : +gain, -gain, +gain, -gain, ...
 *   - 96 kHz Fs : +gain, +gain, -gain, -gain, ...
 *
 * Remove this module when the target speaker is changed.
 */

#ifndef FLIP4_KEEPALIVE_GAIN_LIN
#define FLIP4_KEEPALIVE_GAIN_LIN    (0.01f)
#endif

extern void flip4_keepalive_init(uint32_t sample_rate_Hz);

extern void flip4_keepalive_process(const float* in,
                                        float*       out,
                                        int          num_proc_ch,
                                        int          samples);

#endif /* defined(ENA_FLIP4_KEEPALIVE) */

#endif /* _FLIP4_KEEPALIVE_H */
