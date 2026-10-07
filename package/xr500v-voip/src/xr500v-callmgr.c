/*
 * xr500v-callmgr.c -- real call flow for the XR500v FXS, tying the SLIC hook to
 * baresip's ctrl_tcp so the Philips DECT phone behaves like a real telephone:
 *
 *   incoming SIP call            -> ring the handset (SLIC ring, debugfs)
 *   lift handset while ringing   -> accept the call (off-hook)
 *   hang up during a call        -> hangup (line goes idle, no leftover beep)
 *   lift handset while idle      -> speed-dial the URI in /root/voip-speeddial
 *
 * Polls the hook debugfs and speaks baresip's ctrl_tcp netstring/JSON protocol
 * on 127.0.0.1:4444. A single small musl binary (the device only has busybox).
 *
 * Build (OpenWrt toolchain): mips-openwrt-linux-musl-gcc -O2 callmgr.c -o callmgr
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <errno.h>
#include <json-c/json.h>

static char hook_path[128] = "/sys/kernel/debug/econet-slic/hook";
static char ring_path[128] = "/sys/kernel/debug/econet-slic/ring";
static char lpm_path[128] = "/sys/kernel/debug/econet-slic/lpm";
static char audio_setup_path[128] = "/sys/kernel/debug/econet-slic/audio_setup";
static char speeddial_path[128] = "/root/voip-speeddial";
static int isolated_ec;
static int ctrl_port = 4444;

/* r16 shared ring group. With xr500v-voip.phone2.account_mode='shared' only
 * phone1's baresip + manager run (XR500V_SHARED=1, XR500V_LINE=1) and the pair
 * of jacks behaves as ONE line: both ring, the first lift answers, and the
 * answering jack's char device is published for baresip through
 * $XR500V_RUNTIME_DIR/phone1/audio_device (read by xr500v.c at call start).
 * Everything stays in EC space so the isolated paths are untouched:
 * jacks[0] = jack "phone1" = EC_2, jacks[1] = jack "phone2" = EC_1. */
struct jack_node {
	char hook[128], ring[128], lpm[128], audio_setup[128];
	char device[32];
	const char *led;
};
static struct jack_node jacks[2];
static int shared_mode;
static char audio_device_path[160];
/* Idle line feed per JACK index: 0 = standby (lpm 1, silent buck-boost),
 * 1 = active feed (lpm 0, instant dial tone at the price of the DC-DC hiss). */
static int idle_feed[2];
static int shared_other_logged;
#define JACK_OF_EC(ec)	((ec) == 2 ? 0 : 1)	/* EC_2 -> jack phone1 */
#define EC_OF_JACK(j)	((j) == 0 ? 2 : 1)
#define HOOK_PATH hook_path
#define RING_PATH ring_path
#define HOOK_RAW   "/sys/kernel/debug/econet-slic/hook_raw"	/* "b0 b1": b0=EC_1=phone2, b1=EC_2=phone1 */
#define SLIC_EC_P  "/sys/module/econet_slic/parameters/slic_ec"	/* active channel (1 or 2) */
#define LPM_PATH lpm_path
#define AUDIO_SETUP_P audio_setup_path
/* PHONE panel LEDs. Case labels are inverted vs the silicon: EC_2 (SLIC ch2) =
 * jack "phone1" = LED_PHONE1; EC_1 (ch1) = jack "phone2" = LED_PHONE2. We ring
 * both jacks so the single phone rings in whichever it is plugged into. */
#define LED_PHONE1 "/sys/class/leds/green:phone1"	/* EC_2 */
#define LED_PHONE2 "/sys/class/leds/green:phone2"	/* EC_1 */
#define SPEEDDIAL speeddial_path
#define CTRL_IP    "127.0.0.1"
#define CTRL_PORT ctrl_port

enum { IDLE, RINGING, INCALL, DIALING, ENDING, COLLECTING, OFFHOOK, ANSWERING };
static int state = IDLE;
static int hook_last = -1, stable_hk = -1, stable_cnt, baseline;
static long long ring_started_ms;	/* wall clock of the cadence start; 0 = not running */
static int ring_on, answered_ec = 2;
static int ring_stop_pending;
static long long hook_not_before;
static char active_callid[1024];
/* Call waiting (call-state.inc): the line's second dialog, if any. */
enum { OTHER_NONE, OTHER_WAITING, OTHER_ANSWERING, OTHER_HELD };
static int call_waiting;			/* XR500V_CALL_WAITING=1 */
static char other_callid[1024];
static int other_kind;
static long long other_since;			/* when the waiting call rang in */
static int recall_pending;			/* ringing for a held call: lifting resumes it */
static long flash_min_ms = 80, flash_max_ms = 750;	/* R key; Movistar AR HGU: 120..750 */
static long long onhook_since;			/* start of the current on-hook run */
static long long stable_since;			/* first sample of the current hook level */
static int ctrl_s = -1;				/* the ctrl_tcp connection, for cleanup */
#define CW_REJECT_MS	30000
static void enter_offhook(int ec);
static void enter_idle(void);
static const char *command_token = "x";
static char diagnostic_path[128];
#include "keypad.inc"

