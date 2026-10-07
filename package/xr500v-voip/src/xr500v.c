/**
 * @file xr500v.c  baresip audio driver for the TP-Link Archer XR500v FXS line
 *
 * Reads the SLIC microphone and writes the earpiece through the kernel char
 * device /dev/xr500v-voice (16-bit signed linear, 8 kHz, mono -- host-native
 * byte order, which on the MIPS BE SoC matches the device's big-endian PCM).
 *
 * The char device is full-duplex behind a single fd: opening it runs the SLIC
 * line-up + starts the PCM streaming engine, closing it stops it. It carries
 * ONE capture stream and takes ONE playback stream, but baresip may hold
 * several sources and players on the line at once: a call on hold keeps its
 * source (stream_send drops what it would send), the answered waiting call
 * brings its own pair, a menu sound adds a player. So the line owns the fd
 * while any of them exists, with two threads: the capture thread reads 10 ms,
 * runs the capture chain once and hands the frame to every source; the
 * playback thread writes the sum of every player plus the call waiting tone.
 * Each is paced by its own DMA direction. (One thread doing both in lockstep
 * let every playback underrun add capture delay for the rest of the call.)
 */
#define _DEFAULT_SOURCE 1
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <re.h>
#include <re_atomic.h>
#include <rem.h>
#include <baresip.h>
#include <speex/speex_echo.h>
#include "xr500v-denoise.h"
#include "dtmf.h"

#define XR_DEV "/dev/xr500v-voice"
#define IO_FRAME 80	/* 10 ms: the driver's PCM frame and the AEC/denoise step */

static struct ausrc *ausrc;
static struct auplay *auplay;

/* The line: fd, IO threads and the sources/players they serve. g_open_lock
 * serializes opening and closing; g_lock guards the AEC and both lists, and
 * the IO threads call baresip's read/write handlers with it held, so an
 * endpoint unlinked under g_lock is never called again. */
static struct {
	int fd;
	int ref;
	thrd_t cap_thread, play_thread;
	RE_ATOMIC bool run;
	struct list srcl, playl;
	SpeexPreprocessState *denoise;
	int tap_fd;		/* debug: tee captured mic to /tmp/xr500v_captap.raw */
} io = { .fd = -1, .tap_fd = -1 };
static pthread_mutex_t g_open_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* One process/device per FXS line. Opt-in only: baseline stays unchanged. */
#define AEC_FRAME 80
static SpeexEchoState *g_aec;
static bool g_aec_active;
static int playback_percent = 100;

/* In-call DTMF. The capture thread runs the handset audio through the keypad
 * detector before AEC and noise suppression, and posts each key here;
 * baresip's main thread sends it on the call being talked on as RFC 4733
 * telephone-events (key down when the tone is confirmed, KEYCODE_REL when
 * it stops). The tone itself is muted in the voice path meanwhile, so the
 * far end does not get the digit twice. XR500V_INCALL_DTMF=0 disables it. */
static struct mqueue *g_dtmf_mq;
static struct {
	struct dtmf_detector det;
	int16_t buf[DTMF_SAMPLES];
	size_t len;
	char key;		/* key sent down, release pending */
	unsigned mute;		/* 10 ms frames still to mute */
} g_dtmf;

#include "xr500v-cw.h"

static void dtmf_mq_handler(int id, void *data, void *arg)
{
	struct call *call = talking_call();
	int err;
	(void)data;
	(void)arg;

	if (!call)
		return;
	err = call_send_digit(call, (char)id);
	if (err)
		warning("xr500v: in-call DTMF send failed (%m)\n", err);
}

struct noise_stats {
	uint64_t energy;
	int64_t sum;
	uint32_t samples;
};

static void measure_noise(const int16_t *samples, size_t count, struct noise_stats *n)
{
	size_t i;
	for (i = 0; i < count; i++) {
		int32_t x = samples[i];
		n->sum += x;
		n->energy += (uint32_t)(x * x);
	}
	n->samples += count;
}

