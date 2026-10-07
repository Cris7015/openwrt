/* SPDX-License-Identifier: GPL-2.0-or-later
 * Caller ID in the pause after the first ring burst (on-hook data after
 * ringing, ETSI EN 300 659-1 / Telcordia GR-30): channel seizure, marks, then
 * one MDMF "call set-up" message with the date and time, the calling number
 * (or why it is absent) and the caller's name when the network gives one.
 * 1200 baud FSK with V.23 tones (ETSI: Argentina, Spain, most of Europe) or
 * Bell 202. The number is never logged, only which kind of message was sent.
 *
 * EXPERIMENTAL, off by default: the signal reaches the line (on-hook echo
 * probe, 2026-09-26) but the one phone tested, a Philips CD140 that shows
 * caller ID from the old copper line, decoded none of the variants tried
 * (V.23/Bell 202 MDMF/SDMF, DTMF A/D, before/after the first burst, with a
 * ring pulse alert, three levels). The cause is not known yet.
 *
 * A child per ringing jack opens the line's PCM device, plays the message
 * and then keeps the stream open with silence until the call is answered or
 * goes away. Closing it on an on-hook line would park the SLIC in standby
 * (slic_dual_release) in the middle of the ring pause, and a standby/active
 * step is what makes the old DECT base ring on its own. The stream is closed
 * (cid_stop) before baresip opens the same device on answer.
 */
#include <stdint.h>
#include <strings.h>

#define CID_DELAY_MS	600	/* end of the first burst -> first sample */
#define CID_GUARD_MS	200	/* the data ends at least this long before the next burst */
#define CID_LEAD_MS	60	/* silence while the stream starts */
#define CID_LEVEL	6000	/* peak, s16: about -12 dBm0 */

enum { CID_OFF, CID_V23, CID_BELL202 };
static int cid_fsk = CID_OFF;
static int16_t cid_wave[12000];		/* 1.5 s at 8 kHz, a full message is < 1.3 s */
static size_t cid_samples;
static const char *cid_kind = "";
static int cid_pending;
static pid_t cid_pid[2] = { -1, -1 };

static void cid_stop(void)
{
	int j;

	cid_pending = 0;
	for (j = 0; j < 2; j++) {
		if (cid_pid[j] <= 0)
			continue;
		kill(cid_pid[j], SIGTERM);
		while (waitpid(cid_pid[j], NULL, 0) < 0 && errno == EINTR) {}
		cid_pid[j] = -1;
	}
}

static int cid_mode(const char *env)
{
	if (env && !strcmp(env, "v23")) return CID_V23;
	if (env && !strcmp(env, "bell202")) return CID_BELL202;
	return CID_OFF;
}

/* MDMF message: type 0x80, length, parameters, checksum (two's complement of
 * the byte sum). Parameters: 0x01 date/time MMDDHHMM, 0x02 number or 0x04
 * reason for its absence ('P' private, 'O' unavailable), 0x07 name or 0x08
 * reason for its absence. Returns the message length. */
static size_t cid_message(uint8_t *m, const char *peeruri, const char *name, const char **kind)
{
	char number[21] = "", disp[21] = "", when[9];
	const char *user = NULL;
	size_t n = 0, len, i, at = 2;
	int anonymous = 0;
	unsigned sum = 0;
	struct tm tm;
	time_t now = time(NULL);
	FILE *f;

	/* The service may start without TZ; the system zone lives in /etc/TZ. */
	if (!getenv("TZ") && (f = fopen("/etc/TZ", "r"))) {
		char tz[64];

		if (fgets(tz, sizeof(tz), f)) {
			tz[strcspn(tz, "\r\n")] = 0;
			if (*tz) { setenv("TZ", tz, 1); tzset(); }
		}
		fclose(f);
	}
	localtime_r(&now, &tm);
	snprintf(when, sizeof(when), "%02u%02u%02u%02u",
		 (unsigned)(tm.tm_mon + 1) % 100, (unsigned)tm.tm_mday % 100,
		 (unsigned)tm.tm_hour % 100, (unsigned)tm.tm_min % 100);

	if (peeruri) {
		if (!strncasecmp(peeruri, "sip:", 4) || !strncasecmp(peeruri, "tel:", 4)) user = peeruri + 4;
		else if (!strncasecmp(peeruri, "sips:", 5)) user = peeruri + 5;
	}
	if (user) {
		size_t ulen = strcspn(user, "@;?>");
		const char *p = user;

		const char *host = strchr(user, '@');

		anonymous = (ulen == 9 && !strncasecmp(user, "anonymous", 9)) ||
			    (host && !strncasecmp(host + 1, "anonymous.invalid", 17));
		if (ulen == 6 && !strncmp(user, "phone", 5) && (user[5] == '1' || user[5] == '2')) {
			number[n++] = user[5];		/* local extension: what you dial back */
		} else if (ulen && strspn(user, "+0123456789-.()") >= ulen) {
			/* E.164 from Argentina to the national form: +54 -> 0, and
			 * the mobile 9 after it dropped (0 + area + number). */
			if (!strncmp(p, "+549", 4)) { number[n++] = '0'; p += 4; }
			else if (!strncmp(p, "+54", 3)) { number[n++] = '0'; p += 3; }
			for (; p < user + ulen && n < sizeof(number) - 1; p++)
				if (*p >= '0' && *p <= '9') number[n++] = *p;
		}
		number[n] = 0;
	}
	if (name && strcasecmp(name, "anonymous")) {
		for (i = 0; name[i] && i < sizeof(disp) - 1; i++)
			disp[i] = (name[i] >= 32 && name[i] < 127) ? name[i] : '?';
		disp[i] = 0;
	} else if (name) {
		anonymous = 1;
	}

	m[0] = 0x80;
	m[at++] = 0x01; m[at++] = 8; memcpy(m + at, when, 8); at += 8;
	if (*number) {
		len = strlen(number);
		m[at++] = 0x02; m[at++] = (uint8_t)len; memcpy(m + at, number, len); at += len;
		*kind = "number";
	} else {
		m[at++] = 0x04; m[at++] = 1; m[at++] = anonymous ? 'P' : 'O';
		*kind = anonymous ? "private" : "unavailable";
	}
	if (*disp) {
		len = strlen(disp);
		m[at++] = 0x07; m[at++] = (uint8_t)len; memcpy(m + at, disp, len); at += len;
	} else if (anonymous) {
		m[at++] = 0x08; m[at++] = 1; m[at++] = 'P';
	}
	m[1] = (uint8_t)(at - 2);
	for (i = 0; i < at; i++) sum += m[i];
	m[at++] = (uint8_t)(0x100 - (sum & 0xff));
	return at;
}

