/* SPDX-License-Identifier: GPL-2.0-or-later
 * One owned SIP dialog per physical port. A closed SIP dialog is not an
 * on-hook indication. Rejected/unrelated dialogs never own the PCM/LED state.
 *
 * Call waiting (XR500V_CALL_WAITING=1) adds at most ONE second dialog,
 * other_callid: a call ringing in while active_callid is being talked on
 * (OTHER_WAITING; xr500v.c plays the tone), or the call put on hold when it
 * was answered (OTHER_HELD). A hook flash (the R key) answers the waiting
 * call or swaps the two; hanging up rings the phone again for the one left.
 */
static void enter_idle(void)
{
	keypad_stop();
	cid_stop();
	/* Nothing may outlive the port going idle: a held call would keep the
	 * line's PCM stream open. */
	if (*other_callid && ctrl_s >= 0)
		ctrl_cmd(ctrl_s, "hangup", other_callid);
	other_callid[0] = 0; other_kind = OTHER_NONE; recall_pending = 0;
	active_callid[0] = 0;
	state = IDLE;
	ring(0); ring_on = 0; ring_started_ms = 0;
	leds_off();
	line_standby_both();
}

static void enter_offhook(int ec)
{
	keypad_stop();
	cid_stop();
	active_callid[0] = 0;
	state = OFFHOOK;
	ring(0); ring_on = 0; ring_started_ms = 0;
	answered_ec = ec;
	/* PCM close parks the SLIC and can report false on-hook. Restore active
	 * feed before considering release, then require fresh debounced samples.
	 * No automatic re-dial: only a real on-hook releases this port. */
	line_active(ec);
	leds_answer(ec);
	hook_not_before = clock_ms() + 450;
	stable_cnt = 0; stable_hk = -1;
}

static void finish_owned_call(void)
{
	if (baseline && hook_last == 0)
		enter_idle();
	else
		enter_offhook(answered_ec);
}

/* The ctrl_tcp connection dropped: baresip exited or was respawned, and
 * every dialog it held went with it, the second one too. */
static void ctrl_lost(void)
{
	ctrl_s = -1;
	other_callid[0] = 0; other_kind = OTHER_NONE; recall_pending = 0;
	finish_owned_call();
	reg_reset();	/* baresip is gone, and its registrations with it */
}

/* The handset went on-hook with a second call left: ring the phone for it.
 * Lifting answers a waiting call and resumes a held one (recall). */
static void ring_for_other(void)
{
	recall_pending = other_kind == OTHER_HELD;
	strcpy(active_callid, other_callid);
	other_callid[0] = 0; other_kind = OTHER_NONE;
	keypad_stop();
	cid_stop();
	line_standby_both();
	state = RINGING; ring_on = 0; ring_started_ms = 0;
	leds_ring();
	logmsg(recall_pending ? "ONHOOK -> ringing back the held call" :
				"ONHOOK -> ringing for the waiting call");
}

static void handle_flash(int s, long long ms)
{
	fprintf(stderr, "[callmgr] flash %lld ms (state=%d other=%d)\n", ms, state, other_kind);
	fflush(stderr);
	if (other_kind == OTHER_WAITING) {
		other_kind = OTHER_ANSWERING;
		ctrl_cmd(s, "accept", other_callid);
		logmsg("FLASH -> answer the waiting call, the other one goes on hold");
	} else if (other_kind == OTHER_HELD && state == INCALL && *active_callid) {
		char held[sizeof(active_callid)];

		ctrl_cmd(s, "resume", other_callid);
		strcpy(held, active_callid);
		strcpy(active_callid, other_callid);
		strcpy(other_callid, held);
		logmsg("FLASH -> swap: the held call resumed, the other one on hold");
	} else {
		logmsg("FLASH ignored: no second call");
	}
}

/* A waiting call nobody answers is rejected, as the Movistar AR HGU does
 * (X_ZyXEL_CallWaitingRejectTime 30 s). */
static void other_tick(int s)
{
	if (other_kind == OTHER_WAITING && clock_ms() - other_since >= CW_REJECT_MS) {
		ctrl_cmd(s, "hangup", other_callid);
		other_callid[0] = 0; other_kind = OTHER_NONE;
		logmsg("waiting call not answered in 30 s: rejected");
	}
}