static void logmsg(const char *m)
{
	fprintf(stderr, "[callmgr] %s\n", m);
	fflush(stderr);
}

/* hook debugfs prints the bit as ASCII "0"/"1"; return 1=off-hook, 0=on-hook,
 * -1 on error / unreadable. */
static int read_hook_node(const char *path)
{
	char b[32];
	int fd, n, i;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	for (i = 0; i < n; i++) {
		if (b[i] == '1')
			return 1;
		if (b[i] == '0')
			return 0;
	}
	return -1;
}

static int read_hook(void) { return read_hook_node(HOOK_PATH); }

/* The jack that currently owns the audio. Identical to read_hook() outside the
 * shared ring group; there it must follow the jack that answered. */
static int read_hook_active(void)
{
	if (shared_mode)
		return read_hook_node(jacks[JACK_OF_EC(answered_ec)].hook);
	return read_hook();
}

/* Read BOTH channels' hook bits from hook_raw ("b0 b1"): h[0]=EC_1=jack phone2,
 * h[1]=EC_2=jack phone1. Returns 0 on success, -1 on error (caller falls back to
 * the single-channel read_hook). */
static int read_hook_raw(int h[2])
{
	char b[32];
	int fd, n;

	if (shared_mode) {
		/* Per-line debugfs nodes; hook_raw does not exist in the dual
		 * driver. Keep the legacy EC order: h[0]=EC_1=jack phone2. */
		int j1 = read_hook_node(jacks[0].hook);
		int j2 = read_hook_node(jacks[1].hook);

		if (j1 < 0 || j2 < 0)
			return -1;
		h[0] = j2;
		h[1] = j1;
		return 0;
	}

	if (isolated_ec) {
		n = read_hook();
		if (n < 0)
			return -1;
		h[0] = h[1] = 0;
		h[isolated_ec - 1] = n;
		return 0;
	}

	fd = open(HOOK_RAW, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	b[n] = 0;
	if (sscanf(b, "%d %d", &h[0], &h[1]) != 2)
		return -1;
	return 0;
}

/* Select the active SLIC channel (1 or 2) the audio/line-up routes to. Written
 * before "accept" so voice_dev_open lines up the jack that was answered. */
static void set_slic_ec(int ec)
{
	char c = (char)('0' + ec);
	int fd;

	if (isolated_ec)
		return;
	fd = open(SLIC_EC_P, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, &c, 1) < 0)
			;
		close(fd);
	}
}

/* ring the SLIC line: 1 = ring (STATE 0x07), 0 = stop (STATE 0x23 active+codec) */
static int ring_node(const char *path, int on)
{
	int fd = open(path, O_WRONLY);
	int saved;
	ssize_t n;

	if (fd < 0)
		return -1;
	do { n = write(fd, on ? "1\n" : "0\n", 2); } while (n < 0 && errno == EINTR);
	saved = n < 0 ? errno : EIO;
	close(fd);
	if (n == 2)
		return 0;
	errno = saved;
	return -1;
}

/* In the shared ring group BOTH bells follow the same cadence edge. */
static int ring(int on)
{
	int first, second = 0;

	/* A partial ring-on must be stopped before any new ring edge. */
	if (on && ring_stop_pending)
		return -1;
	if (shared_mode) {
		first = ring_node(jacks[0].ring, on);
		second = ring_node(jacks[1].ring, on);
	} else first = ring_node(RING_PATH, on);
	if (first || second) {
		if (!ring_stop_pending)
			logmsg("SLIC ring command failed; ring-off retry pending");
		ring_stop_pending = 1;
		return -1;
	}
	if (!on) {
		if (ring_stop_pending)
			logmsg("SLIC ring-off confirmed after retry");
		ring_stop_pending = 0;
	}
	return 0;
}

/* Drive the SLIC line feed on the CURRENT slic_ec channel via the driver's lpm
 * node: standby=1 puts the line in SS_LOWPOWER (silent buck-boost, idle DC-DC
 * coil-whine gone) with the LPM hook comparator armed; standby=0 raises the
 * active feed for a call. */
static void lpm_node(const char *path, int standby)
{
	int fd = open(path, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, standby ? "1\n" : "0\n", 2) < 0)
			;
		close(fd);
	}
}

static void lpm_set(int standby) { lpm_node(LPM_PATH, standby); }

/* Park one jack the way the operator configured it for idle. */
static void lpm_idle_jack(int j) { lpm_node(jacks[j].lpm, idle_feed[j] ? 0 : 1); }

/* Idle feed of the single line an isolated manager owns (line = 3 - ec). */
static int isolated_idle_standby(void) { return idle_feed[2 - isolated_ec] ? 0 : 1; }

static void audio_setup_read(const char *path)
{
	int fd = open(path, O_RDONLY);

	if (fd >= 0) {
		char b[8];

		if (read(fd, b, sizeof(b)) < 0)
			;
		close(fd);
	}
}