static void report_noise(const char *stage, struct noise_stats *n)
{
	double mean, power, variance;
	if (!n->samples) return;
	mean = (double)n->sum / n->samples;
	power = (double)n->energy / n->samples;
	variance = power - mean * mean;
	info("xr500v: noise stage=%s samples=%u rms=%u ac_rms=%u dc=%d\n",
	     stage, n->samples, (unsigned)sqrt(power),
	     (unsigned)sqrt(variance > 0 ? variance : 0), (int)mean);
	memset(n, 0, sizeof(*n));
}

/* Level counters only: never record or log speech samples. */
static void measure_level(const int16_t *samples, size_t count,
			  unsigned *peak, unsigned long *near_full)
{
	size_t i;
	for (i = 0; i < count; i++) {
		unsigned magnitude = samples[i] < 0 ? -(int)samples[i] : samples[i];
		if (magnitude > *peak) *peak = magnitude;
		if (magnitude >= 32000) (*near_full)++;
	}
}

static bool aec_requested(void)
{
	const char *env = getenv("XR500V_AEC");
	return env && !strcmp(env, "1");
}

static bool denoise_requested(void)
{
	const char *env = getenv("XR500V_DENOISE");
	return env && !strcmp(env, "1");
}

static void aec_playback(const int16_t *samples, size_t count)
{
	size_t i;
	pthread_mutex_lock(&g_lock);
	if (g_aec && g_aec_active)
		for (i = 0; i + AEC_FRAME <= count; i += AEC_FRAME)
			speex_echo_playback(g_aec, samples + i);
	pthread_mutex_unlock(&g_lock);
}

static void aec_capture(int16_t *samples, size_t count)
{
	int16_t out[AEC_FRAME];
	size_t i;
	pthread_mutex_lock(&g_lock);
	if (g_aec && g_aec_active) {
		for (i = 0; i + AEC_FRAME <= count; i += AEC_FRAME) {
			speex_echo_capture(g_aec, samples + i, out);
			memcpy(samples + i, out, sizeof(out));
		}
	}
	pthread_mutex_unlock(&g_lock);
}

/*
 * Shared ring group (xr500v-voip r16): the call manager publishes the jack that
 * answered in $XR500V_DEVICE_FILE. Accept ONLY the two known device nodes, on
 * a single line and with nothing else in the file; anything unexpected falls
 * back to XR500V_DEVICE. This runs when the line opens (io.ref 0 -> 1).
 */
static bool xr_device_from_file(const char *path, char *out, size_t size)
{
	char buf[64];
	ssize_t n;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return false;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	if (buf[n - 1] == '\n')
		buf[--n] = '\0';
	if (strcmp(buf, "/dev/xr500v-voice") && strcmp(buf, "/dev/xr500v-voice1"))
		return false;
	if ((size_t)n + 1 > size)
		return false;
	memcpy(out, buf, (size_t)n + 1);
	return true;
}

static int read_full(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(fd, p + got, n - got);
		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	size_t put = 0;

	while (put < n) {
		ssize_t r = write(fd, p + put, n - put);
		if (r <= 0)
			return -1;
		put += (size_t)r;
	}
	return 0;
}

/* ------------------------------------------------------------ endpoints */

struct ausrc_st {
	struct le le;
	int16_t *sampv;
	size_t sampc, fill;
	uint64_t frames;
	ausrc_read_h *rh;
	void *arg;
	struct ausrc_prm prm;
	bool opened;
};

struct auplay_st {
	struct le le;
	int16_t *sampv;
	size_t sampc, pos;
	auplay_write_h *wh;
	void *arg;
	struct auplay_prm prm;
	bool opened;
};

