/**
 * @file xr500v.c  baresip audio driver for the TP-Link Archer XR500v FXS line
 *
 * Reads the SLIC microphone and writes the earpiece through the kernel char
 * device /dev/xr500v-voice (16-bit signed linear, 8 kHz, mono -- host-native
 * byte order, which on the MIPS BE SoC matches the device's big-endian PCM).
 *
 * The char device is full-duplex behind a single fd: opening it runs the SLIC
 * line-up + starts the PCM streaming engine, closing it stops it. baresip
 * allocates the ausrc and auplay independently, so the fd is opened once and
 * reference-counted, shared by both the record and playback threads.
 */
#define _DEFAULT_SOURCE 1
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <re.h>
#include <re_atomic.h>
#include <rem.h>
#include <baresip.h>

#define XR_DEV "/dev/xr500v-voice"

static struct ausrc *ausrc;
static struct auplay *auplay;

/* shared full-duplex fd, reference counted across ausrc + auplay */
static int g_fd = -1;
static int g_ref;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int xr_get_fd(void)
{
	int fd = -1;

	pthread_mutex_lock(&g_lock);
	if (g_ref == 0) {
		g_fd = open(XR_DEV, O_RDWR);
		if (g_fd < 0) {
			warning("xr500v: open(%s) failed (%m)\n", XR_DEV, errno);
			goto out;
		}
	}
	g_ref++;
	fd = g_fd;
 out:
	pthread_mutex_unlock(&g_lock);
	return fd;
}

static void xr_put_fd(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_ref > 0 && --g_ref == 0) {
		if (g_fd >= 0)
			close(g_fd);
		g_fd = -1;
	}
	pthread_mutex_unlock(&g_lock);
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

/* ----------------------------------------------------------------- ausrc */

struct ausrc_st {
	thrd_t thread;
	RE_ATOMIC bool run;
	int fd;
	int tap_fd;		/* debug: tee captured mic to /tmp/xr500v_captap.raw */
	int16_t *sampv;
	size_t sampc;
	ausrc_read_h *rh;
	void *arg;
	struct ausrc_prm prm;
};

static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	if (re_atomic_rlx(&st->run)) {
		re_atomic_rlx_set(&st->run, false);
		thrd_join(st->thread, NULL);
	}
	if (st->fd >= 0)
		xr_put_fd();
	if (st->tap_fd >= 0)
		close(st->tap_fd);
	mem_deref(st->sampv);
}

static int src_thread(void *arg)
{
	struct ausrc_st *st = arg;
	const size_t bytes = st->sampc * sizeof(int16_t);
	uint64_t frames = 0;

	while (re_atomic_rlx(&st->run)) {
		struct auframe af;

		if (read_full(st->fd, st->sampv, bytes)) {
			if (re_atomic_rlx(&st->run))
				sys_msleep(5);
			continue;
		}

		if (st->tap_fd >= 0)
			(void)write(st->tap_fd, st->sampv,
				    st->sampc * sizeof(int16_t));

		auframe_init(&af, st->prm.fmt, st->sampv, st->sampc,
			     st->prm.srate, st->prm.ch);
		af.timestamp = frames * AUDIO_TIMEBASE / st->prm.srate;
		frames += st->sampc / st->prm.ch;

		st->rh(&af, st->arg);
	}
	return 0;
}

static int xr_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
			struct ausrc_prm *prm, const char *device,
			ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err = 0;
	(void)device;
	(void)errh;

	if (!stp || !as || !prm || !rh)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->prm  = *prm;
	st->rh   = rh;
	st->arg  = arg;
	st->fd   = -1;
	st->tap_fd = getenv("XR500V_CAPTAP")
		? open("/tmp/xr500v_captap.raw", O_WRONLY | O_CREAT | O_TRUNC, 0644)
		: -1;
	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;

	st->sampv = mem_alloc(st->sampc * sizeof(int16_t), NULL);
	if (!st->sampv) {
		err = ENOMEM;
		goto out;
	}

	st->fd = xr_get_fd();
	if (st->fd < 0) {
		err = EIO;
		goto out;
	}

	re_atomic_rlx_set(&st->run, true);
	err = thread_create_name(&st->thread, "xr500v_src", src_thread, st);
	if (err) {
		re_atomic_rlx_set(&st->run, false);
		goto out;
	}

	info("xr500v: recording started (%u Hz, %u ch, ptime=%u)\n",
	     prm->srate, prm->ch, prm->ptime);
 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;
	return err;
}

/* ---------------------------------------------------------------- auplay */

struct auplay_st {
	thrd_t thread;
	RE_ATOMIC bool run;
	int fd;
	int16_t *sampv;
	size_t sampc;
	auplay_write_h *wh;
	void *arg;
	struct auplay_prm prm;
};

static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	if (re_atomic_rlx(&st->run)) {
		re_atomic_rlx_set(&st->run, false);
		thrd_join(st->thread, NULL);
	}
	if (st->fd >= 0)
		xr_put_fd();
	mem_deref(st->sampv);
}

static int play_thread(void *arg)
{
	struct auplay_st *st = arg;
	const size_t bytes = st->sampc * sizeof(int16_t);
	struct auframe af;

	auframe_init(&af, st->prm.fmt, st->sampv, st->sampc,
		     st->prm.srate, st->prm.ch);

	while (re_atomic_rlx(&st->run)) {
		st->wh(&af, st->arg);

		if (write_full(st->fd, st->sampv, bytes)) {
			if (re_atomic_rlx(&st->run))
				sys_msleep(5);
		}
	}
	return 0;
}

static int xr_play_alloc(struct auplay_st **stp, const struct auplay *ap,
			 struct auplay_prm *prm, const char *device,
			 auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int err = 0;
	(void)device;

	if (!stp || !ap || !prm || !wh)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->prm  = *prm;
	st->wh   = wh;
	st->arg  = arg;
	st->fd   = -1;
	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;

	st->sampv = mem_alloc(st->sampc * sizeof(int16_t), NULL);
	if (!st->sampv) {
		err = ENOMEM;
		goto out;
	}

	st->fd = xr_get_fd();
	if (st->fd < 0) {
		err = EIO;
		goto out;
	}

	re_atomic_rlx_set(&st->run, true);
	err = thread_create_name(&st->thread, "xr500v_play", play_thread, st);
	if (err) {
		re_atomic_rlx_set(&st->run, false);
		goto out;
	}

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

	err  = ausrc_register(&ausrc, baresip_ausrcl(), "xr500v", xr_src_alloc);
	err |= auplay_register(&auplay, baresip_auplayl(), "xr500v",
			       xr_play_alloc);
	return err;
}

static int module_close(void)
{
	ausrc  = mem_deref(ausrc);
	auplay = mem_deref(auplay);
	return 0;
}

const struct mod_export DECL_EXPORTS(xr500v) = {
	"xr500v",
	"sound",
	module_init,
	module_close
};
