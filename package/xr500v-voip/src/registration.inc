/* SIP registration state of the line's accounts, for the LuCI status page.
 *
 * Baresip reports it in "register" events. They go to <runtime>/phone<N>/
 * registration, one account per line ("aor=... state=... code=... since=..."),
 * replaced atomically. Baresip registers before the call manager connects to
 * ctrl_tcp, so a "reginfo" sent on every (re)connect fills in the accounts
 * whose first event was missed. The file is removed while baresip is gone. */

#define REG_MAX 8

static struct reg_entry {
	char aor[256];
	const char *state;	/* registering, registered, failed, unregistering */
	int code;		/* SIP status, 0 when unknown or a transport error */
	long long since;	/* time() of the last state change */
} regs[REG_MAX];
static int nregs;
static char registration_path[128];

static void reg_write(void)
{
	char tmp[sizeof(registration_path) + 8];
	FILE *f;
	int fd, i;

	if (!*registration_path)
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", registration_path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return;
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(tmp);
		return;
	}
	for (i = 0; i < nregs; i++)
		fprintf(f, "aor=%s\tstate=%s\tcode=%d\tsince=%lld\n",
			regs[i].aor, regs[i].state, regs[i].code, regs[i].since);
	if (fclose(f) || rename(tmp, registration_path))
		unlink(tmp);
}

static void reg_reset(void)
{
	nregs = 0;
	if (*registration_path)
		unlink(registration_path);
}

/* An account AOR as baresip prints it: a SIP URI, printable, no blanks. */
static int reg_aor_ok(const char *aor)
{
	const unsigned char *p = (const unsigned char *)aor;

	if (!aor || strncmp(aor, "sip", 3) || strlen(aor) >= sizeof(regs[0].aor))
		return 0;
	for (; *p; p++)
		if (*p <= 32 || *p >= 127)
			return 0;
	return 1;
}

static void reg_set(const char *aor, const char *state, int code)
{
	int i;

	for (i = 0; i < nregs && strcmp(regs[i].aor, aor); i++)
		;
	if (i == nregs) {
		if (nregs == REG_MAX)
			return;
		strcpy(regs[nregs++].aor, aor);
		regs[i].state = NULL;
	}
	/* A refresh of a registered account starts with REGISTERING: keep
	 * showing it registered until the refresh fails. */
	if (!strcmp(state, "registering") && regs[i].state &&
	    !strcmp(regs[i].state, "registered"))
		return;
	if (regs[i].state && !strcmp(regs[i].state, state) && regs[i].code == code)
		return;
	regs[i].state = state;
	regs[i].code = code;
	regs[i].since = time(NULL);
	reg_write();
}

/* "200 OK", "401 Unauthorized": the SIP status. Transport errors ("Connection
 * timed out [110]") carry none. */
static int reg_param_code(const char *param)
{
	if (param && param[0] >= '1' && param[0] <= '6' &&
	    param[1] >= '0' && param[1] <= '9' && param[2] >= '0' && param[2] <= '9' &&
	    (param[3] == ' ' || !param[3]))
		return (param[0] - '0') * 100 + (param[1] - '0') * 10 + (param[2] - '0');
	return 0;
}

static void reg_event(const char *type, const char *aor, const char *param)
{
	const char *state;

	if (!type || !reg_aor_ok(aor))
		return;
	if (!strcmp(type, "REGISTERING"))
		state = "registering";
	else if (!strcmp(type, "REGISTER_OK"))
		state = "registered";
	else if (!strcmp(type, "REGISTER_FAIL"))
		state = "failed";
	else if (!strcmp(type, "UNREGISTERING"))
		state = "unregistering";
	else
		return;	/* FALLBACK_*: the backup outbound proxy, not the account */
	reg_set(aor, state, reg_param_code(param));
}

/* reginfo: "<n> - <aor> <status>..." per account, each registration of it
 * printing "OK " (200), "ERR" (any other status) or "zzz" (none yet) between
 * colour escapes. An account that does not register prints no status. Right
 * after start "ERR" is usually the authentication challenge of a registration
 * still in progress: show it as registering, the next event settles it. */
static void reg_reginfo(const char *data)
{
	char line[512], aor[256];
	const char *p = data, *end;

	while (p && *p) {
		size_t n;
		const char *rest;

		end = strchr(p, '\n');
		n = end ? (size_t)(end - p) : strlen(p);
		if (n < sizeof(line)) {
			memcpy(line, p, n);
			line[n] = 0;
			if (sscanf(line, "%*u - %255s", aor) == 1 && reg_aor_ok(aor) &&
			    (rest = strstr(line, aor))) {
				rest += strlen(aor);
				if (strstr(rest, "OK "))
					reg_set(aor, "registered", 200);
				else if (strstr(rest, "ERR") || strstr(rest, "zzz"))
					reg_set(aor, "registering", 0);
			}
		}
		p = end ? end + 1 : NULL;
	}
}