/* Publish the answering jack's char device for baresip. Private (0600) and
 * replaced atomically, because xr500v.c reads it at the start of every call.
 * A device path is not user data: logging it leaks no number. */
static void audio_device_write(const char *device)
{
	char tmp[176];
	int fd;
	size_t n = strlen(device);

	if (!*audio_device_path)
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", audio_device_path);
	unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return;
	if (write(fd, device, n) == (ssize_t)n && write(fd, "\n", 1) == 1) {
		close(fd);
		if (rename(tmp, audio_device_path) == 0) {
			fprintf(stderr, "[callmgr] audio device -> %s\n", device);
			fflush(stderr);
			return;
		}
	} else {
		close(fd);
	}
	unlink(tmp);
}

/* Idle: park BOTH jacks (EC_1=phone2, EC_2=phone1) in low-power standby so the
 * line is silent in whichever jack the phone sits, and off-hook is still sensed
 * on both via the armed LPM comparator. Leaves slic_ec back on the default (2). */
static void line_standby_both(void)
{
	if (shared_mode) {
		lpm_idle_jack(0); lpm_idle_jack(1);
		/* Back to the default jack so a stale selection cannot survive. */
		audio_device_write(jacks[0].device);
		shared_other_logged = 0;
		return;
	}
	if (isolated_ec) { lpm_set(isolated_idle_standby()); return; }
	set_slic_ec(1); lpm_set(1);
	set_slic_ec(2); lpm_set(1);
}

/* A channel that goes straight from cold initialization to low-power standby
 * does not reliably arm the Le9642 hook comparator.  PHONE2/EC_1 then remains
 * at hook_raw=0 even after the handset is lifted.  Prime each channel with a
 * short active-feed interval before parking it in standby.  This is needed
 * once at call-manager startup only; normal hang-up paths use
 * line_standby_both() directly and therefore do not pulse the line again. */
static void line_prime_and_standby_both(void)
{
	int ec;

	if (shared_mode) {
		for (ec = 0; ec < 2; ec++) {
			lpm_node(jacks[ec].lpm, 0);
			usleep(100000);
			lpm_idle_jack(ec);
			usleep(100000);
		}
		audio_device_write(jacks[0].device);
		return;
	}

	if (isolated_ec) {
		lpm_set(0); usleep(100000); lpm_set(isolated_idle_standby());
		return;
	}

	for (ec = 1; ec <= 2; ec++) {
		set_slic_ec(ec);
		lpm_set(0);
		usleep(100000);
		lpm_set(1);
		usleep(100000);
	}
	set_slic_ec(2);
}

/* Off-hook / in-call: raise the active feed on the jack the call is on, and
 * re-run audio_setup to wake the codec/ADC. Standby (STATE 0x0c) powers the
 * codec down, so the FIRST call after boot would otherwise establish with no
 * audio until a second attempt; reading the audio_setup debugfs node re-applies
 * the full audio path (switcher HP + active+codec + slots/gains) every time. */
static void line_active(int ec)
{
	if (shared_mode) {
		int j = JACK_OF_EC(ec), other = j ? 0 : 1;

		/* Publish the device BEFORE accept/dial: baresip opens it when the
		 * call starts. The collector must use the same jack. */
		snprintf(keypad_device, sizeof(keypad_device), "%s", jacks[j].device);
		audio_device_write(jacks[j].device);
		/* Park the other jack FIRST: the buck-boost switcher rail is common
		 * to both channels, so the answering jack must come up against a
		 * line that is already settled (see dual_soft_offhook in the
		 * driver). It is never stolen, only returned to its idle feed. */
		lpm_idle_jack(other);
		lpm_node(jacks[j].lpm, 0);
		audio_setup_read(jacks[j].audio_setup);
		return;
	}
	set_slic_ec(ec);
	lpm_set(0);
	audio_setup_read(AUDIO_SETUP_P);
}

/* Incoming call: raise the active feed on the line(s) that will ring before
 * the first burst. Rung straight out of low-power standby, the first burst
 * came out weak (the line still had to charge up), while the later bursts,
 * which start from an active feed, rang fully. The hang-up and answer paths
 * park the lines again as usual. */
static void line_feed_for_ring(void)
{
	if (shared_mode) {
		lpm_node(jacks[0].lpm, 0);
		lpm_node(jacks[1].lpm, 0);
		return;
	}
	lpm_set(0);
}

/* Drive ONE PHONE panel LED (by sysfs base path): 2 = blink (ringing, via the
 * kernel timer trigger), 1 = solid on (off-hook / in a call), 0 = off (on-hook).
 * Tolerates a missing LED node (open fails -> skipped). */
