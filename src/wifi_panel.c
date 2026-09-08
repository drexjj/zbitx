/*
 * wifi_panel.c — front-panel wireless-LAN management via NetworkManager.
 *
 * See wifi_panel.h for the overall design. Short version: panel sends
 * "WIFI ..." commands, we run the matching nmcli in a background thread so the
 * I2C poll never blocks, stash the human-readable result, and let the main
 * thread push it to the panel from wifi_panel_poll().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>

#include "wifi_panel.h"
#include "i2cbb.h"

/* The front panel lives at this I2C address (matches ZBITX_I2C_ADDRESS in
 * sbitx_gtk.c). We only ever write from the main thread via wifi_panel_poll. */
#define WIFI_PANEL_I2C_ADDRESS 0x0a

/* Wireless interface. Bookworm's default onboard interface is wlan0. */
#define WIFI_IFACE "wlan0"

/*
 * The zBitx app runs as the unprivileged `pi` user (see start.sh) and relies
 * on passwordless sudo for privileged actions, exactly like the existing
 * `sudo /sbin/shutdown` calls in sbitx_gtk.c. NetworkManager lets the active
 * console user *read* status without privilege, but changing state (forcing a
 * rescan, connecting, disconnecting, deleting a profile) needs privilege. So
 * we split nmcli into two prefixes:
 *   NMCLI_RO — read-only queries, run as the normal user.
 *   NMCLI    — state-changing commands, run via sudo.
 * If a given install has polkit rules that already grant the pi user these
 * actions, sudo is harmless; if not, sudo is what makes them work. Requires
 * the pi user to have NOPASSWD sudo (default on Raspberry Pi OS), same
 * assumption the shutdown path already makes.
 */
#define NMCLI_RO "nmcli"
#define NMCLI    "sudo nmcli"

/* Panel field labels we drive (defined in the Pico's fields_list.h). */
#define WIFI_STATUS_LABEL "WIFI_STAT" /* one-line status (SSID / IP / signal) */
#define WIFI_LIST_LABEL   "WIFI_LIST" /* multi-line scan result (a text box)  */

/* ------------------------------------------------------------------------- */
/* Shared state between the worker thread and the main thread.               */
/* ------------------------------------------------------------------------- */

/* Pending text the main thread still needs to push to the panel. Each is
 * guarded by the mutex; a non-empty buffer means "there is something to send".
 * We keep the status and list buffers separate so a slow scan result and a
 * quick status update don't clobber one another. */
/* The panel's WIFI_STAT / WIFI_LIST fields are plain FIELD_STATIC boxes whose
 * value buffer is FIELD_TEXT_MAX_LENGTH (128) bytes on the Pico, and a single
 * bit-banged I2C block is capped at 255 bytes. So both of these must stay well
 * under 128 chars of actual text. We build a compact, bounded list. */
#define WIFI_STATUS_MAX 120
#define WIFI_LIST_MAX   128

static pthread_mutex_t wifi_lock = PTHREAD_MUTEX_INITIALIZER;
static char pending_status[WIFI_STATUS_MAX];
static char pending_list[WIFI_LIST_MAX];
static int  status_dirty = 0;
static int  list_dirty = 0;

/* Only allow one worker at a time; a second "scan" while one is running is
 * ignored rather than spawning overlapping nmcli processes. */
static volatile int worker_busy = 0;

/* ------------------------------------------------------------------------- */
/* Small helpers.                                                            */
/* ------------------------------------------------------------------------- */

static void set_status(const char *text)
{
	pthread_mutex_lock(&wifi_lock);
	strncpy(pending_status, text, WIFI_STATUS_MAX - 1);
	pending_status[WIFI_STATUS_MAX - 1] = 0;
	status_dirty = 1;
	pthread_mutex_unlock(&wifi_lock);
}

static void set_list(const char *text)
{
	pthread_mutex_lock(&wifi_lock);
	strncpy(pending_list, text, WIFI_LIST_MAX - 1);
	pending_list[WIFI_LIST_MAX - 1] = 0;
	list_dirty = 1;
	pthread_mutex_unlock(&wifi_lock);
}