/* Collect 10 ms frames into the source's ptime and hand it to baresip. */
static void src_push(struct ausrc_st *st, const int16_t *s)
{
	struct auframe af;

	memcpy(st->sampv + st->fill, s, IO_FRAME * sizeof(int16_t));
	st->fill += IO_FRAME;
	if (st->fill < st->sampc)
		return;
	st->fill = 0;
	auframe_init(&af, st->prm.fmt, st->sampv, st->sampc,
		     st->prm.srate, st->prm.ch);
	af.timestamp = st->frames * AUDIO_TIMEBASE / st->prm.srate;
	st->frames += st->sampc;
	st->rh(&af, st->arg);
}

/* Add the player's next 10 ms to the mix, asking baresip for a new ptime
 * frame when the last one is used up. */
static void play_pull(struct auplay_st *st, int32_t *mix)
{
	size_t i;

	if (st->pos >= st->sampc) {
		struct auframe af;

		auframe_init(&af, st->prm.fmt, st->sampv, st->sampc,
			     st->prm.srate, st->prm.ch);
		st->wh(&af, st->arg);
		st->pos = 0;
	}
	for (i = 0; i < IO_FRAME; i++)
		mix[i] += st->sampv[st->pos + i];
	st->pos += IO_FRAME;
}

/* Feed the raw handset audio to the keypad detector in 20 ms windows and
 * post key down / key up edges to the main thread. */
static void dtmf_scan(const int16_t *s, size_t n)
{
	size_t i = 0;

	while (i < n) {
		size_t k = DTMF_SAMPLES - g_dtmf.len;
		char c;

		if (k > n - i)
			k = n - i;
		memcpy(g_dtmf.buf + g_dtmf.len, s + i, k * sizeof(int16_t));
		g_dtmf.len += k;
		i += k;
		if (g_dtmf.len < DTMF_SAMPLES)
			break;
		g_dtmf.len = 0;
		c = dtmf_feed(&g_dtmf.det, g_dtmf.buf);
		if (c) {
			g_dtmf.key = c;
			mqueue_push(g_dtmf_mq, c, NULL);
		}
		else if (g_dtmf.key && !g_dtmf.det.held) {
			mqueue_push(g_dtmf_mq, KEYCODE_REL, NULL);
			g_dtmf.key = 0;
		}
	}
	if (g_dtmf.det.candidate || g_dtmf.det.held || g_dtmf.key)
		g_dtmf.mute = 4;	/* this frame and the 30 ms after it */
}

static int16_t cw_tone(unsigned *pos, double *phase)
{
	unsigned t = *pos;
	bool on = t < 1600 || (t >= 3200 && t < 4800);

	*pos = (t + 1) % CW_PERIOD;
	*phase += 2 * M_PI * 425 / 8000;
	if (*phase > 2 * M_PI)
		*phase -= 2 * M_PI;
	return on ? (int16_t)(CW_LEVEL * sin(*phase)) : 0;
}

/* Capture: 10 ms from the line, the capture chain once, every source. */
static int cap_thread(void *arg)
{
	int16_t cap[IO_FRAME];
	uint64_t frames = 0;
	unsigned mic_peak = 0, clean_peak = 0;
	unsigned long mic_full = 0, clean_full = 0;
	struct noise_stats mic_noise = {0}, clean_noise = {0}, denoise_noise = {0};
	(void)arg;

	while (re_atomic_rlx(&io.run)) {
		struct le *le;

		if (read_full(io.fd, cap, sizeof(cap))) {
			if (re_atomic_rlx(&io.run))
				sys_msleep(5);
			continue;
		}

		if (g_dtmf_mq)
			dtmf_scan(cap, IO_FRAME);
		measure_level(cap, IO_FRAME, &mic_peak, &mic_full);
		measure_noise(cap, IO_FRAME, &mic_noise);
		aec_capture(cap, IO_FRAME);
		measure_level(cap, IO_FRAME, &clean_peak, &clean_full);
		measure_noise(cap, IO_FRAME, &clean_noise);
		if (io.denoise) {
			speex_preprocess_run(io.denoise, cap);
			measure_noise(cap, IO_FRAME, &denoise_noise);
		}
		if (io.tap_fd >= 0)
			(void)write(io.tap_fd, cap, sizeof(cap));
		if (g_dtmf.mute) {
			memset(cap, 0, sizeof(cap));
			g_dtmf.mute--;
		}

		pthread_mutex_lock(&g_lock);
		for (le = list_head(&io.srcl); le; le = le->next)
			src_push(le->data, cap);
		pthread_mutex_unlock(&g_lock);

		frames += IO_FRAME;
		if (frames >= 8000 * 5) {
			info("xr500v: capture levels mic_peak=%u aec_peak=%u mic_near_full=%lu aec_near_full=%lu\n",
			     mic_peak, clean_peak, mic_full, clean_full);
			report_noise("mic", &mic_noise);
			report_noise("aec", &clean_noise);
			if (io.denoise)
				report_noise("denoise", &denoise_noise);
			mic_peak = clean_peak = 0;
			mic_full = clean_full = 0;
			frames = 0;
		}
	}
	return 0;
}

