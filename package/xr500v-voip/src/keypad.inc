/* SPDX-License-Identifier: GPL-2.0-or-later
 * The char device ignores O_NONBLOCK: keep blocking PCM IO in a child, not
 * the hook/ring event loop. Stop/reap it BEFORE baresip opens the same line.
 */
#include <signal.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include "dtmf.h"
static pid_t keypad_pid = -1;
static int keypad_fd = -1;
static char keypad_device[64], dialing_path[128];
static char dial_kind[16], dial_domain[254], dial_digits[33];
static unsigned digit_count;
static long long dial_started, digit_at;
static volatile sig_atomic_t stopping;

/* What the line plays while the handset is up and no call holds it. With a
 * provider account the tone at off-hook tells whether a call can go out, as
 * the Movistar AR HGU does: dial tone when registered, the special
 * information tone (ITU-T E.180, 950/1400/1800 Hz) while registering, busy
 * when the registration failed. */
enum { TONE_DIAL, TONE_SIT, TONE_BUSY };
#define DIAL_TONE_MS	20000	/* HGU: 20 s of dial tone without a digit, then busy */
#define OFFHOOK_BUSY_MS	60000	/* busy, then silence until the handset goes down */
#define TONE_LEVEL	1200
static const struct tone_step { short hz, ms; }
	busy_cadence[] = { { 425, 300 }, { 0, 200 } },	/* not measured on the HGU */
	sit_cadence[] = { { 950, 330 }, { 1400, 330 }, { 1800, 330 }, { 0, 1000 } };
static int keypad_tone = TONE_DIAL;
static long tone_limit_ms;		/* 0: play until stopped */
static int worker_diag_fd = -1;		/* the diagnostic FIFO, closed in workers */
static int reg_dial_tone(void);

/* Sample t (8 kHz, counted from the start of the tone) of a tone kind. */
static int16_t tone_at(int kind, unsigned long long t)
{
	const struct tone_step *c = kind == TONE_SIT ? sit_cadence : busy_cadence;
	unsigned n = kind == TONE_SIT ? sizeof(sit_cadence) / sizeof(*c) :
					sizeof(busy_cadence) / sizeof(*c);
	unsigned i, period = 0;
	unsigned long long ms = t / 8;
	int hz;

	if (kind == TONE_DIAL) {
		hz = 425;
	} else {
		for (i = 0; i < n; i++)
			period += c[i].ms;
		ms %= period;
		for (i = 0; ms >= (unsigned)c[i].ms; i++)
			ms -= c[i].ms;
		hz = c[i].hz;
	}
	return hz ? (int16_t)(TONE_LEVEL * sin(2 * 3.141592653589793 * hz * t / 8000.0)) : 0;
}

static long long clock_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void stop_signal(int sig) { (void)sig; stopping = 1; }

static void keypad_stop(void)
{
	if (keypad_fd >= 0) close(keypad_fd);
	keypad_fd = -1;
	if (keypad_pid > 0) {
		kill(keypad_pid, SIGTERM);
		while (waitpid(keypad_pid, NULL, 0) < 0 && errno == EINTR) {}
	}
	keypad_pid = -1;
}

static int frame_io(int fd, void *buf, size_t size, int playback)
{
	size_t got = 0;
	while (got < size) {
		ssize_t n = playback ? write(fd, (char *)buf + got, size - got) : read(fd, (char *)buf + got, size - got);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		got += n;
	}
	return 0;
}

/* output < 0: a tone-only worker, nothing is collected. */
static void keypad_worker(int output, int ctrl, int diagnostic)
{
	struct dtmf_detector d = {0};
	int16_t samples[DTMF_SAMPLES], tone[DTMF_SAMPLES];
	int fd, i, quiet = 0;
	unsigned long long t = 0, limit = (unsigned long long)tone_limit_ms * 8;
	pid_t parent = getppid();
	signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
	prctl(PR_SET_PDEATHSIG, SIGTERM);
	if (getppid() != parent || parent == 1) _exit(1);
	/* ctrl_tcp takes one client: a copy left here would keep a dropped
	 * connection open and lock the manager out after it reconnects. */
	if (ctrl >= 0) close(ctrl);
	if (diagnostic >= 0) close(diagnostic);
	/* Right after a call baresip may still be releasing the device. */
	for (i = 0; (fd = open(keypad_device, O_RDWR | O_CLOEXEC)) < 0 && i < 20; i++)
		usleep(50000);
	if (fd < 0) _exit(1);
	for (;;) {
		char digit;
		if (frame_io(fd, samples, sizeof(samples), 0)) break;
		if (output >= 0 && (digit = dtmf_feed(&d, samples))) {
			quiet = 1;
			if (write(output, &digit, 1) != 1) break;
		}
		/* Conservative local tones; preserve existing call audio/AEC. Past
		 * the limit the device stays open and silent: closing it parks the
		 * SLIC, which can read as a false on-hook. */
		for (i = 0; i < DTMF_SAMPLES; i++, t++)
			tone[i] = quiet || (limit && t >= limit) ? 0 : tone_at(keypad_tone, t);
		if (frame_io(fd, tone, sizeof(tone), 1)) break;
	}
	close(fd); if (output >= 0) close(output); _exit(0);
}

