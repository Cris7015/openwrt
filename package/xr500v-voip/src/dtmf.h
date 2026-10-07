/* SPDX-License-Identifier: GPL-2.0-or-later
 * Keypad detector shared by the call manager (dialling) and the baresip
 * audio module (in-call digits, sent as RFC 4733 events by baresip).
 * 8 kHz native s16, 20 ms windows, 40 ms make/break. No audio is recorded.
 */
#include <stdint.h>
#include <math.h>
#define DTMF_SAMPLES 160
struct dtmf_detector { char candidate, held; unsigned count, gap; };

static char dtmf_window(const int16_t *s)
{
	static const double hz[] = { 697, 770, 852, 941, 1209, 1336, 1477, 1633 };
	static const char keys[] = "123A456B789C*0#D";
	double energy = 0, mean = 0, power[8];
	int i, k, row = 0, col = 4;
	for (i = 0; i < DTMF_SAMPLES; i++) mean += s[i];
	mean /= DTMF_SAMPLES;
	for (i = 0; i < DTMF_SAMPLES; i++) energy += (s[i] - mean) * (s[i] - mean);
	if (energy < DTMF_SAMPLES * 100.0 * 100.0) return 0;
	for (k = 0; k < 8; k++) {
		double coeff = 2 * cos(2 * 3.141592653589793 * hz[k] / 8000), q1 = 0, q2 = 0;
		for (i = 0; i < DTMF_SAMPLES; i++) {
			double q = s[i] - mean + coeff * q1 - q2;
			q2 = q1; q1 = q;
		}
		power[k] = q1 * q1 + q2 * q2 - coeff * q1 * q2;
	}
	for (k = 1; k < 4; k++) if (power[k] > power[row]) row = k;
	for (k = 5; k < 8; k++) if (power[k] > power[col]) col = k;
	for (k = 0; k < 8; k++)
		if (k != row && k != col && power[k] * 4 > power[k < 4 ? row : col]) return 0;
	if (power[row] > power[col] * 4 || power[col] > power[row] * 4) return 0;
	if (2 * (power[row] + power[col]) < 0.70 * DTMF_SAMPLES * energy) return 0;
	return keys[row * 4 + col - 4];
}

static char dtmf_feed(struct dtmf_detector *d, const int16_t *s)
{
	char c = dtmf_window(s);
	if (!c) {
		if (++d->gap >= 2) d->held = 0;
		d->candidate = 0; d->count = 0;
		return 0;
	}
	d->gap = 0;
	if (d->candidate == c) { if (d->count < 3) d->count++; }
	else { d->candidate = c; d->count = 1; }
	if (d->count >= 2 && !d->held) { d->held = c; return c; }
	return 0;
}