/* Continuous-phase FSK: 300 alternating seizure bits, 180 marks, the bytes
 * framed 0 + 8 data LSB first + 1, then 50 ms of marks. */
static size_t cid_modulate(int16_t *w, size_t max, const uint8_t *m, size_t len, int mode)
{
	const double mark = mode == CID_BELL202 ? 1200 : 1300;
	const double space = mode == CID_BELL202 ? 2200 : 2100;
	const size_t total = 300 + 180 + len * 10 + 60;
	double phase = 0;
	size_t n = 0, b;

	for (; n < 8 * CID_LEAD_MS && n < max; n++) w[n] = 0;
	for (b = 0; b < total; b++) {
		size_t end = (size_t)((b + 1) * 8000.0 / 1200 + 8 * CID_LEAD_MS + 0.5);
		int bit;

		if (b < 300) bit = b & 1;
		else if (b < 480) bit = 1;
		else if (b < 480 + len * 10) {
			size_t k = (b - 480) % 10;

			bit = k == 0 ? 0 : k == 9 ? 1 : (m[(b - 480) / 10] >> (k - 1)) & 1;
		} else bit = 1;
		for (; n < end && n < max; n++) {
			phase += 2 * 3.141592653589793 * (bit ? mark : space) / 8000;
			if (phase > 2 * 3.141592653589793) phase -= 2 * 3.141592653589793;
			w[n] = (int16_t)(CID_LEVEL * sin(phase));
		}
	}
	return n;
}

/* Build the waveform for this call now; it is played after the first burst. */
static void cid_prepare(const char *peeruri, const char *name)
{
	uint8_t m[80];
	size_t len;

	cid_stop();
	if (cid_fsk == CID_OFF)
		return;
	len = cid_message(m, peeruri, name, &cid_kind);
	cid_samples = cid_modulate(cid_wave, sizeof(cid_wave) / sizeof(cid_wave[0]), m, len, cid_fsk);
	cid_pending = 1;
}

static void cid_worker(const char *device, int ctrl, int diagnostic)
{
	int16_t frame[DTMF_SAMPLES], in[DTMF_SAMPLES];
	size_t pos = 0;
	pid_t parent = getppid();
	int fd;

	signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
	prctl(PR_SET_PDEATHSIG, SIGTERM);
	if (getppid() != parent || parent == 1) _exit(1);
	close(ctrl); if (diagnostic >= 0) close(diagnostic);
	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) _exit(1);
	for (;;) {
		size_t k = cid_samples - pos < DTMF_SAMPLES ? cid_samples - pos : DTMF_SAMPLES;

		memset(frame, 0, sizeof(frame));
		memcpy(frame, cid_wave + pos, k * sizeof(int16_t));
		pos += k;
		if (frame_io(fd, in, sizeof(in), 0)) break;
		if (frame_io(fd, frame, sizeof(frame), 1)) break;
	}
	close(fd); _exit(0);
}

static void cid_start(int ctrl, int diagnostic)
{
	const char *dev[2] = { NULL, NULL };
	sigset_t block, old_mask;
	int j;

	if (shared_mode) { dev[0] = jacks[0].device; dev[1] = jacks[1].device; }
	else if (*keypad_device) dev[0] = keypad_device;
	else return;
	sigemptyset(&block); sigaddset(&block, SIGTERM); sigaddset(&block, SIGINT);
	if (sigprocmask(SIG_BLOCK, &block, &old_mask)) return;
	for (j = 0; j < 2; j++) {
		if (!dev[j]) continue;
		cid_pid[j] = fork();
		if (!cid_pid[j]) {
			signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
			sigprocmask(SIG_SETMASK, &old_mask, NULL);
			cid_worker(dev[j], ctrl, diagnostic);
		}
	}
	sigprocmask(SIG_SETMASK, &old_mask, NULL);
	fprintf(stderr, "[callmgr] caller ID %s (%s) after the first burst\n",
		cid_kind, cid_fsk == CID_BELL202 ? "Bell 202" : "V.23");
	fflush(stderr);
}

/* Called every loop pass: start the data CID_DELAY_MS after the first burst,
 * if it still fits in the pause. */
static void cid_tick(int ctrl, int diagnostic)
{
	long long now, first_off, next_on;

	if (!cid_pending || state != RINGING || !ring_started_ms)
		return;
	now = cadence_now_ms();
	first_off = ring_started_ms + ring_on_ms;
	if (now < first_off + CID_DELAY_MS)
		return;
	cid_pending = 0;
	next_on = first_off + ring_off_ms;
	if (now + (long long)cid_samples / 8 + CID_GUARD_MS > next_on) {
		logmsg("caller ID skipped: no room left in the ring pause");
		return;
	}
	cid_start(ctrl, diagnostic);
}
