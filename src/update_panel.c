/*
 * update_panel.c — software update from the front panel (MENU -> UPDATE).
 *
 * See update_panel.h for the design. Short version: git and the build run as
 * detached shell scripts that leave their results in files; the main thread
 * polls those files and pushes versions, progress and the final state to the
 * panel's UPD_* fields.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "update_panel.h"
#include "i2cbb.h"

/* Same address as ZBITX_I2C_ADDRESS in sbitx_main.c. */
#define UPDATE_PANEL_I2C_ADDRESS 0x0a

/* The install the update script works on. Overridable for bench testing. */
#ifndef UPD_SBITX_DIR
#define UPD_SBITX_DIR "/home/pi/sbitx"
#endif
#ifndef UPD_HOME
#define UPD_HOME "/home/pi"
#endif
#ifndef UPD_REBOOT_CMD
#define UPD_REBOOT_CMD "sudo /sbin/reboot"
#endif

/* Working files live OUTSIDE the git checkout: the update script runs
 * `git stash --include-untracked`, which would stash (and remove) anything
 * untracked inside ~/sbitx, including a log it is still writing. */
#define UPD_WORK_DIR    UPD_HOME "/.zbitx_update"
#define UPD_CHECK_SH    UPD_WORK_DIR "/check.sh"
#define UPD_CHECK_FILE  UPD_WORK_DIR "/check.txt"
#define UPD_RUN_SH      UPD_WORK_DIR "/run.sh"
#define UPD_RESULT_FILE UPD_WORK_DIR "/result"
#define UPD_LOG_FILE    UPD_WORK_DIR "/update.log"
#define UPD_PANEL_DIR   UPD_WORK_DIR "/panel"            /* zbitxfrontpanel clone */
#define UPD_PANEL_VER   UPD_WORK_DIR "/panel_installed"  /* from the panel */
#ifndef UPD_PANEL_REPO
#define UPD_PANEL_REPO  "https://github.com/drexjj/zbitxfrontpanel"
#endif

/* Shell: bring the front-panel repo up to date (shallow) in UPD_PANEL_DIR,
 * then set $u to the newest .uf2 in its build folder. Panel releases are published there
 * as zbitxfrontpanel.v<version>.uf2, so a version sort picks the latest. */
#define PANEL_FETCH_SH \
	"P=" UPD_PANEL_DIR "\n" \
	"if [ -d $P/.git ]; then\n" \
	"  (cd $P && timeout 120 git fetch -q --depth 1 origin main && git reset -q --hard FETCH_HEAD)\n" \
	"else\n" \
	"  rm -rf $P && timeout 120 git clone -q --depth 1 " UPD_PANEL_REPO " $P\n" \
	"fi\n" \
	"pf=$?\n" \
	"u=$(ls $P/build/*.uf2 2>/dev/null | sort -V | tail -n 1)\n"
/* How the log path is shown on the panel. */
#define UPD_LOG_SHOWN   "~/.zbitx_update/update.log"

/* Fetch the branch this install is on and report the version it would get,
 * and the newest published front-panel firmware. Writes one line,
 * "OK|<version>|<commits behind>|<panel>" or "ERR", via a temp file so the
 * poller never reads a half-written result. <panel> is the panel version,
 * "-" if the published panel image can't be installed over I2C, or empty if
 * unknown (no download, or no zbitx-panel-flash yet). */
static const char *check_script =
	"#!/bin/sh\n"
	"cd " UPD_SBITX_DIR " || { echo ERR > " UPD_CHECK_FILE "; exit 0; }\n"
	"t=" UPD_CHECK_FILE ".tmp\n"
	"b=$(git rev-parse --abbrev-ref HEAD)\n"
	"if timeout 60 git fetch -q origin \"$b\" >/dev/null 2>&1; then\n"
	"  v=$(git show FETCH_HEAD:src/sdr_ui.h | grep 'define VER_STR' | head -n 1 | cut -d'\"' -f2)\n"
	"  n=$(git rev-list --count HEAD..FETCH_HEAD)\n"
	PANEL_FETCH_SH
	"  pv=''\n"
	"  if [ -n \"$u\" ] && [ -x ./zbitx-panel-flash ]; then\n"
	"    pv=$(./zbitx-panel-flash --info \"$u\" 2>/dev/null) || pv=-\n"
	"  fi\n"
	"  echo \"OK|${v:-unknown}|${n:-?}|$pv\" > $t\n"
	"else\n"
	"  echo ERR > $t\n"
	"fi\n"
	"mv -f $t " UPD_CHECK_FILE "\n";

