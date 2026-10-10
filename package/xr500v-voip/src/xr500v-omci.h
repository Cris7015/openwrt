/* SPDX-License-Identifier: GPL-2.0-or-later
 * Tell the GPON OMCI agent how many calls are in progress, so that an OLT
 * Reboot that must not drop a call is answered "device busy" instead.
 *
 * Each baresip instance (one per phone line with its own account) keeps
 * its own count in VOICE_CALLS_DIR, named after its configuration
 * directory, and writes the sum of all of them to the agent. An instance
 * writes its file before it adds the files up, so the last one to report
 * always sees the other's latest count.
 */
#ifndef XR500V_OMCI_H
#define XR500V_OMCI_H

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

#define VOICE_CALLS_DIR		"/var/run/xr500v-voice-calls"
#define OMCI_VOICE_CALLS	"/sys/class/xpon/pon0/omci/voice_calls"

static char g_calls_file[256];
static int g_calls_last = -1;

/* Calls of this instance that are in progress, ringing ones included. */
static unsigned int calls_in_progress_except(const struct call *closing)
{
	struct le *le, *lc;
	unsigned int n = 0;

	for (le = list_head(uag_list()); le; le = le->next) {
		for (lc = list_head(ua_calls(le->data)); lc; lc = lc->next) {
			const struct call *call = lc->data;

			if (call != closing &&
			    call_state(call) != CALL_STATE_TERMINATED)
				n++;
		}
	}
	return n;
}

static unsigned int voice_calls_total(void)
{
	struct dirent *de;
	unsigned int total = 0, n;
	char path[512];
	DIR *dir;
	FILE *f;

	dir = opendir(VOICE_CALLS_DIR);
	if (!dir)
		return 0;
	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", VOICE_CALLS_DIR,
			 de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fscanf(f, "%u", &n) == 1)
			total += n;
		fclose(f);
	}
	closedir(dir);
	return total;
}

static void voice_calls_publish(unsigned int n)
{
	unsigned int total;
	FILE *f;

	if (!g_calls_file[0] || (int)n == g_calls_last)
		return;
	g_calls_last = n;

	f = fopen(g_calls_file, "w");
	if (f) {
		fprintf(f, "%u\n", n);
		fclose(f);
	}

	total = voice_calls_total();
	/* No xPON or no OMCI agent: there is nobody to tell. */
	f = fopen(OMCI_VOICE_CALLS, "w");
	if (!f)
		return;
	fprintf(f, "%u\n", total > 255 ? 255 : total);
	fclose(f);
}

static void voice_calls_report(const struct call *closing)
{
	voice_calls_publish(calls_in_progress_except(closing));
}

static void voice_calls_event_handler(enum ua_event ev, struct bevent *event,
				      void *arg)
{
	(void)arg;

	switch (ev) {
	case UA_EVENT_CALL_INCOMING:
	case UA_EVENT_CALL_OUTGOING:
	case UA_EVENT_CALL_RINGING:
	case UA_EVENT_CALL_PROGRESS:
	case UA_EVENT_CALL_ANSWERED:
	case UA_EVENT_CALL_ESTABLISHED:
		voice_calls_report(NULL);
		break;
	case UA_EVENT_CALL_CLOSED:
		/* The closing call is still listed while this event runs. */
		voice_calls_report(bevent_get_call(event));
		break;
	default:
		break;
	}
}

static int voice_calls_init(void)
{
	char path[256];
	const char *name;

	if (conf_path_get(path, sizeof(path)))
		return 0;
	name = strrchr(path, '/');
	name = name && name[1] ? name + 1 : path;
	if (mkdir(VOICE_CALLS_DIR, 0755) && errno != EEXIST)
		return 0;
	snprintf(g_calls_file, sizeof(g_calls_file), "%s/%s",
		 VOICE_CALLS_DIR, name);
	/* A restarted instance may have left a count behind. */
	g_calls_last = -1;
	voice_calls_publish(0);
	return bevent_register(voice_calls_event_handler, NULL);
}

static void voice_calls_close(void)
{
	bevent_unregister(voice_calls_event_handler);
	g_calls_last = -1;
	voice_calls_publish(0);
}

#endif
