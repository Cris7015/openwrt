/* SPDX-License-Identifier: GPL-2.0-or-later
 * Numbers are never logged: before a baresip response goes to the log, the
 * user part of every sip:/sips:/tel: URI is replaced by "*". A host test
 * runs it (scripts/tests/test_voip_redact.c).
 */
#ifndef XR500V_REDACT_H
#define XR500V_REDACT_H

#include <stddef.h>
#include <string.h>

/* Copies m into out (always NUL-terminated, truncated to size). */
static void redact_uris(char *out, size_t size, const char *m)
{
	static const char *const schemes[] = { "sips:", "sip:", "tel:" };
	size_t o = 0, i;

	if (!size)
		return;
	while (*m && o + 1 < size) {
		for (i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
			size_t s = strlen(schemes[i]), n;

			if (strncmp(m, schemes[i], s))
				continue;
			/* tel: is all user part; sip(s): only before an '@' */
			n = strcspn(m + s, "@ \t\r\n<>;,\"");
			if (!n || (i < 2 && m[s + n] != '@'))
				break;
			for (; s && o + 1 < size; s--)
				out[o++] = *m++;
			if (o + 1 < size)
				out[o++] = '*';
			m += n;
			break;
		}
		if (*m && o + 1 < size)
			out[o++] = *m++;
	}
	out[o] = 0;
}

#endif