/* The whole update, run detached:
 *  1. the stock update script (git pull + build), unless the branch on
 *     GitHub has the same VER_STR as the installed source and a build is in
 *     place: then nothing is rebuilt. On failure: FAIL, or NOBIN if the
 *     build failed after `make clean` removed the binary (the radio would
 *     not start again if it were restarted now).
 *  2. the front panel: fetch the newest published panel firmware; if it can
 *     be installed over I2C and differs from what the panel runs, stop the
 *     zBitx app (it owns the panel's I2C traffic) and send it with
 *     zbitx-panel-flash, which also tells the panel's lock screen when the
 *     panel isn't flashed. Otherwise "OK <reason>" tells the app, still
 *     running, why the panel was left alone.
 *  3. reboot, unless nothing was rebuilt or flashed: then "NOCHANGE <reason>"
 *     tells the app to unlock the panel and carry on.
 * The result file drives what the app shows (see update_panel_poll()). */
static const char *run_script =
	"#!/bin/sh\n"
	"R=" UPD_RESULT_FILE "\n"
	"L=" UPD_LOG_FILE "\n"
	"cd " UPD_SBITX_DIR " || { echo FAIL > $R; exit 0; }\n"
	/* Same version on GitHub as installed? Then skip the rebuild. */
	"b=$(git rev-parse --abbrev-ref HEAD); [ \"$b\" = HEAD ] && b=main\n"
	"cur=$(grep 'define VER_STR' src/sdr_ui.h | head -n 1 | cut -d'\"' -f2)\n"
	"new=''\n"
	"if timeout 60 git fetch -q origin \"$b\" > $L 2>&1; then\n"
	"  new=$(git show FETCH_HEAD:src/sdr_ui.h | grep 'define VER_STR' | head -n 1 | cut -d'\"' -f2)\n"
	"fi\n"
	"built=1\n"
	"if [ -n \"$cur\" ] && [ \"$new\" = \"$cur\" ] && [ -x zbitx ] && [ -x zbitx-panel-flash ]; then\n"
	"  echo \"SKIPBUILD: $cur is already installed; not rebuilding\" >> $L\n"
	"  built=0\n"
	"elif ! HOME=" UPD_HOME " ./update >> $L 2>&1; then\n"
	"  if [ -x zbitx ]; then echo FAIL > $R; else echo NOBIN > $R; fi\n"
	"  exit 0\n"
	"fi\n"
	/* Panel left alone: reboot only if something was rebuilt. */
	"finish() {\n"
	"  if [ $built -eq 0 ]; then echo \"NOCHANGE $1\" > $R; exit 0; fi\n"
	"  echo \"OK $1\" > $R; sleep 5; " UPD_REBOOT_CMD "; exit 0\n"
	"}\n"
	"echo 'PANEL: getting the front panel firmware' >> $L\n"
	PANEL_FETCH_SH
	"[ $pf -eq 0 ] || finish nodownload\n"
	"[ -n \"$u\" ] || finish nofile\n"
	"[ -x ./zbitx-panel-flash ] || finish notool\n"
	"nv=$(./zbitx-panel-flash --info \"$u\" 2>>$L) || finish notota\n"
	"cv=$(cat " UPD_PANEL_VER " 2>/dev/null)\n"
	"[ \"$nv\" = \"$cv\" ] && finish \"current $nv\"\n"
	"echo \"PANEL: updating the front panel to $nv\" >> $L\n"
	"echo \"PANEL $built $nv\" > $R\n"
	"sleep 3\n"
	"pkill -x zbitx\n"
	"i=0; while pgrep -x zbitx >/dev/null && [ $i -lt 20 ]; do sleep 1; i=$((i+1)); done\n"
	"pkill -9 -x zbitx 2>/dev/null\n"
	"./zbitx-panel-flash --yes --if-different --report \"$u\" >> $L 2>&1\n"
	"echo \"PANEL: zbitx-panel-flash exit $?\" >> $L\n"
	"sleep 3\n"
	UPD_REBOOT_CMD "\n";