/* Escape a value so it is safe to hand to a shell inside single quotes.
 * Turns   foo'bar   into   'foo'\''bar'   so word-splitting and metacharacters
 * in SSIDs / passphrases can't break out of the quoting. dst must hold at
 * least 4*len+3 bytes. */
static void shell_single_quote(const char *src, char *dst, size_t dstsz)
{
	size_t j = 0;
	if (dstsz < 3)
	{
		if (dstsz)
			dst[0] = 0;
		return;
	}
	dst[j++] = '\'';
	for (const char *p = src; *p && j + 5 < dstsz; p++)
	{
		if (*p == '\'')
		{
			/* close quote, escaped quote, reopen quote */
			dst[j++] = '\'';
			dst[j++] = '\\';
			dst[j++] = '\'';
			dst[j++] = '\'';
		}
		else
			dst[j++] = *p;
	}
	dst[j++] = '\'';
	dst[j] = 0;
}

/* Trim trailing whitespace/newline in place. */
static void rstrip(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
				 s[n - 1] == ' '  || s[n - 1] == '\t'))
		s[--n] = 0;
}

/* ------------------------------------------------------------------------- */
/* nmcli operations (run on the worker thread).                              */
/* ------------------------------------------------------------------------- */

/* Build the one-line status: connected SSID, IP address, and signal %. */
static void do_status(void)
{
	char ssid[64] = "";
	char ip[64] = "";
	char signal[16] = "";
	char line[256];
	FILE *pf;

	/* Active Wi-Fi connection SSID + signal, terminal-mode (colon separated).
	 * `nmcli -t -f ACTIVE,SSID,SIGNAL dev wifi` lists the visible networks;
	 * the active one is marked "yes". */
	pf = popen(NMCLI_RO " -t -f ACTIVE,SSID,SIGNAL dev wifi 2>/dev/null", "r");
	if (pf)
	{
		while (fgets(line, sizeof(line), pf))
		{
			rstrip(line);
			if (!strncmp(line, "yes:", 4))
			{
				/* format: yes:<ssid>:<signal> — but SSID could contain ':'
				 * (rare). Take the first field after "yes:" up to the LAST
				 * colon as SSID, remainder as signal. */
				char *body = line + 4;
				char *last = strrchr(body, ':');
				if (last)
				{
					*last = 0;
					strncpy(ssid, body, sizeof(ssid) - 1);
					strncpy(signal, last + 1, sizeof(signal) - 1);
				}
				else
					strncpy(ssid, body, sizeof(ssid) - 1);
				break;
			}
		}
		pclose(pf);
	}

	/* IPv4 address on the wireless interface. */
	pf = popen(NMCLI_RO " -t -f IP4.ADDRESS dev show " WIFI_IFACE " 2>/dev/null", "r");
	if (pf)
	{
		if (fgets(line, sizeof(line), pf))
		{
			rstrip(line);
			/* format: IP4.ADDRESS[1]:192.168.1.23/24 */
			char *colon = strchr(line, ':');
			if (colon)
			{
				char *slash = strchr(colon + 1, '/');
				if (slash)
					*slash = 0;
				strncpy(ip, colon + 1, sizeof(ip) - 1);
			}
		}
		pclose(pf);
	}

	/* Keep the SSID short in the status line so the whole "ssid  ip  sig%"
	 * string fits the panel's box without truncation. */
	char ssid_short[40];
	strncpy(ssid_short, ssid, sizeof(ssid_short) - 1);
	ssid_short[sizeof(ssid_short) - 1] = 0;

	/* An IPv4 address is at most 15 chars; bound the buffer view so the
	 * status-line snprintf is provably in range. */
	char ip_short[16];
	strncpy(ip_short, ip, sizeof(ip_short) - 1);
	ip_short[sizeof(ip_short) - 1] = 0;

	char sig_short[8];
	strncpy(sig_short, strlen(signal) ? signal : "?", sizeof(sig_short) - 1);
	sig_short[sizeof(sig_short) - 1] = 0;

	char out[WIFI_STATUS_MAX];
	if (strlen(ssid))
	{
		if (strlen(ip_short))
			snprintf(out, sizeof(out), "%s  %s  %s%%",
					 ssid_short, ip_short, sig_short);
		else
			snprintf(out, sizeof(out), "%s  (no IP yet)", ssid_short);
	}
	else
		snprintf(out, sizeof(out), "Not connected");

	set_status(out);
}

