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

#define HOOK_PATH  "/sys/kernel/debug/econet-slic/hook"
#define RING_PATH  "/sys/kernel/debug/econet-slic/ring"
#define HOOK_RAW   "/sys/kernel/debug/econet-slic/hook_raw"	/* "b0 b1": b0=EC_1=phone2, b1=EC_2=phone1 */
#define SLIC_EC_P  "/sys/module/econet_slic/parameters/slic_ec"	/* active channel (1 or 2) */
#define LPM_PATH   "/sys/kernel/debug/econet-slic/lpm"		/* 1 = low-power standby (silent), 0 = active feed */
#define AUDIO_SETUP_P "/sys/kernel/debug/econet-slic/audio_setup"	/* read re-runs slic_audio_setup() (wakes codec) */
/* PHONE panel LEDs. Case labels are inverted vs the silicon: EC_2 (SLIC ch2) =
 * jack "phone1" = LED_PHONE1; EC_1 (ch1) = jack "phone2" = LED_PHONE2. We ring
 * both jacks so the single phone rings in whichever it is plugged into. */
#define LED_PHONE1 "/sys/class/leds/green:phone1"	/* EC_2 */
#define LED_PHONE2 "/sys/class/leds/green:phone2"	/* EC_1 */
#define SPEEDDIAL  "/root/voip-speeddial"	/* optional: 1 line, a SIP URI */
#define CTRL_IP    "127.0.0.1"
#define CTRL_PORT  4444

enum { IDLE, RINGING, INCALL };
static int state = IDLE;

static void logmsg(const char *m)
{
	fprintf(stderr, "[callmgr] %s\n", m);
	fflush(stderr);
}

/* hook debugfs prints the bit as ASCII "0"/"1"; return 1=off-hook, 0=on-hook,
 * -1 on error / unreadable. */
static int read_hook(void)
{
	char b[32];
	int fd, n, i;

	fd = open(HOOK_PATH, O_RDONLY);
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

/* Read BOTH channels' hook bits from hook_raw ("b0 b1"): h[0]=EC_1=jack phone2,
 * h[1]=EC_2=jack phone1. Returns 0 on success, -1 on error (caller falls back to
 * the single-channel read_hook). */
static int read_hook_raw(int h[2])
{
	char b[32];
	int fd, n;

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
	int fd = open(SLIC_EC_P, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, &c, 1) < 0)
			;
		close(fd);
	}
}

/* ring the SLIC line: 1 = ring (STATE 0x07), 0 = stop (STATE 0x23 active+codec) */
static void ring(int on)
{
	int fd = open(RING_PATH, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, on ? "1\n" : "0\n", 2) < 0)
			;
		close(fd);
	}
}

/* Drive the SLIC line feed on the CURRENT slic_ec channel via the driver's lpm
 * node: standby=1 puts the line in SS_LOWPOWER (silent buck-boost, idle DC-DC
 * coil-whine gone) with the LPM hook comparator armed; standby=0 raises the
 * active feed for a call. */
static void lpm_set(int standby)
{
	int fd = open(LPM_PATH, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, standby ? "1\n" : "0\n", 2) < 0)
			;
		close(fd);
	}
}

/* Idle: park BOTH jacks (EC_1=phone2, EC_2=phone1) in low-power standby so the
 * line is silent in whichever jack the phone sits, and off-hook is still sensed
 * on both via the armed LPM comparator. Leaves slic_ec back on the default (2). */
static void line_standby_both(void)
{
	set_slic_ec(1); lpm_set(1);
	set_slic_ec(2); lpm_set(1);
}

/* Off-hook / in-call: raise the active feed on the jack the call is on, and
 * re-run audio_setup to wake the codec/ADC. Standby (STATE 0x0c) powers the
 * codec down, so the FIRST call after boot would otherwise establish with no
 * audio until a second attempt; reading the audio_setup debugfs node re-applies
 * the full audio path (switcher HP + active+codec + slots/gains) every time. */
static void line_active(int ec)
{
	int fd;

	set_slic_ec(ec);
	lpm_set(0);
	fd = open(AUDIO_SETUP_P, O_RDONLY);
	if (fd >= 0) {
		char b[8];
		if (read(fd, b, sizeof(b)) < 0)
			;
		close(fd);
	}
}