static int check_pending = 0;
static char panel_installed[24] = "";   /* as reported by the panel */
static char app_version[40] = "";       /* this app's VER_STR */
static long unlock_at_ms = 0;           /* NOCHANGE: when to unlock the panel */
static long check_started_ms = 0;
static int running = 0;
static long run_started_ms = 0;
static long last_poll_ms = 0;
static long last_beat_ms = 0;

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Push one {LABEL value} block. Braces would end the block early on the
 * panel, and the panel's static fields hold 127 characters, so clean and
 * bound the value. Main thread only. */
static void panel_push(const char *label, const char *value)
{
	char clean[124];
	int i = 0;
	for (const char *p = value; *p && i < (int)sizeof(clean) - 1; p++) {
		char c = *p;
		if (c == '{' || c == '}' || ((unsigned char)c < 32 && c != '\n'))
			c = ' ';
		clean[i++] = c;
	}
	clean[i] = 0;

	char buff[160];
	snprintf(buff, sizeof(buff), "%s %s}", label, clean);
	i2cbb_write_i2c_block_data(UPDATE_PANEL_I2C_ADDRESS, '{',
							   (uint8_t)strlen(buff), (const uint8_t *)buff);
}

/* Keep only characters that belong in a version string ("5.16f-t2"). */
static void clean_version(const char *in, char *out, int len)
{
	int k = 0;
	for (const char *p = in ? in : ""; *p && k < len - 1; p++)
		if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
		    (*p >= 'A' && *p <= 'Z') || *p == '.' || *p == '-' || *p == '_')
			out[k++] = *p;
	out[k] = 0;
}

static int write_script(const char *path, const char *body)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	fputs(body, f);
	fclose(f);
	return 0;
}

/* Run a script in its own session so it outlives this process. */
static int spawn_script(const char *path)
{
	char cmd[300];
	snprintf(cmd, sizeof(cmd), "setsid sh %s </dev/null >/dev/null 2>&1 &", path);
	return system(cmd);
}

/* First line of a small file, trailing newline removed. 0 if missing. */
static int read_line(const char *path, char *out, int len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	if (!fgets(out, len, f))
		out[0] = 0;
	fclose(f);
	out[strcspn(out, "\r\n")] = 0;
	return 1;
}

/* What the update script is doing, worked out from its log. The build is a
 * single long gcc command, so the log is a sequence of stage markers rather
 * than a progress count. */
static void current_step(char *out, int len)
{
	static char log[65536];
	FILE *f = fopen(UPD_LOG_FILE, "r");
	size_t n = 0;
	if (f) {
		n = fread(log, 1, sizeof(log) - 1, f);
		fclose(f);
	}
	log[n] = 0;

	char *p;
	int skipped = strstr(log, "SKIPBUILD:") != NULL;
	if (strstr(log, "PANEL: getting"))
		snprintf(out, len, "%s\nGetting the front panel firmware...",
			skipped ? "zBitx software is up to date." : "zBitx software installed.");
	else if (skipped)
		snprintf(out, len, "zBitx software is up to date.\nChecking the front panel...");
	else if ((p = strstr(log, "Build completed")) || strstr(log, "Stripping zbitx"))
		snprintf(out, len, "Finishing the build...");
	else if ((p = strstr(log, "compiling zbitx version "))) {
		/* "compiling zbitx version v5.15a in /home/pi/sbitx" */
		char ver[24] = "";
		sscanf(p + strlen("compiling zbitx version "), "%23s", ver);
		snprintf(out, len, "Compiling %s.\nThis is the longest step.", ver);
	}
	else if (strstr(log, "rm -f zbitx"))
		snprintf(out, len, "Preparing the build...");
	else if (n > 0)
		snprintf(out, len, "Downloading from GitHub...");
	else
		snprintf(out, len, "Starting...");
}

static void installed_version_from_source(char *out, int len)
{
	char line[200];
	FILE *f = fopen(UPD_SBITX_DIR "/src/sdr_ui.h", "r");
	out[0] = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *q = strstr(line, "define VER_STR \"");
		if (q) {
			q += strlen("define VER_STR \"");
			char *e = strchr(q, '"');
			if (e)
				*e = 0;
			snprintf(out, len, "%s", q);
			break;
		}
	}
	fclose(f);
}