/* Scan for networks and build a newline-separated list of
 * "signal%  SSID  security" rows, strongest first. */
static void do_scan(void)
{
	char line[256];
	char list[WIFI_LIST_MAX];
	FILE *pf;

	set_status("Scanning...");

	/* Ask NetworkManager to rescan, then list. --rescan yes forces fresh
	 * results rather than the cached table. -t = terminal (colon-separated),
	 * -f picks the fields and their order. */
	pf = popen(NMCLI " --wait 8 -t -f SIGNAL,SSID,SECURITY dev wifi list "
			   "--rescan yes 2>/dev/null | sort -t: -k1 -nr",
			   "r");
	if (!pf)
	{
		set_list("Scan failed");
		set_status("Scan failed");
		return;
	}

	/* Build a compact list that fits the panel's 128-byte static box. Each row
	 * is "<lock><ssid>\n" where <lock> is '*' for a secured network and ' '
	 * for open. We show the strongest few and de-duplicate repeated SSIDs
	 * (multiple APs / bands broadcast the same name). */
	list[0] = 0;
	int shown = 0;
	char seen[8][40];
	int nseen = 0;

	while (fgets(line, sizeof(line), pf))
	{
		rstrip(line);
		if (!strlen(line))
			continue;

		/* format: <signal>:<ssid>:<security>  (ssid may contain ':') */
		char *first = strchr(line, ':');
		if (!first)
			continue;
		*first = 0;
		char *rest = first + 1;
		char *last = strrchr(rest, ':');
		char *ssid = rest;
		char *sec = (char *)"";
		if (last)
		{
			*last = 0;
			sec = last + 1;
		}

		if (!strlen(ssid))
			continue; /* hidden network */

		/* de-dup against SSIDs already shown */
		int dup = 0;
		for (int k = 0; k < nseen; k++)
			if (!strcmp(seen[k], ssid))
			{
				dup = 1;
				break;
			}
		if (dup)
			continue;
		if (nseen < 8)
		{
			strncpy(seen[nseen], ssid, sizeof(seen[0]) - 1);
			seen[nseen][sizeof(seen[0]) - 1] = 0;
			nseen++;
		}

		/* Truncate long SSIDs so more rows fit the small box. */
		char ssid_short[22];
		strncpy(ssid_short, ssid, sizeof(ssid_short) - 1);
		ssid_short[sizeof(ssid_short) - 1] = 0;

		char row[32];
		snprintf(row, sizeof(row), "%c%s\n",
				 (strlen(sec) ? '*' : ' '), ssid_short);

		/* Stop before we overflow the 128-byte panel field. */
		if (strlen(list) + strlen(row) >= WIFI_LIST_MAX - 1)
			break;
		strcat(list, row);
		shown++;
		if (shown >= 7) /* the box holds ~8 short rows */
			break;
	}
	pclose(pf);

	if (!shown)
		strcpy(list, "No networks found");

	set_list(list);
	/* Refresh the status line too, so the user sees where they stand. */
	do_status();
}

/* Connect to <ssid> with optional <psk>. If a saved profile for the SSID
 * already exists, bring it up; otherwise create a new one. */