/* Drive ONE PHONE panel LED (by sysfs base path): 2 = blink (ringing, via the
 * kernel timer trigger), 1 = solid on (off-hook / in a call), 0 = off (on-hook).
 * Tolerates a missing LED node (open fails -> skipped). */
static void led_set(const char *base, int mode)
{
	const char *trig = (mode == 2) ? "timer" : "none";
	char path[96];
	int fd;

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
static void ctrl_cmd(int s, const char *cmd, const char *params)
{
	char json[256], msg[300];
	int jl, ml;

	if (params && *params)
		jl = snprintf(json, sizeof(json),
			"{\"command\":\"%s\",\"params\":\"%s\",\"token\":\"x\"}",
			cmd, params);
	else
		jl = snprintf(json, sizeof(json),
			"{\"command\":\"%s\",\"token\":\"x\"}", cmd);
	ml = snprintf(msg, sizeof(msg), "%d:%s,", jl, json);
	if (write(s, msg, ml) < 0)
		;
	fprintf(stderr, "[callmgr] -> cmd %s %s\n", cmd, params ? params : "");
	fflush(stderr);
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
	if (n > 0)
		ctrl_cmd(s, "dial", uri);
}

/* Ring cadence (in 150ms ticks): ~0.75 s on, ~1.05 s off. The SLIC ring voltage
 * corrupts the SIGREG hook bit, so we only sample the hook during the OFF gap.
 * The 450ms debounce is the silicon settle guard (shortening it -> false hooks).
 * During RINGING we watch BOTH channels' hook bits to learn which jack answered. */
#define RING_ON_TICKS	5
#define RING_PERIOD	12
#define DEBOUNCE	3	/* consecutive stable reads (3*150=450ms) before a hook change counts */

int main(void)
{
	char buf[8192];
	int s = -1, blen = 0;
	int hook_last = -1, stable_hk = -1, stable_cnt = 0, baseline = 0;
	int ring_tick = 0, ring_on = 0;	/* ring_on = SLIC ring state we last asserted */
	int answered_ec = 2;		/* channel/jack the active call was answered on (default phone1=EC_2) */

	logmsg("starting");
	leds_off();			/* idle: both PHONE LEDs off */
	line_standby_both();		/* idle: silent low-power feed on both jacks */
	for (;;) {
		struct pollfd pfd;
		int rc, ringing_now = 0;

		if (s < 0) {
			s = ctrl_connect();
			if (s < 0) { sleep(2); continue; }
			blen = 0;
			logmsg("connected to ctrl_tcp");
		}

		pfd.fd = s;
		pfd.events = POLLIN;
		rc = poll(&pfd, 1, 150);	/* 150ms tick: cadence + hook poll */

		/* ---- baresip events ---- */
		if (rc > 0 && (pfd.revents & POLLIN)) {
			int n = read(s, buf + blen, sizeof(buf) - 1 - blen);
			char *q;

			if (n <= 0) { close(s); s = -1; continue; }
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
				payload = colon + 1;
				if (payload + len + 1 > buf + blen)
					break;		/* incomplete, wait for more */
				payload[len] = 0;	/* terminate the JSON (over the ',') */

				if (strstr(payload, "\"CALL_INCOMING\"")) {
					if (state == IDLE) {
						state = RINGING;
						ring_tick = 0;
						leds_ring();	/* ringing: blink both jacks */
						logmsg("INCOMING -> ringing");
					}
				} else if (strstr(payload, "\"CALL_ESTABLISHED\"")) {
					state = INCALL;
					ring(0);
					leds_answer(answered_ec);	/* in call: answered jack solid on */
					logmsg("ESTABLISHED");
				} else if (strstr(payload, "\"CALL_CLOSED\"")) {
					state = IDLE;
					ring(0);
					leds_off();	/* idle: both off */
					line_standby_both();	/* back to silent low-power feed */
					answered_ec = 2;
					logmsg("CLOSED -> idle");
				}
				q = payload + len + 1;	/* past json + ',' */
			}
			blen -= (q - buf);
			if (blen > 0)
				memmove(buf, q, blen);
			buf[blen] = 0;
			if (blen > (int)sizeof(buf) - 64)
				blen = 0;	/* runaway guard */
		}

		/* ---- ring cadence (only while RINGING) ---- */
		if (state == RINGING) {
			int phase = ring_tick % RING_PERIOD;

			ringing_now = (phase < RING_ON_TICKS);
			/* Only (re)assert the SLIC ring state on a transition.
			 * Writing STATE 0x07 every tick re-enters BAL_RING and
			 * restarts the ring waveform, so the bell stutters / rings
			 * continuously instead of cadencing. Toggle on edges only. */
			if (ringing_now != ring_on) {
				ring(ringing_now);
				ring_on = ringing_now;
			}
			ring_tick++;
		} else {
			if (ring_on) {		/* left RINGING -> make sure ring is off */
				ring(0);
				ring_on = 0;
			}
			ring_tick = 0;
		}

		/* ---- hook poll (debounced; skip while the ring is energized) ----
		 * Waiting for an answer (RINGING/IDLE) we watch BOTH jacks; in a call
		 * we watch only the answered channel (for hang-up). `which` = the jack
		 * that went off-hook, so the audio is routed to it before "accept". */
		if (!ringing_now) {
			int h[2], hk, which = answered_ec;

			if (read_hook_raw(h) == 0) {
				if (state == INCALL) {
					hk = h[(answered_ec == 1) ? 0 : 1];
				} else {		/* RINGING or IDLE: either jack */
					hk = h[0] || h[1];
					which = h[0] ? 1 : (h[1] ? 2 : answered_ec);
				}
			} else {
				hk = read_hook();	/* fallback: active channel only */
			}

			if (hk >= 0) {
				if (hk == stable_hk) {
					if (stable_cnt < 50)
						stable_cnt++;
				} else {
					stable_hk = hk;
					stable_cnt = 1;
				}
				if (stable_cnt == DEBOUNCE && stable_hk != hook_last) {
					if (!baseline) {
						baseline = 1;		/* first settle: no action */
					} else {
						fprintf(stderr, "[callmgr] hook %d->%d (state=%d ec=%d)\n",
							hook_last, stable_hk, state, which);
						fflush(stderr);
						if (stable_hk == 1) {		/* off-hook (lifted) */
							if (state == RINGING) {
								answered_ec = which;
								set_slic_ec(answered_ec);	/* route audio to this jack BEFORE accept */
								leds_answer(answered_ec);
								ctrl_cmd(s, "accept", NULL);
								ring(0);
								line_active(answered_ec);	/* wake codec so incoming audio works first try */
							} else if (state == IDLE) {
								answered_ec = which;
								set_slic_ec(answered_ec);
								line_active(answered_ec);	/* raise active feed before dialing */
								leds_answer(answered_ec);
								speeddial(s);
							} else {
								leds_answer(answered_ec);
							}
						} else {			/* on-hook */
							/* During RINGING the handset is SUPPOSED to be
							 * on-hook (not answered yet), so an on-hook read
							 * here is the NORMAL state and must NOT hang up.
							 * The SLIC SIGREG reads a false off-hook at cold
							 * start (only a BAL_RING cycle corrects it), so the
							 * callmgr's baseline starts at 1; the SIGREG's
							 * 1->0 self-correction during the FIRST ring used
							 * to trip this branch -> spurious hangup -> baresip
							 * 486 Busy Here on the first call. A caller giving
							 * up arrives as CALL_CLOSED, not via the hook, so
							 * only a real on-hook while INCALL is a hang-up. */
							if (state == INCALL) {
								ctrl_cmd(s, "hangup", NULL);
								ring(0);
								leds_off();
								line_standby_both();	/* hang-up: back to silent standby */
								state = IDLE;
								answered_ec = 2;
							} else if (state == IDLE) {
								leds_off();
								line_standby_both();	/* lifted then hung up w/o a call: re-park standby */
							}
							/* state == RINGING: keep ringing (bell + LEDs). */
						}
					}
					hook_last = stable_hk;
				}
			}
		}
	}
	return 0;
}