static const char *event_string(struct json_object *obj, const char *key)
{
	struct json_object *v;
	const char *s;
	if (!json_object_object_get_ex(obj, key, &v) ||
	    !json_object_is_type(v, json_type_string))
		return NULL;
	s = json_object_get_string(v);
	return s && strlen(s) == (size_t)json_object_get_string_len(v) ? s : NULL;
}

static int event_true(struct json_object *obj, const char *key)
{
	struct json_object *v;
	return json_object_object_get_ex(obj, key, &v) &&
		json_object_is_type(v, json_type_boolean) && json_object_get_boolean(v);
}

static int valid_callid(const char *id)
{
	const unsigned char *p = (const unsigned char *)id;
	if (!id || !*id || strlen(id) >= sizeof(active_callid)) return 0;
	for (; *p; p++) if (*p <= 32 || *p >= 127) return 0;
	return 1;
}

static int handset_available(void)
{
	int h[2];
	/* Check the physical level too: an INVITE may arrive just before the
	 * poll has debounced a lift. Unknown is not permission to ring/answer. */
	return state == IDLE && baseline && hook_last == 0 &&
		read_hook_raw(h) == 0 && h[0] == 0 && h[1] == 0;
}

static void handle_call_event(int s, const char *payload)
{
	struct json_tokener *tok = json_tokener_new();
	struct json_object *obj;
	const char *type, *id, *klass, *token, *direction;
	size_t len = strlen(payload);
	if (!tok) return;
	json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
	obj = json_tokener_parse_ex(tok, payload, len);
	if (json_tokener_get_error(tok) != json_tokener_success ||
	    json_tokener_get_parse_end(tok) != len ||
	    !json_object_is_type(obj, json_type_object)) {
		json_object_put(obj); json_tokener_free(tok); return;
	}
	json_tokener_free(tok);
	token = event_string(obj, "token");
	if (event_true(obj, "response")) {
		if (token && !strncmp(token, "diag-", 5)) logmsg(payload);
		if (token && !strcmp(token, "callmgr-reginfo") && event_true(obj, "ok"))
			reg_reginfo(event_string(obj, "data"));
		if (token && !strcmp(token, "callmgr-dial") &&
		    !event_true(obj, "ok") && !*active_callid &&
		    (state == DIALING || state == ENDING)) {
			finish_owned_call();
			logmsg("DIAL failed -> wait for on-hook");
		}
		goto out;
	}
	klass = event_string(obj, "class");
	type = event_string(obj, "type");
	id = event_string(obj, "id");
	direction = event_string(obj, "direction");
	if (!event_true(obj, "event") || !klass || !type) goto out;
	/* Baresip 3.x reports registrations in the "ua" class, older
	 * versions in "register"; reg_event() ignores the other types. */
	if (!strcmp(klass, "ua") || !strcmp(klass, "register")) {
		reg_event(type, event_string(obj, "accountaor"), event_string(obj, "param"));
		goto out;
	}
	if (strcmp(klass, "call")) goto out;
	if (!valid_callid(id)) {
		/* A dialog the call manager cannot track would hold one of
		 * Baresip's call slots for good: reject it. One too long to
		 * address is still Baresip's current call while this line owns
		 * none, so hang that up. */
		if (!strcmp(type, "CALL_INCOMING")) {
			if (state == IDLE && !*active_callid && !*other_callid) {
				ctrl_cmd(s, "hangup", NULL);
				logmsg("INCOMING with an untrackable Call-ID -> rejected");
			} else {
				logmsg("INCOMING with an untrackable Call-ID -> ignored (line busy)");
			}
		}
		goto out;
	}
	if (!strcmp(type, "CALL_INCOMING")) {
		if (!strcmp(id, active_callid) || !strcmp(id, other_callid))
			goto out; /* duplicate event */
		if (!handset_available()) {
			if (call_waiting && state == INCALL && *active_callid && !*other_callid) {
				strcpy(other_callid, id);
				other_kind = OTHER_WAITING;
				other_since = clock_ms();
				logmsg("INCOMING -> call waiting (R answers it)");
				goto out;
			}
			/* Address exactly this dialog, never Baresip's current selection.
			 * Keep collecting digits, the dial tone and any owned call intact. */
			ctrl_cmd(s, "hangup", id);
			logmsg("INCOMING -> busy (port unavailable)");
			goto out;
		}
		strcpy(active_callid, id);
		state = RINGING; ring_started_ms = 0;
		cid_prepare(event_string(obj, "peeruri"), event_string(obj, "peerdisplayname"));
		leds_ring();
		logmsg("INCOMING -> ringing");
	} else if (!strcmp(type, "CALL_OUTGOING")) {
		if (!strcmp(id, active_callid)) goto out;
		if (!*active_callid && (state == DIALING || state == ENDING)) {
			strcpy(active_callid, id);
			if (state == ENDING) ctrl_cmd(s, "hangup", id);
		} else {
			ctrl_cmd(s, "hangup", id);
		}
	} else if (!strcmp(type, "CALL_ESTABLISHED")) {
		if (*other_callid && !strcmp(id, other_callid) &&
		    (other_kind == OTHER_WAITING || other_kind == OTHER_ANSWERING) &&
		    (state == INCALL || state == OFFHOOK)) {
			/* The waiting call answered by a flash: baresip has put the
			 * one being talked on (if any) on hold. */
			if (*active_callid) {
				strcpy(other_callid, active_callid);
				other_kind = OTHER_HELD;
			} else {
				other_callid[0] = 0; other_kind = OTHER_NONE;
			}
			strcpy(active_callid, id);
			state = INCALL;
			leds_answer(answered_ec);
			logmsg("ESTABLISHED (the waiting call)");
			goto out;
		}
		if (strcmp(id, active_callid) || state == ENDING) {
			ctrl_cmd(s, "hangup", id);
			goto out;
		}
		if (state != DIALING && state != ANSWERING) goto out;
		keypad_stop(); state = INCALL;
		ring(0); ring_on = 0; ring_started_ms = 0;
		leds_answer(answered_ec);
		logmsg("ESTABLISHED");
	} else if (!strcmp(type, "CALL_CLOSED")) {
		if (*other_callid && !strcmp(id, other_callid)) {
			logmsg(other_kind == OTHER_HELD ? "CLOSED (the held call)" :
							  "CLOSED (the waiting call)");
			other_callid[0] = 0; other_kind = OTHER_NONE;
			goto out;
		}
		if (*other_callid && !strcmp(id, active_callid) && state == INCALL) {
			/* The far end of the call being talked on hung up. */
			if (other_kind == OTHER_HELD) {
				strcpy(active_callid, other_callid);
				other_callid[0] = 0; other_kind = OTHER_NONE;
				ctrl_cmd(s, "resume", active_callid);
				logmsg("CLOSED -> the held call resumed");
			} else {
				active_callid[0] = 0;
				state = OFFHOOK;
				logmsg("CLOSED -> a call is waiting: R answers it, hanging up rings it");
			}
			goto out;
		}
		/* Rejected calls do not own local state, even if several CLOSED
		 * events arrive after the user has started a different call. */
		if (strcmp(id, active_callid)) {
			/* An outgoing failure can precede CALL_OUTGOING. */
			if (*active_callid || !direction || strcmp(direction, "outgoing") ||
			    (state != DIALING && state != ENDING)) goto out;
		}
		recall_pending = 0;
		finish_owned_call();
		logmsg(state == IDLE ? "CLOSED -> idle" : "CLOSED -> wait for on-hook");
	}
out:
	json_object_put(obj);
}