static void led_set(const char *base, int mode)
{
	const char *trig = (mode == 2) ? "timer" : "none";
	char path[96];
	int fd;

	if (isolated_ec && !shared_mode &&
	    strcmp(base, isolated_ec == 2 ? LED_PHONE1 : LED_PHONE2))
		return;

	snprintf(path, sizeof(path), "%s/trigger", base);
	fd = open(path, O_WRONLY);
	if (fd >= 0) {
		if (write(fd, trig, strlen(trig)) < 0)
			;
		close(fd);
	}
	if (mode == 2) {
		snprintf(path, sizeof(path), "%s/delay_on", base);
		fd = open(path, O_WRONLY);
		if (fd >= 0) { if (write(fd, "400", 3) < 0) ; close(fd); }
		snprintf(path, sizeof(path), "%s/delay_off", base);
		fd = open(path, O_WRONLY);
		if (fd >= 0) { if (write(fd, "400", 3) < 0) ; close(fd); }
	} else {
		snprintf(path, sizeof(path), "%s/brightness", base);
		fd = open(path, O_WRONLY);
		if (fd >= 0) { if (write(fd, mode ? "1" : "0", 1) < 0) ; close(fd); }
	}
}

/* ringing: blink BOTH jacks (the phone is in one of them). */
static void leds_ring(void) { led_set(LED_PHONE1, 2); led_set(LED_PHONE2, 2); }
/* answered on channel ec: that jack solid on, the other off (ec=2 -> phone1). */
static void leds_answer(int ec)
{
	led_set(LED_PHONE1, ec == 2 ? 1 : 0);
	led_set(LED_PHONE2, ec == 1 ? 1 : 0);
}
static void leds_off(void) { led_set(LED_PHONE1, 0); led_set(LED_PHONE2, 0); }

static int ctrl_connect(void)
{
	struct sockaddr_in a;
	int s = socket(AF_INET, SOCK_STREAM, 0);

	if (s < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(CTRL_PORT);
	inet_aton(CTRL_IP, &a.sin_addr);
	if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(s);
		return -1;
	}
	return s;
}

/* send a baresip ctrl_tcp command as a netstring-framed JSON object */
static int ctrl_cmd(int s, const char *cmd, const char *params)
{
	char json[2304], msg[2320], escaped[2 * sizeof(active_callid)];
	const char *token = !strcmp(cmd, "dial") ? "callmgr-dial" : command_token;
	int jl, ml, pos = 0;
	size_t i;
	if (params) {
		/* Any Call-ID the call manager can track must be addressable. */
		if (strlen(params) >= sizeof(active_callid)) return -1;
		for (i = 0; params[i]; i++) {
			unsigned char c = params[i];
			if (c < 32 || c == 127) return -1;
			if (c == '"' || c == '\\') escaped[pos++] = '\\';
			escaped[pos++] = c;
		}
	}
	escaped[pos] = 0;

	if (params && *params)
		jl = snprintf(json, sizeof(json),
			"{\"command\":\"%s\",\"params\":\"%s\",\"token\":\"%s\"}",
			cmd, escaped, token);
	else
		jl = snprintf(json, sizeof(json),
			"{\"command\":\"%s\",\"token\":\"%s\"}", cmd, token);
	if (jl < 0 || jl >= (int)sizeof(json)) return -1;
	ml = snprintf(msg, sizeof(msg), "%d:%s,", jl, json);
	if (send(s, msg, ml, MSG_NOSIGNAL) != ml) {
		logmsg("control command send failed");
		return -1;
	}
	/* Dial targets may be telephone numbers: never log their contents. */
	fprintf(stderr, "[callmgr] -> cmd %s\n", cmd);
	fflush(stderr);
	return 0;
}

static void speeddial(int s)
{
	char uri[160];
	int fd = open(SPEEDDIAL, O_RDONLY), n;

	if (fd < 0)
		return;
	n = read(fd, uri, sizeof(uri) - 1);
	close(fd);
	if (n <= 0)
		return;
	uri[n] = 0;
	while (n > 0 && (uri[n-1] == '\n' || uri[n-1] == '\r' || uri[n-1] == ' '))
		uri[--n] = 0;
	if (n > 0 && ctrl_cmd(s, "dial", uri) == 0) {
		/* Cancellation must work before CALL_ESTABLISHED arrives. */
		state = DIALING;
		logmsg("OUTGOING -> dialing");
	}
}

static int keypad_finish(int s, int ec, const char *reason)
{
	char uri[380];
	int hook;
	if (!reason) reason = keypad_destination_error();
	if (!reason && keypad_uri(uri, sizeof(uri))) reason = "uri_too_long";
	// Read while the collector still holds active feed. Closing its PCM fd
	// executes slic_dual_release(), which parks the SLIC in 0x0c. A raw hook
	// sample during that transition can falsely report on-hook. Never make
	// the dialing decision from a single post-close comparator sample.
	if (!reason) {
		hook = read_hook_active();
		if (hook != 1) reason = hook == 0 ? "on_hook_before_handoff" : "hook_unavailable";
	}
	keypad_stop();
	if (!reason) {
		line_active(ec);
		if (!ctrl_cmd(s, "dial", uri)) {
			state = DIALING;
			logmsg("KEYPAD -> dialing (active-feed hook checked before PCM release)");
			return 0;
		}
		reason = "control_send_failed";
	}
	enter_offhook(ec);
	fprintf(stderr, "[callmgr] keypad cancelled reason=%s digits=%u; hang up to retry\n", reason, digit_count);
	fflush(stderr);
	return -1;
}

