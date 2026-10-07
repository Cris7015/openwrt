/* Experimental mild denoiser; shared by the module and synthetic test. */
#ifndef XR500V_DENOISE_H
#define XR500V_DENOISE_H
#include <stddef.h>
#include <speex/speex_preprocess.h>

static SpeexPreprocessState *xr500v_denoise_create(void)
{
	SpeexPreprocessState *st = speex_preprocess_state_init(80, 8000);
	int enabled = 1, disabled = 0, suppression = -6, vad = -1;
	if (!st) return NULL;
	/* No AGC in our fixed-point SpeexDSP build. No VAD/gate, dereverb or
	 * residual-echo coupling; leave the proven echo canceller unchanged. */
	if (speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_DENOISE, &enabled) ||
	    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppression) ||
	    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_DEREVERB, &disabled) ||
	    speex_preprocess_ctl(st, SPEEX_PREPROCESS_GET_VAD, &vad) || vad) {
		speex_preprocess_state_destroy(st);
		return NULL;
	}
	return st;
}
#endif
