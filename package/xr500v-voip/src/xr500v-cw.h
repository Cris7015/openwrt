/* SPDX-License-Identifier: GPL-2.0-or-later
 * Which call the handset is talking on, and whether the call waiting tone
 * is due. Kept apart from xr500v.c so a host test can run it against
 * baresip's event order with mocks (scripts/tests/test_voip_cw_tone.c).
 *
 * Call waiting tone: on while a call rings in and another is being talked
 * on (XR500V_CALL_WAITING=1). Argentina / ETSI: 425 Hz, 0.2 s on, 0.2 s off,
 * 0.2 s on, every 5 s, mixed over the call audio by the playback thread.
 */
#ifndef XR500V_CW_H
#define XR500V_CW_H

#define CW_LEVEL	2000
#define CW_PERIOD	(8000 * 5)
static bool g_cw_enabled;
static RE_ATOMIC bool g_cw;

/* The call the handset is talking on: established and not on hold. */
static struct call *talking_call_except(const struct call *skip)
{
	struct le *le, *lc;

	for (le = list_head(uag_list()); le; le = le->next) {
		for (lc = list_head(ua_calls(le->data)); lc; lc = lc->next) {
			struct call *call = lc->data;

			if (call != skip &&
			    call_state(call) == CALL_STATE_ESTABLISHED &&
			    !call_is_onhold(call))
				return call;
		}
	}
	return NULL;
}

static struct call *talking_call(void)
{
	return talking_call_except(NULL);
}

/* A call the far end closed (BYE, CANCEL) is still listed, in its old
 * state, while UA_EVENT_CALL_CLOSED is emitted (call.c
 * sipsess_close_handler, then ua.c emits and only then drops the call):
 * leave it out, or the tone of a caller who gave up would go on. */
static void update_call_waiting(const struct call *closing)
{
	struct le *le, *lc;
	bool ringing = false;

	for (le = list_head(uag_list()); le; le = le->next)
		for (lc = list_head(ua_calls(le->data)); lc; lc = lc->next)
			if (lc->data != closing &&
			    call_state(lc->data) == CALL_STATE_INCOMING)
				ringing = true;
	re_atomic_rlx_set(&g_cw, g_cw_enabled && ringing &&
			  talking_call_except(closing) != NULL);
}

static void call_event_handler(enum ua_event ev, struct bevent *event, void *arg)
{
	(void)arg;

	switch (ev) {
	case UA_EVENT_CALL_INCOMING:
	case UA_EVENT_CALL_ANSWERED:
	case UA_EVENT_CALL_ESTABLISHED:
	case UA_EVENT_CALL_REMOTE_SDP:
		update_call_waiting(NULL);
		break;
	case UA_EVENT_CALL_CLOSED:
		update_call_waiting(bevent_get_call(event));
		break;
	default:
		break;
	}
}

#endif
