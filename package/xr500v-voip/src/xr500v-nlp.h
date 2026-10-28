/* Residual echo suppressor after the line echo canceller; shared by the
 * module and the offline replay. The hybrid returns the far end's voice at
 * only ~6 dB down and its band above 1 kHz is barely coherent with the
 * reference (u-law noise), so the linear canceller stops near 18 dB; what is
 * left is attenuated while only the far end talks. Speex's own residual echo
 * suppression estimates the leak by correlation and misses exactly this. */
#ifndef XR500V_NLP_H
#define XR500V_NLP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* Reference frames the echo of one capture frame can come from: the kernel's
 * reference lead, +-1 frame of ring phase and the canceller tail. */
#define NLP_HIST	10
#define NLP_FAR_RMS	50	/* far end talking above this reference rms */
#define NLP_NEAR_RMS	40	/* below this the capture holds no speech */
#define NLP_HANG	5	/* frames kept open after near-end speech */
#define NLP_ERL0	16384	/* first echo return guess, Q16 (-6 dB) */

struct xr_nlp {
	uint64_t ref_e[NLP_HIST];
	unsigned pos;
	int32_t gain, floor;	/* Q15 */
	uint32_t erl;		/* capture/reference power of the echo, Q16 */
	unsigned hang;
	uint32_t frames, suppressed, doubletalk;
};

static uint64_t nlp_energy(const int16_t *s, size_t n)
{
	uint64_t e = 0;
	size_t i;

	for (i = 0; i < n; i++)
		e += (uint64_t)((int32_t)s[i] * s[i]);
	return e;
}

static void xr_nlp_init(struct xr_nlp *n, int atten_db)
{
	memset(n, 0, sizeof(*n));
	n->gain = 32767;
	n->erl = NLP_ERL0;
	n->floor = (int32_t)(32767 * pow(10, -atten_db / 20.0));
}

/* mic: capture before the canceller; ref: its reference frame; aec: the
 * canceller output; out: the frame that leaves (aec after the denoiser),
 * scaled in place. */
static void xr_nlp_run(struct xr_nlp *n, const int16_t *mic, const int16_t *ref,
		       const int16_t *aec, int16_t *out, size_t len)
{
	uint64_t em = nlp_energy(mic, len), ee = nlp_energy(aec, len), erw = 0, echo;
	int32_t target, step, g;
	bool far, near;
	size_t i;

	n->ref_e[n->pos] = nlp_energy(ref, len);
	n->pos = (n->pos + 1) % NLP_HIST;
	for (i = 0; i < NLP_HIST; i++)
		if (n->ref_e[i] > erw)
			erw = n->ref_e[i];
	far = erw > (uint64_t)NLP_FAR_RMS * NLP_FAR_RMS * len;
	echo = (erw * n->erl) >> 16;	/* what the hybrid returns of it */
	/* Near-end speech is louder than the echo could be: 6 dB over it, or
	 * 3 dB over it while the canceller leaves more than a quarter of the
	 * capture (it cannot remove speech). The canceller alone is not trusted:
	 * a frame it gets wrong would otherwise open the echo for the hangover. */
	near = em > (uint64_t)NLP_NEAR_RMS * NLP_NEAR_RMS * len &&
	       (em > 4 * echo || (ee * 4 > em && em > 2 * echo));
	/* Learn the echo return on far-end-only frames the canceller handles. */
	if (far && !near && ee * 8 < em) {
		uint64_t r = (em << 16) / erw;

		r = r > 65536 ? 65536 : r;
		n->erl = (uint32_t)((int64_t)n->erl + ((int64_t)r - n->erl) / 100);
		if (n->erl < 1311)
			n->erl = 1311;	/* -17 dB: never assume a perfect hybrid */
	}
	if (near) {
		n->hang = NLP_HANG;
		n->doubletalk += far;
	} else if (n->hang) {
		n->hang--;
	}
	target = far && !n->hang ? n->floor : 32767;
	n->frames++;
	n->suppressed += target != 32767;
	/* Open within this frame (speech onsets), close over about two. */
	step = (target - n->gain) / (int32_t)(target > n->gain ? len : 2 * len);
	g = n->gain;
	for (i = 0; i < len; i++) {
		g += step;
		out[i] = (int16_t)(((int32_t)out[i] * g) >> 15);
	}
	n->gain = target > n->gain ? target : g;
}
#endif