/* Ring cadence by wall clock (defaults 1 s on / 4 s off, see cadence_ms).
 * The hook is sampled on every tick, ringing or not, except the tick that has
 * just energized the bell: the r17 driver owns the ring trip, so a ringing
 * line's hook node returns its frozen pre-ring value until the loop closes and
 * then 1 (with the bell already off on that channel); an older driver returns
 * the frozen value for the whole burst, i.e. the r16 OFF-gap behaviour.
 * The 450ms debounce is the silicon settle guard (shortening it -> false hooks).
 * During RINGING we watch BOTH channels' hook bits to learn which jack answered. */
#define TICK_MS		150	/* idle / ringing: cadence tick, both jacks sampled */
#define FAST_TICK_MS	40	/* in a call: the answered jack only, at the driver's hook rate */
#define DEBOUNCE_MS	450	/* silicon settle guard in wall-clock ms, whatever the tick */

/* Ringing keeps the 150 ms tick: the driver's ring trip counts consecutive
 * samples >= 150 ms apart to ride out the <100 ms SIGREG glitch at bell start.
 * With a call up (or a lifted handset after the far end hung up) the hook is
 * sampled every 40 ms so the driver mutes the capture within one sample of the
 * loop opening (the DC-DC light-load buzz the far end used to hear for ~0.2 s),
 * and the hang-up itself still waits the same 450 ms of stable on-hook. */
static int tick_ms(void)
{
	switch (state) {
	case INCALL: case ANSWERING: case DIALING: case OFFHOOK: case ENDING: case COLLECTING:
		return FAST_TICK_MS;
	default:
		return TICK_MS;
	}
}
static int debounce_samples(void)
{
	int t = tick_ms();

	return (DEBOUNCE_MS + t - 1) / t;	/* 3 at 150 ms, 12 at 40 ms */
}
#define DEBOUNCE debounce_samples()	/* samples needed at the CURRENT tick (3 slow, 12 fast) */

/* Ring cadence in milliseconds of CLOCK_MONOTONIC, never in loop iterations:
 * a tick stretched by ZSI contention (r17 test: 300 ms per tick with a probe
 * on the bus made the burst 2.2 s and the pause 8 s) must not stretch the
 * cadence. Defaults are the Argentine POTS cadence (1 s on / 4 s off);
 * XR500V_RING_ON_MS / XR500V_RING_OFF_MS override them. */
static long ring_on_ms = 1000, ring_off_ms = 4000;
/* Active feed held before the first burst (see line_feed_for_ring);
 * XR500V_RING_PREFEED_MS overrides it, 0 disables the pre-feed. */
static long ring_prefeed_ms = 1500;
#ifndef cadence_now_ms
#define cadence_now_ms clock_ms		/* tests substitute a fake clock */
#endif

/* Digit strings dialed immediately, without waiting the inter-digit timeout.
 * Empty by default: the list is per country and matched exactly, so a longer
 * number starting with one of them could no longer be dialed. The service sets
 * it from UCI through XR500V_EMERGENCY_NUMBERS. */
static char emergency_numbers[256] = "";

static long cadence_ms(const char *env, long fallback_ms)
{
	long ms = fallback_ms;

	if (env && *env) {
		char *end;
		ms = strtol(env, &end, 10);
		if (*end || ms < 300 || ms > 10000) ms = fallback_ms;
	}
	return ms;
}

/* XR500V_IDLE_FEED* : "active" keeps the line powered at idle (instant dial
 * tone, audible DC-DC hiss); anything else, including unset, means standby. */
static int feed_is_active(const char *env)
{
	return env && !strcmp(env, "active");
}

static int emergency_match(const char *digits)
{
	const char *p = emergency_numbers;
	size_t n = strlen(digits);

	while (*p) {
		size_t len = strcspn(p, " ,;");
		if (len && len == n && !strncmp(p, digits, len)) return 1;
		p += len;
		while (*p == ' ' || *p == ',' || *p == ';') p++;
	}
	return 0;
}

/* One key from the collector: 1 = dial now, 0 = keep collecting, -1 = too
 * many digits. '#' is the send key, except in a service code, a string that
 * starts with '*' or '#' (*43#, #43#, *#43#, *21*<number>#): there it is
 * part of the code and ends it once the code holds a digit. Codes without a
 * final '#' (*611) still go out on the inter-digit timeout. */