static void do_connect(const char *ssid, const char *psk)
{
	char qssid[256], qpsk[256];
	char cmd[900];
	int rc;

	if (!ssid || !strlen(ssid))
	{
		set_status("No SSID given");
		return;
	}

	set_status("Connecting...");

	shell_single_quote(ssid, qssid, sizeof(qssid));

	/* `nmcli dev wifi connect` both creates the profile (if new) and
	 * activates it, and updates the password on an existing profile. */
	if (psk && strlen(psk))
	{
		shell_single_quote(psk, qpsk, sizeof(qpsk));
		snprintf(cmd, sizeof(cmd),
				 NMCLI " --wait 25 dev wifi connect %s password %s "
				 "ifname " WIFI_IFACE " 2>&1",
				 qssid, qpsk);
	}
	else
	{
		snprintf(cmd, sizeof(cmd),
				 NMCLI " --wait 25 dev wifi connect %s "
				 "ifname " WIFI_IFACE " 2>&1",
				 qssid);
	}

	FILE *pf = popen(cmd, "r");
	char reply[256] = "";
	if (pf)
	{
		char line[256];
		while (fgets(line, sizeof(line), pf))
		{
			rstrip(line);
			if (strlen(line))
				strncpy(reply, line, sizeof(reply) - 1); /* keep last line */
		}
		rc = pclose(pf);
	}
	else
		rc = -1;

	/* On success nmcli prints "Device 'wlan0' successfully activated...".
	 * Rather than parse locale-specific text, just re-read real status. */
	if (rc == 0)
		do_status();
	else
	{
		/* Trim the nmcli error to what fits the status box. */
		char reply_short[96];
		strncpy(reply_short, reply, sizeof(reply_short) - 1);
		reply_short[sizeof(reply_short) - 1] = 0;

		char out[WIFI_STATUS_MAX];
		if (strlen(reply_short))
			snprintf(out, sizeof(out), "Failed: %s", reply_short);
		else
			snprintf(out, sizeof(out), "Connect failed");
		set_status(out);
	}
}

static void do_disconnect(void)
{
	set_status("Disconnecting...");
	int rc = system(NMCLI " dev disconnect " WIFI_IFACE " >/dev/null 2>&1");
	(void)rc;
	do_status();
}

static void do_forget(const char *ssid)
{
	char qssid[256], cmd[600];
	if (!ssid || !strlen(ssid))
	{
		set_status("No SSID given");
		return;
	}
	shell_single_quote(ssid, qssid, sizeof(qssid));
	/* Delete the saved connection profile whose name matches the SSID.
	 * nmcli names auto-created profiles after the SSID by default. */
	snprintf(cmd, sizeof(cmd),
			 NMCLI " connection delete id %s >/dev/null 2>&1", qssid);
	int rc = system(cmd);
	(void)rc;
	do_status();
}

/* ------------------------------------------------------------------------- */
/* Worker thread plumbing.                                                   */
/* ------------------------------------------------------------------------- */

/* One heap-allocated job description handed to the worker. */
struct wifi_job
{
	char op[16];    /* "scan" / "status" / "connect" / "disconnect" / "forget" */
	char ssid[64];
	char psk[128];
};

static void *wifi_worker(void *arg)
{
	struct wifi_job *job = (struct wifi_job *)arg;

	if (!strcmp(job->op, "scan"))
		do_scan();
	else if (!strcmp(job->op, "status"))
		do_status();
	else if (!strcmp(job->op, "connect"))
		do_connect(job->ssid, job->psk);
	else if (!strcmp(job->op, "disconnect"))
		do_disconnect();
	else if (!strcmp(job->op, "forget"))
		do_forget(job->ssid);

	free(job);
	worker_busy = 0;
	return NULL;
}

static void start_job(struct wifi_job *job)
{
	if (worker_busy)
	{
		/* Something is already running. For status/scan just drop the
		 * duplicate; the in-flight one will refresh the panel shortly. */
		free(job);
		return;
	}
	worker_busy = 1;

	pthread_t t;
	if (pthread_create(&t, NULL, wifi_worker, job) != 0)
	{
		worker_busy = 0;
		free(job);
		set_status("WiFi busy");
		return;
	}
	pthread_detach(t);
}