/* Fork a worker on keypad_device: a collector when pipefd is given. */
static int worker_spawn(int *pipefd, int ctrl, int diagnostic)
{
	sigset_t block, old_mask;

	// A new child must not inherit the manager's non-exiting TERM handler
	// if cancellation races its first instruction after fork.
	sigemptyset(&block); sigaddset(&block, SIGTERM); sigaddset(&block, SIGINT);
	if (sigprocmask(SIG_BLOCK, &block, &old_mask)) return -1;
	keypad_pid = fork();
	if (!keypad_pid) {
		signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
		sigprocmask(SIG_SETMASK, &old_mask, NULL);
		if (pipefd) close(pipefd[0]);
		keypad_worker(pipefd ? pipefd[1] : -1, ctrl, diagnostic);
	}
	sigprocmask(SIG_SETMASK, &old_mask, NULL);
	return keypad_pid < 0 ? -1 : 0;
}

/* A line tone with no digit collection, e.g. busy after the far end hung up.
 * Returns 1 when playing, 0 when the line has no keypad device. */
static int tone_start(int kind, long limit_ms)
{
	if (!isolated_ec || !*keypad_device) return 0;
	keypad_stop();
	keypad_tone = kind;
	tone_limit_ms = limit_ms;
	return worker_spawn(NULL, ctrl_s, worker_diag_fd) ? -1 : 1;
}

/* Why a collection must end now, if it must: no digit for DIAL_TONE_MS, or a
 * number still incomplete after 30 s. */
static const char *keypad_expired(long long now)
{
	if (!digit_count && now - dial_started >= DIAL_TONE_MS) return "dial_tone_timeout";
	if (now - dial_started >= 30000) return "collection_timeout";
	return NULL;
}

static int keypad_start(int ctrl, int diagnostic)
{
	FILE *f;
	char mode[16];
	int pipefd[2];
	if (!isolated_ec || !*dialing_path) return 0;
	f = fopen(dialing_path, "r");
	if (!f) return 0;
	if (!fgets(mode, sizeof(mode), f) || !fgets(dial_kind, sizeof(dial_kind), f) || !fgets(dial_domain, sizeof(dial_domain), f)) { fclose(f); return 0; }
	fclose(f);
	mode[strcspn(mode, "\r\n")] = 0;
	dial_kind[strcspn(dial_kind, "\r\n")] = 0;
	dial_domain[strcspn(dial_domain, "\r\n")] = 0;
	if (strcmp(mode, "keypad")) return 0;
	if ((strcmp(dial_kind, "local") && strcmp(dial_kind, "provider")) || !*dial_domain ||
	    strspn(dial_domain, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:") != strlen(dial_domain)) return -1;
	keypad_stop();
	keypad_tone = strcmp(dial_kind, "provider") ? TONE_DIAL : reg_dial_tone();
	tone_limit_ms = 0;
	if (pipe(pipefd)) return -1;
	if (worker_spawn(pipefd, ctrl, diagnostic)) { close(pipefd[0]); close(pipefd[1]); return -1; }
	close(pipefd[1]);
	keypad_fd = pipefd[0];
	fcntl(keypad_fd, F_SETFL, O_NONBLOCK);
	digit_count = 0; dial_digits[0] = 0; dial_started = clock_ms(); digit_at = 0;
	return 1;
}

static const char *keypad_destination_error(void)
{
	if (!digit_count || !strpbrk(dial_digits, "0123456789")) return "no_digits";
	if (!strcmp(dial_kind, "local")) {
		// A shared ring group is ONE line: the 1#/2# intercom no longer
		// exists, there is no second account to reach.
		if (shared_mode) return "shared_no_local_dialplan";
		if (digit_count != 1 || (dial_digits[0] != '1' && dial_digits[0] != '2')) return "invalid_local_extension";
		// Do not call the account owned by this same physical port.
		if (dial_digits[0] - '0' == 3 - isolated_ec) return "same_port";
	}
	return NULL;
}

static int keypad_uri(char *uri, size_t size)
{
	if (keypad_destination_error()) return -1;
	if (!strcmp(dial_kind, "local")) {
		return snprintf(uri, size, "sip:phone%c@%s:%d", dial_digits[0], dial_domain, 5058 + 2 * (dial_digits[0] - '0')) >= (int)size ? -1 : 0;
	}
	{
		/* '#' is not allowed as is in a SIP URI user part (*43# -> *43%23) */
		char user[3 * sizeof(dial_digits)];
		size_t i, n = 0;

		for (i = 0; dial_digits[i]; i++) {
			if (dial_digits[i] == '#') { memcpy(user + n, "%23", 3); n += 3; }
			else user[n++] = dial_digits[i];
		}
		user[n] = 0;
		return snprintf(uri, size, "sip:%s@%s", user, dial_domain) >= (int)size ? -1 : 0;
	}
}