/* Playback: the players' sum, scaled, plus the call waiting tone. The write
 * blocks while the driver's FIFO is full, which paces this thread. */
static int play_thread(void *arg)
{
	int16_t out[IO_FRAME];
	int32_t mix[IO_FRAME];
	uint64_t frames = 0;
	unsigned in_peak = 0, out_peak = 0;
	unsigned long in_full = 0, out_full = 0;
	struct noise_stats speaker_noise = {0};
	unsigned cw_pos = 0;
	double cw_phase = 0;
	(void)arg;

	while (re_atomic_rlx(&io.run)) {
		struct le *le;
		size_t i;

		memset(mix, 0, sizeof(mix));
		pthread_mutex_lock(&g_lock);
		for (le = list_head(&io.playl); le; le = le->next)
			play_pull(le->data, mix);
		pthread_mutex_unlock(&g_lock);

		for (i = 0; i < IO_FRAME; i++) {
			int32_t v = mix[i] < -32768 ? -32768 : mix[i] > 32767 ? 32767 : mix[i];

			out[i] = (int16_t)v;
		}
		measure_level(out, IO_FRAME, &in_peak, &in_full);
		if (re_atomic_rlx(&g_cw)) {
			for (i = 0; i < IO_FRAME; i++) {
				int32_t v = (int32_t)out[i] * playback_percent / 100 +
					    cw_tone(&cw_pos, &cw_phase);

				out[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
			}
		}
		else {
			cw_pos = 0;	/* the next waiting call starts with a beep */
			if (playback_percent != 100)
				for (i = 0; i < IO_FRAME; i++)
					out[i] = (int32_t)out[i] * playback_percent / 100;
		}
		measure_level(out, IO_FRAME, &out_peak, &out_full);
		measure_noise(out, IO_FRAME, &speaker_noise);
		/* Reference is the outgoing PCM queued below, not received RTP.
		 * The echo filter must cover kernel FIFO + DMA playback latency. */
		aec_playback(out, IO_FRAME);
		if (write_full(io.fd, out, sizeof(out)) && re_atomic_rlx(&io.run))
			sys_msleep(5);

		frames += IO_FRAME;
		if (frames >= 8000 * 5) {
			info("xr500v: playback levels percent=%d input_peak=%u output_peak=%u input_near_full=%lu output_near_full=%lu\n",
			     playback_percent, in_peak, out_peak, in_full, out_full);
			report_noise("speaker", &speaker_noise);
			in_peak = out_peak = 0;
			in_full = out_full = 0;
			frames = 0;
		}
	}
	return 0;
}

static void io_close(void)
{
	if (io.fd >= 0)
		close(io.fd);
	io.fd = -1;
	pthread_mutex_lock(&g_lock);
	if (g_aec)
		speex_echo_state_destroy(g_aec);
	g_aec = NULL;
	g_aec_active = false;
	pthread_mutex_unlock(&g_lock);
	if (io.denoise)
		speex_preprocess_state_destroy(io.denoise);
	io.denoise = NULL;
	if (io.tap_fd >= 0)
		close(io.tap_fd);
	io.tap_fd = -1;
}

/* First endpoint: open the line and start the IO threads. */
static int io_get(void)
{
	const char *device = getenv("XR500V_DEVICE");
	const char *selector = getenv("XR500V_DEVICE_FILE");
	char selected[64];
	int err = 0;

	pthread_mutex_lock(&g_open_lock);
	if (io.ref > 0)
		goto out;
	if (!device || !*device)
		device = XR_DEV;
	if (selector && *selector && xr_device_from_file(selector, selected, sizeof(selected)))
		device = selected;
	io.fd = open(device, O_RDWR);
	if (io.fd < 0) {
		err = errno ? errno : EIO;
		warning("xr500v: open(%s) failed (%m)\n", device, err);
		goto out;
	}
	info("xr500v: line device %s opened\n", device);
	if (aec_requested()) {
		int rate = 8000;

		pthread_mutex_lock(&g_lock);
		g_aec = speex_echo_state_init(AEC_FRAME, 3200);
		if (g_aec)
			speex_echo_ctl(g_aec, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
		g_aec_active = false;
		pthread_mutex_unlock(&g_lock);
		if (!g_aec) {
			warning("xr500v: AEC allocation failed\n");
			err = ENOMEM;
			goto fail;
		}
		info("xr500v: experimental fixed-point AEC enabled, 10ms frames/400ms tail\n");
	}
	if (denoise_requested()) {
		io.denoise = xr500v_denoise_create();
		if (!io.denoise) {
			err = ENOMEM;
			goto fail;
		}
	}
	if (getenv("XR500V_CAPTAP"))
		io.tap_fd = open("/tmp/xr500v_captap.raw", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	memset(&g_dtmf, 0, sizeof(g_dtmf));
	re_atomic_rlx_set(&io.run, true);
	err = thread_create_name(&io.cap_thread, "xr500v_cap", cap_thread, NULL);
	if (err) {
		re_atomic_rlx_set(&io.run, false);
		goto fail;
	}
	err = thread_create_name(&io.play_thread, "xr500v_play", play_thread, NULL);
	if (err) {
		re_atomic_rlx_set(&io.run, false);
		thrd_join(io.cap_thread, NULL);
		goto fail;
	}
	goto out;
 fail:
	io_close();
 out:
	if (!err)
		io.ref++;
	pthread_mutex_unlock(&g_open_lock);
	return err;
}

/* Last endpoint gone (already unlinked): stop the threads, close the line. */
static void io_put(void)
{
	pthread_mutex_lock(&g_open_lock);
	if (io.ref > 0 && --io.ref == 0) {
		re_atomic_rlx_set(&io.run, false);
		thrd_join(io.cap_thread, NULL);
		thrd_join(io.play_thread, NULL);
		io_close();
	}
	pthread_mutex_unlock(&g_open_lock);
}

/* The mixer and the 10 ms step assume the line's own format. */
static bool line_format(enum aufmt fmt, uint32_t srate, uint8_t ch, uint32_t ptime)
{
	return fmt == AUFMT_S16LE && srate == 8000 && ch == 1 && ptime && !(ptime % 10);
}

/* ----------------------------------------------------------------- ausrc */

static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	if (st->opened) {
		pthread_mutex_lock(&g_lock);
		list_unlink(&st->le);
		g_aec_active = !list_isempty(&io.srcl);
		pthread_mutex_unlock(&g_lock);
		io_put();
	}
	mem_deref(st->sampv);
}

static int xr_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
			struct ausrc_prm *prm, const char *device,
			ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err;
	(void)device;
	(void)errh;

	if (!stp || !as || !prm || !rh)
		return EINVAL;
	if (!line_format(prm->fmt, prm->srate, prm->ch, prm->ptime))
		return ENOTSUP;

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->prm  = *prm;
	st->rh   = rh;
	st->arg  = arg;
	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;
	st->sampv = mem_alloc(st->sampc * sizeof(int16_t), NULL);
	if (!st->sampv) {
		err = ENOMEM;
		goto out;
	}

	err = io_get();
	if (err)
		goto out;
	st->opened = true;
	pthread_mutex_lock(&g_lock);
	if (g_aec) {
		speex_echo_state_reset(g_aec);
		g_aec_active = true;
	}
	list_append(&io.srcl, &st->le, st);
	pthread_mutex_unlock(&g_lock);

	info("xr500v: recording started (%u Hz, %u ch, ptime=%u, denoise=%s)\n",
	     prm->srate, prm->ch, prm->ptime, io.denoise ? "mild-6dB" : "off");
 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;
	return err;
}

/* ---------------------------------------------------------------- auplay */

static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	if (st->opened) {
		pthread_mutex_lock(&g_lock);
		list_unlink(&st->le);
		pthread_mutex_unlock(&g_lock);
		io_put();
	}
	mem_deref(st->sampv);
}