/* Where a short break of the loop is a flash rather than a hang-up. */
static int flash_state(void)
{
	return call_waiting && (state == INCALL || (state == OFFHOOK && *other_callid));
}

/* Stable on-hook samples that end the call. With call waiting a break up
 * to flash_max_ms is the R key, so the hang-up waits past it. */
static int onhook_samples(void)
{
	int t = tick_ms();
	long ms = DEBOUNCE_MS;

	if (flash_state() && flash_max_ms + t > ms)
		ms = flash_max_ms + t;
	return (ms + t - 1) / t;
}

/* How long a hook level must hold, from its first sample, before it counts.
 * In wall-clock time, not in samples: poll() also returns early for baresip
 * events, so with traffic on the control socket the samples come faster than
 * the tick, and a short flash could reach the hang-up count. (n - 1) ticks is
 * what n samples took on an undisturbed loop, so that timing is unchanged. */
static long long hook_settle_ms(void)
{
	int n = stable_hk == 0 ? onhook_samples() : debounce_samples();

	return (long long)(n - 1) * tick_ms();
}

static void handle_hook_sample(int s, int hk, int which, int diagnostic_fd)
{
	if (clock_ms() < hook_not_before || hk < 0) {
		stable_cnt = 0; stable_hk = -1; onhook_since = 0; return;
	}
	/* Flash: an on-hook run of flash_min_ms..flash_max_ms that ends
	 * off-hook again before the debounced level ever changed. */
	if (hk == 0 && !onhook_since) {
		onhook_since = clock_ms();
	} else if (hk == 1 && onhook_since) {
		long long ms = clock_ms() - onhook_since;

		onhook_since = 0;
		if (flash_state() && hook_last == 1 &&
		    ms >= flash_min_ms && ms <= flash_max_ms)
			handle_flash(s, ms);
	}
	if (hk == stable_hk) {
		if (stable_cnt < 50) stable_cnt++;
	} else { stable_hk = hk; stable_cnt = 1; stable_since = clock_ms(); }
	if (clock_ms() - stable_since < hook_settle_ms()) return;
	/* The handset may already have been put down before the SIP close.
	 * Thus OFFHOOK release is level based, not dependent on a second edge. */
	if (state == OFFHOOK && stable_hk == 0) {
		baseline = 1; hook_last = 0;
		if (other_kind == OTHER_WAITING) {
			ring_for_other();
			return;
		}
		enter_idle(); logmsg("ONHOOK -> idle"); return;
	}
	if (stable_hk == hook_last) return;
	if (!baseline) {
		baseline = 1; hook_last = stable_hk;
		/* A manager starting with the handset lifted must not auto-answer.
		 * Active feed also rechecks a stale low-power hook sample safely. */
		if (stable_hk == 1) enter_offhook(which);
		return;
	}
	fprintf(stderr, "[callmgr] hook %d->%d (state=%d ec=%d)\n",
		hook_last, stable_hk, state, which);
	fflush(stderr);
	hook_last = stable_hk;
	if (stable_hk == 1) {
		if (state == RINGING && *active_callid) {
			/* baresip opens the same PCM device on accept. */
			cid_stop();
			answered_ec = which; set_slic_ec(which);
			if (ring(0)) {
				/* Keep the dialog unanswered and allow this level to retry
				 * after ring_cadence_tick has confirmed both physical stops. */
				hook_last = 0;
				return;
			}
			ring_on = 0; ring_started_ms = 0;
			state = ANSWERING; leds_answer(which);
			line_active(which);
			if (recall_pending) {
				/* A held call has no answer to wait for. */
				recall_pending = 0;
				ctrl_cmd(s, "resume", active_callid);
				state = INCALL;
				logmsg("ANSWER -> the held call resumed");
			} else {
				ctrl_cmd(s, "accept", active_callid);
			}
		} else if (state == IDLE) {
			int collect;
			answered_ec = which; line_active(which); leds_answer(which);
			collect = keypad_start(s, diagnostic_fd);
			if (collect > 0) { state = COLLECTING; logmsg("KEYPAD -> collecting (30 s maximum)"); }
			else {
				state = OFFHOOK;
				if (!collect) speeddial(s);
				else logmsg("keypad unavailable; hang up to retry");
			}
		} else leds_answer(answered_ec);
	} else if (state == INCALL && (other_kind == OTHER_WAITING || other_kind == OTHER_HELD)) {
		if (*active_callid) ctrl_cmd(s, "hangup", active_callid);
		ring_for_other();
	} else if (state == INCALL || state == DIALING || state == ANSWERING) {
		if (*active_callid) ctrl_cmd(s, "hangup", active_callid);
		/* A waiting call being answered right now goes too. */
		if (*other_callid) ctrl_cmd(s, "hangup", other_callid);
		other_callid[0] = 0; other_kind = OTHER_NONE;
		ring(0); ring_on = 0; ring_started_ms = 0;
		leds_off(); line_standby_both(); state = ENDING;
	} else if (state == IDLE || state == COLLECTING) {
		enter_idle();
	}
}