static int keypad_key(char c)
{
	int service = digit_count ? dial_digits[0] == '*' || dial_digits[0] == '#' : c == '#';

	if (c == '#' && !service) {
		logmsg("keypad send key detected (#)");
		return 1;
	}
	if (!((c >= '0' && c <= '9') || c == '*' || c == '#'))
		return 0;
	if (digit_count >= sizeof(dial_digits) - 1)
		return -1;
	dial_digits[digit_count++] = c; dial_digits[digit_count] = 0;
	digit_at = clock_ms();
	logmsg("keypad digit accepted");
	if (c == '#' && digit_count > 1 && strpbrk(dial_digits, "0123456789")) {
		logmsg("keypad service code complete: dialing");
		return 1;
	}
	if (!strcmp(dial_kind, "provider") && emergency_match(dial_digits)) {
		logmsg("keypad emergency number: dialing immediately");
		return 1;
	}
	// Local extension labels are not provider phone numbers.
	if (!strcmp(dial_kind, "local") && digit_count == 1 && (c == '1' || c == '2')) {
		fprintf(stderr, "[callmgr] local keypad target=Phone%c source=Phone%d\n", c, 3 - isolated_ec);
		fflush(stderr);
	}
	return 0;
}

#include "cid.inc"
#include "registration.inc"
#include "call-state.inc"

/* One tick of the ring cadence. Returns 1 while the bell is energized and
 * leaves ring_edge = 1 on the tick that turned it on. */
static int ring_edge;

static int ring_cadence_tick(void)
{
	int ringing_now = 0;

	ring_edge = 0;
	if (ring_stop_pending) {
		if (ring(0)) ring_edge = 1; /* no answer until both stops succeeded */
		else { ring_on = 0; ring_started_ms = 0; }
		return 0;
	}
	if (state == RINGING) {
		long long now = cadence_now_ms();
		long phase;

		if (!ring_started_ms) {
			if (ring_prefeed_ms)
				line_feed_for_ring();
			ring_started_ms = now + ring_prefeed_ms;
		}
		if (now < ring_started_ms)
			return 0;
		phase = (long)((now - ring_started_ms) % (ring_on_ms + ring_off_ms));
		ringing_now = (phase < ring_on_ms);
		/* Only (re)assert the SLIC ring state on a transition.
		 * Writing STATE 0x07 every tick re-enters BAL_RING and
		 * restarts the ring waveform, so the bell stutters / rings
		 * continuously instead of cadencing. Toggle on edges only. */
		if (ringing_now != ring_on) {
			if (ring(ringing_now)) {
				ring_edge = 1;
				return 0;
			}
			ring_on = ringing_now;
			ring_edge = ringing_now;
		}
	} else {
		if (ring_on) {		/* left RINGING -> make sure ring is off */
			ring(0);
			ring_on = 0;
		}
		ring_started_ms = 0;
	}
	return ringing_now;
}

/* One hook sample. Waiting for an answer (RINGING/IDLE) we watch BOTH jacks;
 * in a call we watch only the answered channel (for hang-up). `which` = the
 * jack that went off-hook, so the audio is routed to it before "accept". */
static void hook_poll_tick(int s, int diagnostic_fd)
{
	static unsigned fast_ticks;
	int h[2], hk, which = answered_ec;

	/* Fast tick: one ZSI read (the answered jack) per 40 ms; the other jack
	 * is only looked at every ~600 ms for the shared-mode diagnostic. A full
	 * two-jack read holds the ZSI lock 16-32 ms and would crowd the bus. */
	if (state != IDLE && state != RINGING && (++fast_ticks % 15)) {
		hk = read_hook_active();
		handle_hook_sample(s, hk, which, diagnostic_fd);
		return;
	}
	if (read_hook_raw(h) == 0) {
		if (state != IDLE && state != RINGING) {
			hk = h[(answered_ec == 1) ? 0 : 1];
			/* The jack that did not answer never takes
			 * the call over and never ends it. */
			if (shared_mode && h[(answered_ec == 1) ? 1 : 0] &&
			    !shared_other_logged) {
				shared_other_logged = 1;
				logmsg("shared: the other jack was lifted during a call; ignored");
			}
		} else {		/* RINGING or IDLE: either jack */
			hk = h[0] || h[1];
			which = h[0] ? 1 : (h[1] ? 2 : answered_ec);
		}
	} else {
		hk = read_hook();	/* fallback: active channel only */
	}

	handle_hook_sample(s, hk, which, diagnostic_fd);
}

