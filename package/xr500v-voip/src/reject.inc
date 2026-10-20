/* SPDX-License-Identifier: GPL-2.0-or-later
 * Incoming call rejection with a chosen SIP status. The network turns the
 * status into its own announcement for the caller (RFC 3398 / TS 29.163):
 * 404/604 -> unallocated number, 480 -> subscriber absent, 486 -> busy,
 * 410 -> number changed, 603 -> call rejected.
 *
 * $XR500V_RUNTIME_DIR/reject.conf, rendered by /etc/init.d/voip from UCI and
 * read again on every incoming call, so editing it takes effect at once:
 *   mode off|all|list
 *   code 404
 *   number 2611234567      (one per line, compared on the trailing digits)
 *   number anonymous       (a withheld caller number)
 * Caller numbers are never logged.
 */
static char reject_path[128];

static int reject_code_valid(long code)
{
	return code == 404 || code == 410 || code == 480 || code == 486 ||
	       code == 603 || code == 604;
}

/* Digits of the user part of a sip:/tel: URI, e.g. "+5492611234567". */
static void reject_caller_digits(const char *uri, char *out, size_t len)
{
	const char *p = uri, *colon;
	size_t n = 0;

	if (len)
		*out = 0;
	if (!uri || len < 2)
		return;
	colon = strchr(uri, ':');
	if (colon)
		p = colon + 1;
	for (; *p && *p != '@' && *p != ';' && *p != '>'; p++) {
		if (*p >= '0' && *p <= '9' && n + 1 < len)
			out[n++] = *p;
	}
	out[n] = 0;
}

/* Compare the trailing digits: the national number (area code + subscriber)
 * is 10 digits in Argentina, so "+54 9 261 ...", "0261 ..." and "261 ..."
 * all match. Shorter forms (a local number) compare on what both have. */
static int reject_number_matches(const char *caller, const char *entry)
{
	size_t lc = strlen(caller), le = strlen(entry);
	size_t n = lc < le ? lc : le;

	if (n > 10)
		n = 10;
	if (n < 6)
		return 0;
	return !strcmp(caller + lc - n, entry + le - n);
}

/* The SIP status to reject this caller with, or 0 to let the phone ring.
 * The list entry "anonymous" matches a withheld number (no digits). */
static int reject_lookup(const char *peeruri)
{
	char line[96], caller[48], entry[48];
	int mode = 0, listed = 0;	/* mode: 0 off, 1 all, 2 list */
	long code = 404;
	FILE *f;

	if (!*reject_path || !(f = fopen(reject_path, "r")))
		return 0;
	reject_caller_digits(peeruri, caller, sizeof(caller));
	while (fgets(line, sizeof(line), f)) {
		char *end;

		if (!strncmp(line, "mode ", 5)) {
			mode = !strncmp(line + 5, "all", 3) ? 1 :
			       !strncmp(line + 5, "list", 4) ? 2 : 0;
		} else if (!strncmp(line, "code ", 5)) {
			code = strtol(line + 5, &end, 10);
		} else if (!strncmp(line, "number anonymous", 16) && !listed) {
			listed = !*caller;
		} else if (!strncmp(line, "number ", 7) && !listed) {
			reject_caller_digits(line + 7, entry, sizeof(entry));
			listed = reject_number_matches(caller, entry);
		}
	}
	fclose(f);
	if (!reject_code_valid(code))
		return 0;
	if (mode == 1 || (mode == 2 && listed))
		return (int)code;
	return 0;
}

/* Reject this incoming dialog with @code through the xr500v module. */
static void reject_incoming(int s, const char *id, int code)
{
	char prm[sizeof(active_callid)], msg[64];

	if (snprintf(prm, sizeof(prm), "%s %d", id, code) >= (int)sizeof(prm)) {
		ctrl_cmd(s, "hangup", id);	/* no room: plain 486 */
		code = 486;
	} else {
		ctrl_cmd(s, "xr500v_reject", prm);
	}
	snprintf(msg, sizeof(msg), "INCOMING -> rejected with %d (reject rule)", code);
	logmsg(msg);
}
