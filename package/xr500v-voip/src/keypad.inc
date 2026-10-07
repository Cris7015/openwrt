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

static void keypad_worker(int output, int ctrl, int diagnostic)
{
	struct dtmf_detector d = {0};
	int16_t samples[DTMF_SAMPLES], tone[DTMF_SAMPLES];
	int fd, i, quiet = 0;
	unsigned phase = 0;
	pid_t parent = getppid();
	signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
	prctl(PR_SET_PDEATHSIG, SIGTERM);
	if (getppid() != parent || parent == 1) _exit(1);
	close(ctrl); if (diagnostic >= 0) close(diagnostic);
	fd = open(keypad_device, O_RDWR | O_CLOEXEC);
	if (fd < 0) _exit(1);
	for (;;) {
		char digit;
		if (frame_io(fd, samples, sizeof(samples), 0)) break;
		digit = dtmf_feed(&d, samples);
		if (digit) {
			quiet = 1;
			if (write(output, &digit, 1) != 1) break;
		}
		// Conservative local dial tone; preserve existing call audio/AEC.
		for (i = 0; i < DTMF_SAMPLES; i++, phase = (phase + 425) % 8000)
			tone[i] = quiet ? 0 : (int16_t)(1200 * sin(2 * 3.141592653589793 * phase / 8000));
		if (frame_io(fd, tone, sizeof(tone), 1)) break;
	}
	close(fd); close(output); _exit(0);
}

static int keypad_start(int ctrl, int diagnostic)
{
	FILE *f;
	char mode[16];
	int pipefd[2];
	sigset_t block, old_mask;
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
	if (pipe(pipefd)) return -1;
	// A new child must not inherit the manager's non-exiting TERM handler
	// if cancellation races its first instruction after fork.
	sigemptyset(&block); sigaddset(&block, SIGTERM); sigaddset(&block, SIGINT);
	if (sigprocmask(SIG_BLOCK, &block, &old_mask)) { close(pipefd[0]); close(pipefd[1]); return -1; }
	keypad_pid = fork();
	if (!keypad_pid) {
		signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
		sigprocmask(SIG_SETMASK, &old_mask, NULL);
		close(pipefd[0]); keypad_worker(pipefd[1], ctrl, diagnostic);
	}
	sigprocmask(SIG_SETMASK, &old_mask, NULL);
	close(pipefd[1]);
	if (keypad_pid < 0) { close(pipefd[0]); return -1; }
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