/* ------------------------------------------------------------------------- */
/* Public API.                                                               */
/* ------------------------------------------------------------------------- */

void wifi_panel_init(void)
{
	pending_status[0] = 0;
	pending_list[0] = 0;
	status_dirty = 0;
	list_dirty = 0;
	worker_busy = 0;
}

void wifi_panel_command(const char *args)
{
	if (!args)
		return;

	/* Skip leading spaces. */
	while (*args == ' ')
		args++;

	/* First token is the sub-command. */
	char op[16];
	int i = 0;
	while (args[i] && args[i] != ' ' && i < (int)sizeof(op) - 1)
	{
		op[i] = tolower((unsigned char)args[i]);
		i++;
	}
	op[i] = 0;
	const char *rest = args + i;
	while (*rest == ' ')
		rest++;

	struct wifi_job *job = calloc(1, sizeof(*job));
	if (!job)
		return;

	if (!strcmp(op, "scan"))
	{
		strcpy(job->op, "scan");
		start_job(job);
	}
	else if (!strcmp(op, "status"))
	{
		strcpy(job->op, "status");
		start_job(job);
	}
	else if (!strcmp(op, "disconnect"))
	{
		strcpy(job->op, "disconnect");
		start_job(job);
	}
	else if (!strcmp(op, "forget"))
	{
		strcpy(job->op, "forget");
		strncpy(job->ssid, rest, sizeof(job->ssid) - 1);
		rstrip(job->ssid);
		start_job(job);
	}
	else if (!strcmp(op, "connect"))
	{
		strcpy(job->op, "connect");
		/* rest is "<ssid>\t<psk>" — split on the TAB. The panel always
		 * sends a TAB even for open networks (psk empty), but tolerate its
		 * absence (whole remainder = ssid, no psk). */
		char *tab = strchr(rest, '\t');
		if (tab)
		{
			int n = (int)(tab - rest);
			if (n > (int)sizeof(job->ssid) - 1)
				n = sizeof(job->ssid) - 1;
			memcpy(job->ssid, rest, n);
			job->ssid[n] = 0;
			strncpy(job->psk, tab + 1, sizeof(job->psk) - 1);
		}
		else
			strncpy(job->ssid, rest, sizeof(job->ssid) - 1);
		rstrip(job->ssid);
		rstrip(job->psk);
		start_job(job);
	}
	else
	{
		/* Unknown sub-command; nothing to do. */
		free(job);
	}
}

/* Push one {LABEL value} block to the panel. Mirrors the helper style used in
 * sbitx_gtk.c: the block is "<LABEL> <value>}" written with '{' as the command
 * byte. Called on the main thread only. */
static void panel_push(const char *label, const char *value)
{
	char buff[260];
	/* Keep within the 255-byte block ceiling of the bit-banged bus. */
	snprintf(buff, sizeof(buff), "%s %s}", label, value);
	i2cbb_write_i2c_block_data(WIFI_PANEL_I2C_ADDRESS, '{',
							   (uint8_t)strlen(buff), (const uint8_t *)buff);
}

void wifi_panel_poll(void)
{
	char status_copy[WIFI_STATUS_MAX];
	char list_copy[WIFI_LIST_MAX];
	int have_status = 0, have_list = 0;

	pthread_mutex_lock(&wifi_lock);
	if (status_dirty)
	{
		strcpy(status_copy, pending_status);
		status_dirty = 0;
		have_status = 1;
	}
	if (list_dirty)
	{
		strcpy(list_copy, pending_list);
		list_dirty = 0;
		have_list = 1;
	}
	pthread_mutex_unlock(&wifi_lock);

	if (have_status)
		panel_push(WIFI_STATUS_LABEL, status_copy);

	if (have_list)
	{
		/* The scan builder bounds the list to fit both the single 255-byte
		 * I2C block and the panel's 128-byte static field, so one push is
		 * always enough. */
		panel_push(WIFI_LIST_LABEL, list_copy);
	}
}