static int xr_play_alloc(struct auplay_st **stp, const struct auplay *ap,
			 struct auplay_prm *prm, const char *device,
			 auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int err;
	(void)device;

	if (!stp || !ap || !prm || !wh)
		return EINVAL;
	if (!line_format(prm->fmt, prm->srate, prm->ch, prm->ptime))
		return ENOTSUP;

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->prm  = *prm;
	st->wh   = wh;
	st->arg  = arg;
	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;
	st->pos = st->sampc;	/* ask baresip for the first frame */
	st->sampv = mem_zalloc(st->sampc * sizeof(int16_t), NULL);
	if (!st->sampv) {
		err = ENOMEM;
		goto out;
	}

	err = io_get();
	if (err)
		goto out;
	st->opened = true;
	pthread_mutex_lock(&g_lock);
	list_append(&io.playl, &st->le, st);
	pthread_mutex_unlock(&g_lock);

	info("xr500v: playback started (%u Hz, %u ch, ptime=%u)\n",
	     prm->srate, prm->ch, prm->ptime);
 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;
	return err;
}

/* ---------------------------------------------------------------- module */

static int module_init(void)
{
	int err;
	const char *value = getenv("XR500V_PLAYBACK_PERCENT");

	if (value) {
		char *end;
		long percent = strtol(value, &end, 10);
		if (!*value || *end || percent < 25 || percent > 100)
			return EINVAL;
		playback_percent = percent;
	}
	info("xr500v: playback scale=%d%%, experimental AEC=%s\n",
	     playback_percent, aec_requested() ? "requested" : "off");

	value = getenv("XR500V_INCALL_DTMF");
	if (!value || strcmp(value, "0")) {
		err = mqueue_alloc(&g_dtmf_mq, dtmf_mq_handler, NULL);
		if (err)
			return err;
	}
	info("xr500v: in-call DTMF (RFC 4733) %s\n", g_dtmf_mq ? "on" : "off");

	value = getenv("XR500V_CALL_WAITING");
	g_cw_enabled = value && !strcmp(value, "1");
	err = bevent_register(call_event_handler, NULL);
	if (err)
		return err;
	info("xr500v: call waiting tone %s\n", g_cw_enabled ? "on" : "off");

	err  = ausrc_register(&ausrc, baresip_ausrcl(), "xr500v", xr_src_alloc);
	err |= auplay_register(&auplay, baresip_auplayl(), "xr500v",
			       xr_play_alloc);
	return err;
}

static int module_close(void)
{
	bevent_unregister(call_event_handler);
	ausrc  = mem_deref(ausrc);
	auplay = mem_deref(auplay);
	g_dtmf_mq = mem_deref(g_dtmf_mq);
	return 0;
}

const struct mod_export DECL_EXPORTS(xr500v) = {
	"xr500v",
	"sound",
	module_init,
	module_close
};