int main(void)
{
	char buf[8192];
	int s = -1, blen = 0;
	int diagnostic_fd = -1, diagnostic_len = 0;
	char diagnostic_buf[256];
	const char *line_env = getenv("XR500V_LINE");
	const char *runtime = getenv("XR500V_RUNTIME_DIR");
	if (!runtime || !*runtime)
		runtime = "/tmp/xr500v-dual";
	if (*runtime != '/' || strlen(runtime) > 80)
		return 2;

	ring_on_ms = cadence_ms(getenv("XR500V_RING_ON_MS"), 1000);
	ring_off_ms = cadence_ms(getenv("XR500V_RING_OFF_MS"), 4000);
	{
		const char *e = getenv("XR500V_RING_PREFEED_MS");
		char *end;
		long ms;

		if (e && *e) {
			ms = strtol(e, &end, 10);
			if (!*end && ms >= 0 && ms <= 5000)
				ring_prefeed_ms = ms;
		}
	}
	cid_fsk = cid_mode(getenv("XR500V_CALLER_ID"));
	{
		const char *e = getenv("XR500V_CALL_WAITING");
		long lo = flash_min_ms, hi = flash_max_ms;
		char *end;

		call_waiting = e && !strcmp(e, "1");
		e = getenv("XR500V_FLASH_MIN_MS");
		if (e && *e) { lo = strtol(e, &end, 10); if (*end) lo = -1; }
		e = getenv("XR500V_FLASH_MAX_MS");
		if (e && *e) { hi = strtol(e, &end, 10); if (*end) hi = -1; }
		/* The hang-up waits past the maximum: keep it under 1.5 s. */
		if (lo >= 40 && hi <= 1500 && lo < hi) {
			flash_min_ms = lo; flash_max_ms = hi;
		}
	}
	{
		const char *e = getenv("XR500V_EMERGENCY_NUMBERS");

		if (e && strspn(e, "0123456789*# ,;") == strlen(e) && strlen(e) < sizeof(emergency_numbers))
			strcpy(emergency_numbers, e);
	}

	if (line_env) {
		int line = atoi(line_env), j;
		const char *shared_env = getenv("XR500V_SHARED");

		if (line < 1 || line > 2)
			return 2;
		for (j = 0; j < 2; j++) {
			snprintf(jacks[j].hook, sizeof(jacks[j].hook),
				 "/sys/kernel/debug/econet-slic/phone%d/hook", j + 1);
			snprintf(jacks[j].ring, sizeof(jacks[j].ring),
				 "/sys/kernel/debug/econet-slic/phone%d/ring", j + 1);
			snprintf(jacks[j].lpm, sizeof(jacks[j].lpm),
				 "/sys/kernel/debug/econet-slic/phone%d/lpm", j + 1);
			snprintf(jacks[j].audio_setup, sizeof(jacks[j].audio_setup),
				 "/sys/kernel/debug/econet-slic/phone%d/audio_setup", j + 1);
			snprintf(jacks[j].device, sizeof(jacks[j].device),
				 "/dev/xr500v-voice%s", j ? "1" : "");
			jacks[j].led = j ? LED_PHONE2 : LED_PHONE1;
		}
		if (shared_env && !strcmp(shared_env, "1")) {
			/* Only phone1 owns the account of a shared ring group. */
			if (line != 1) {
				logmsg("XR500V_SHARED requires XR500V_LINE=1");
				return 2;
			}
			shared_mode = 1;
			snprintf(audio_device_path, sizeof(audio_device_path),
				 "%s/phone1/audio_device", runtime);
			idle_feed[0] = feed_is_active(getenv("XR500V_IDLE_FEED_1"));
			idle_feed[1] = feed_is_active(getenv("XR500V_IDLE_FEED_2"));
		} else {
			idle_feed[line - 1] = feed_is_active(getenv("XR500V_IDLE_FEED"));
		}
		isolated_ec = 3 - line;
		answered_ec = isolated_ec;
		ctrl_port = 4443 + line;
		snprintf(hook_path, sizeof(hook_path), "/sys/kernel/debug/econet-slic/phone%d/hook", line);
		snprintf(ring_path, sizeof(ring_path), "/sys/kernel/debug/econet-slic/phone%d/ring", line);
		snprintf(lpm_path, sizeof(lpm_path), "/sys/kernel/debug/econet-slic/phone%d/lpm", line);
		snprintf(audio_setup_path, sizeof(audio_setup_path), "/sys/kernel/debug/econet-slic/phone%d/audio_setup", line);
		snprintf(speeddial_path, sizeof(speeddial_path), "%s/phone%d/speeddial", runtime, line);
		snprintf(diagnostic_path, sizeof(diagnostic_path), "%s/phone%d/control", runtime, line);
		snprintf(dialing_path, sizeof(dialing_path), "%s/phone%d/dialing", runtime, line);
		snprintf(registration_path, sizeof(registration_path), "%s/phone%d/registration", runtime, line);
		reg_reset();	/* a previous instance's state is stale */
		snprintf(keypad_device, sizeof(keypad_device), "/dev/xr500v-voice%s", line == 2 ? "1" : "");
	}
	if (*diagnostic_path) {
		/* ctrl_tcp permits ONE client. Diagnostics must use our connection,
		 * never connect directly and displace the hook/event manager. */
		{
			struct stat st;

			if ((mkfifo(diagnostic_path, 0600) == 0 || errno == EEXIST) &&
			    lstat(diagnostic_path, &st) == 0 && S_ISFIFO(st.st_mode) && st.st_uid == getuid())
				diagnostic_fd = open(diagnostic_path, O_RDWR | O_NONBLOCK);
		}
	}

	logmsg(shared_mode ? "starting (release 9, shared ring group on phone1)" :
			     "starting (release 9, off-hook busy policy)");
	if (call_waiting)
		fprintf(stderr, "[callmgr] call waiting on, flash %ld..%ld ms\n", flash_min_ms, flash_max_ms);
	signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal);
	leds_off();			/* idle: both PHONE LEDs off */
	ring(0);			/* a respawn mid-ring must not leave the bell energized */
	line_prime_and_standby_both();	/* cold boot: arm both hook comparators, then idle */
	while (!stopping) {
		struct pollfd pfd[3];
		int rc;

		if (s < 0) {
			if (ring_stop_pending) ring(0);
			s = ctrl_connect();
			if (s < 0) { sleep(2); continue; }
			blen = 0;
			ctrl_s = s;
			logmsg("connected to ctrl_tcp");
			/* The accounts may have registered before we connected. */
			command_token = "callmgr-reginfo";
			ctrl_cmd(s, "reginfo", NULL);
			command_token = "x";
		}

		pfd[0].fd = s;
		pfd[0].events = POLLIN;
		pfd[1].fd = diagnostic_fd;
		pfd[1].events = POLLIN;
		pfd[2].fd = keypad_fd; pfd[2].events = POLLIN;
		rc = poll(pfd, 3, tick_ms());	/* bounded tick: cadence + hook poll */

		if (rc > 0 && (pfd[1].revents & POLLIN)) {
			int n = read(diagnostic_fd, diagnostic_buf + diagnostic_len,
				     sizeof(diagnostic_buf) - 1 - diagnostic_len);
			if (n > 0) {
				char *end;
				diagnostic_len += n;
				diagnostic_buf[diagnostic_len] = 0;
				while ((end = strchr(diagnostic_buf, '\n'))) {
					char token[48], cmd[24], extra;
					int used = end - diagnostic_buf + 1;
					*end = 0;
					if (sscanf(diagnostic_buf, "%47s %23s %c", token, cmd, &extra) == 2 &&
					    !strncmp(token, "diag-", 5) &&
					    strspn(token, "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(token) &&
					    (!strcmp(cmd, "mute") || !strcmp(cmd, "listcalls") || !strcmp(cmd, "callstat"))) {
						command_token = token;
						ctrl_cmd(s, cmd, NULL);
						command_token = "x";
					}
					diagnostic_len -= used;
					memmove(diagnostic_buf, diagnostic_buf + used, diagnostic_len);
					diagnostic_buf[diagnostic_len] = 0;
				}
				if (diagnostic_len == sizeof(diagnostic_buf) - 1)
					diagnostic_len = 0;
			}
		}

		/* ---- baresip events ---- */
		if (rc > 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
			int n = read(s, buf + blen, sizeof(buf) - 1 - blen);
			char *q;

			if (n <= 0) { ctrl_lost(); close(s); s = -1; continue; }
			blen += n;
			buf[blen] = 0;
			q = buf;
			for (;;) {		/* parse complete netstrings <len>:<json>, */
				char *colon = memchr(q, ':', blen - (q - buf));
				char *payload;
				int len;

				if (!colon)
					break;
				len = atoi(q);
				if (len < 0 || len > (int)sizeof(buf) - 32) { q = buf + blen; break; }
				payload = colon + 1;
				if (payload + len + 1 > buf + blen)
					break;		/* incomplete, wait for more */
				payload[len] = 0;	/* terminate the JSON (over the ',') */
				handle_call_event(s, payload);
				q = payload + len + 1;	/* past json + ',' */
			}
			blen -= (q - buf);
			if (blen > 0)
				memmove(buf, q, blen);
			buf[blen] = 0;
			if (blen > (int)sizeof(buf) - 64)
				blen = 0;	/* runaway guard */
		}

		if (state == COLLECTING) {
			char digits[32];
			int finish = 0, abort = 0, i;
			const char *reason = NULL;
			if (pfd[2].revents & POLLIN) {
				int n = read(keypad_fd, digits, sizeof(digits));
				if (n <= 0) { abort = 1; reason = "collector_read_failed"; }
				for (i = 0; i < n; i++) {
					int r = keypad_key(digits[i]);

					if (r < 0) { abort = 1; reason = "digit_limit"; break; }
					if (r > 0) { finish = 1; break; }
				}
			}
			if (pfd[2].revents & (POLLHUP | POLLERR)) { abort = 1; reason = "collector_closed"; }
			if (!finish && digit_count && clock_ms() - digit_at >= 4000) { finish = 1; logmsg("keypad interdigit timeout"); }
			if (clock_ms() - dial_started >= 30000) { abort = 1; reason = "collection_timeout"; }
			if (finish || abort) keypad_finish(s, answered_ec, reason);
		}

		/* ---- ring cadence (only while RINGING) ---- */
		ring_cadence_tick();
		cid_tick(s, diagnostic_fd);
		other_tick(s);

		/* ---- hook poll (debounced) ----
		 * Skipped only on the tick that has just energized the bell (the
		 * driver's 0x03 -> 20 ms -> 0x07 sequence has just completed). */
		if (!ring_edge)
			hook_poll_tick(s, diagnostic_fd);
	}
	keypad_stop(); cid_stop(); ring(0); leds_off(); line_standby_both();
	if (s >= 0) close(s);
	if (diagnostic_fd >= 0) close(diagnostic_fd);
	return 0;
}