void update_panel_check(const char *installed_version, const char *panel_version)
{
	char b[120];
	snprintf(app_version, sizeof(app_version), "%s", installed_version);
	clean_version(panel_version, panel_installed, sizeof(panel_installed));
	snprintf(b, sizeof(b), "Installed: %s", installed_version);
	panel_push("UPD_CUR", b);

	if (running) {
		panel_push("UPD_NEW", "Available: (update already running)");
		return;
	}

	mkdir(UPD_WORK_DIR, 0755);
	unlink(UPD_CHECK_FILE);
	if (write_script(UPD_CHECK_SH, check_script) || spawn_script(UPD_CHECK_SH)) {
		panel_push("UPD_NEW", "Available: could not check");
		return;
	}
	panel_push("UPD_NEW", "Available: checking GitHub...");
	check_pending = 1;
	check_started_ms = now_ms();
}

int update_panel_start(const char *installed_version, const char *panel_version)
{
	snprintf(app_version, sizeof(app_version), "%s", installed_version);
	if (running)
		return 0;

	mkdir(UPD_WORK_DIR, 0755);
	/* What the panel runs now, so the script can skip an unneeded panel
	 * update. Older panel firmware doesn't send it: then the file is
	 * removed and zbitx-panel-flash asks the panel itself. */
	char pv[24];
	clean_version(panel_version, pv, sizeof(pv));
	unlink(UPD_PANEL_VER);
	if (pv[0]) {
		FILE *f = fopen(UPD_PANEL_VER, "w");
		if (f) {
			fputs(pv, f);
			fclose(f);
		}
	}
	/* Clear the last outcome before the script starts, so the poller can't
	 * pick up a stale result in the moment before the script runs. */
	unlink(UPD_RESULT_FILE);
	unlink(UPD_LOG_FILE);
	if (write_script(UPD_RUN_SH, run_script) || spawn_script(UPD_RUN_SH)) {
		panel_push("UPD_STAT", "Could not start the update script.");
		panel_push("UPD_STATE", "FAIL");
		return -1;
	}
	running = 1;
	run_started_ms = now_ms();
	last_beat_ms = 0;
	panel_push("UPD_STATE", "RUNNING");
	return 0;
}

int update_panel_running(void)
{
	return running;
}

void update_panel_poll(void)
{
	long t = now_ms();
	if (t - last_poll_ms < 250)
		return;
	last_poll_ms = t;

	if (check_pending) {
		char line[160];
		if (read_line(UPD_CHECK_FILE, line, sizeof(line))) {
			char ver[64] = "", out[120];
			int behind = -1;
			check_pending = 0;
			char panel[24] = "";
			int panel_known = 0;
			if (!strncmp(line, "OK|", 3)) {
				char *v = line + 3;
				char *bar = strchr(v, '|');
				if (bar) {
					*bar = 0;
					behind = atoi(bar + 1);
					char *bar2 = strchr(bar + 1, '|');
					if (bar2) {
						panel_known = 1;
						snprintf(panel, sizeof(panel), "%s", bar2 + 1);
					}
				}
				snprintf(ver, sizeof(ver), "%s", v);
				if (app_version[0] && !strcmp(ver, app_version))
					snprintf(out, sizeof(out), "Available: %s (already installed)", ver);
				else if (behind == 0)
					snprintf(out, sizeof(out), "Available: %s (up to date)", ver);
				else if (behind == 1)
					snprintf(out, sizeof(out), "Available: %s (1 new change)", ver);
				else
					snprintf(out, sizeof(out), "Available: %s (%d new changes)", ver, behind);
			}
			else
				snprintf(out, sizeof(out), "Available: can't reach GitHub");
			panel_push("UPD_NEW", out);

			/* Front panel line (panels with the UPD_PNL field show it). */
			const char *cur = panel_installed[0] ? panel_installed : "?";
			if (!panel_known || !panel[0])
				snprintf(out, sizeof(out), "Front panel: %s", cur);
			else if (!strcmp(panel, "-"))
				snprintf(out, sizeof(out), "Front panel: %s (no update available)", cur);
			else if (!strcmp(panel, panel_installed))
				snprintf(out, sizeof(out), "Front panel: %s (up to date)", cur);
			else
				snprintf(out, sizeof(out), "Front panel: %s -> %s", cur, panel);
			panel_push("UPD_PNL", out);
		}
		else if (t - check_started_ms > 90000) {
			check_pending = 0;
			panel_push("UPD_NEW", "Available: no answer from GitHub");
		}
	}

	if (unlock_at_ms && t >= unlock_at_ms) {
		char b[80];
		unlock_at_ms = 0;
		snprintf(b, sizeof(b), "Software version: %s", app_version);
		panel_push("PIVERSION", b);
	}

	if (!running)
		return;

	char result[64];
	int have_result = read_line(UPD_RESULT_FILE, result, sizeof(result));

	/* Pi software built; the script is about to stop this app and update
	 * the front panel. Keep the panel locked and say so until then. */
	if (have_result && !strncmp(result, "PANEL", 5)) {
		if (t - last_beat_ms >= 1000) {
			char b[160];
			last_beat_ms = t;
			int was_built = 1;
			char pver[24] = "";
			sscanf(result + 5, "%d %23s", &was_built, pver);
			snprintf(b, sizeof(b),
				"%s\n"
				"Updating the front panel to %s...\n"
				"Do not power off the radio.",
				was_built ? "zBitx software installed." : "zBitx software is up to date.", pver);
			panel_push("UPD_STAT", b);
			panel_push("UPD_STATE", "RUNNING");
		}
		return;
	}

	if (have_result) {
		running = 0;
		if (!strncmp(result, "NOCHANGE", 8)) {
			/* Nothing rebuilt or flashed: no reboot. Say so, then unlock
			 * the panel: its lock screen leaves on a PIVERSION push, the
			 * same signal the software sends when it starts. */
			const char *why = result[8] ? result + 9 : "";
			const char *pnl = "No front panel update is available.\n";
			if (!strncmp(why, "current", 7))
				pnl = "The front panel is up to date.\n";
			else if (!strcmp(why, "nodownload"))
				pnl = "Couldn't download the front panel\nfirmware.\n";
			char b[160];
			snprintf(b, sizeof(b), "%.24s is already installed.\n%sNothing to update.",
				app_version, pnl);
			panel_push("UPD_STAT", b);
			panel_push("UPD_STATE", "DONE");
			unlock_at_ms = t + 4000;
			return;
		}
		if (!strncmp(result, "OK", 2)) {
			char ver[64], b[160];
			installed_version_from_source(ver, sizeof(ver));
			if (ver[0]) {
				snprintf(b, sizeof(b), "New version: %s", ver);
				panel_push("UPD_NEW", b);
			}
			/* "OK <why the panel was left alone>" */
			const char *why = result[2] ? result + 3 : "";
			const char *pnl = "";
			if (!strncmp(why, "current", 7))
				pnl = "The front panel is up to date.\n";
			else if (!strcmp(why, "nodownload"))
				pnl = "Couldn't download the front panel\nfirmware; the panel was not changed.\n";
			else if (!strcmp(why, "nofile") || !strcmp(why, "notota"))
				pnl = "No front panel update is available.\n";
			snprintf(b, sizeof(b), "Update complete.\n%sRebooting the radio...", pnl);
			panel_push("UPD_STAT", b);
			panel_push("UPD_STATE", "DONE");
		}
		else if (!strcmp(result, "NOBIN")) {
			panel_push("UPD_STAT",
				"BUILD FAILED.\n"
				"Don't restart the radio yet: it\n"
				"won't start until this is fixed.\n"
				"Log: " UPD_LOG_SHOWN);
			panel_push("UPD_STATE", "FAIL");
		}
		else {
			panel_push("UPD_STAT",
				"UPDATE FAILED.\n"
				"The old version is still running.\n"
				"Log: " UPD_LOG_SHOWN);
			panel_push("UPD_STATE", "FAIL");
		}
		return;
	}

	/* Once a second: progress plus the heartbeat that keeps the panel
	 * locked (and re-locks a panel that was reset during the update). */
	if (t - last_beat_ms >= 1000) {
		last_beat_ms = t;
		long secs = (t - run_started_ms) / 1000;
		char step[96], b[160];
		current_step(step, sizeof(step));
		snprintf(b, sizeof(b),
			"Do not power off the radio.\n"
			"Elapsed: %ld:%02ld\n"
			"%s", secs / 60, secs % 60, step);
		panel_push("UPD_STAT", b);
		panel_push("UPD_STATE", "RUNNING");
	}
}
