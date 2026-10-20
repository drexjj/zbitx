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
/* How the log path is shown on the panel. */
#define UPD_LOG_SHOWN   "~/.zbitx_update/update.log"

/* Fetch the branch this install is on and report the version it would get.
 * Writes one line, "OK|<version>|<commits behind>" or "ERR", via a temp file
 * so the poller never reads a half-written result. */
static const char *check_script =
	"#!/bin/sh\n"
	"cd " UPD_SBITX_DIR " || { echo ERR > " UPD_CHECK_FILE "; exit 0; }\n"
	"t=" UPD_CHECK_FILE ".tmp\n"
	"b=$(git rev-parse --abbrev-ref HEAD)\n"
	"if timeout 60 git fetch -q origin \"$b\" >/dev/null 2>&1; then\n"
	"  v=$(git show FETCH_HEAD:src/sdr_ui.h | grep 'define VER_STR' | head -n 1 | cut -d'\"' -f2)\n"
	"  n=$(git rev-list --count HEAD..FETCH_HEAD)\n"
	"  echo \"OK|${v:-unknown}|${n:-?}\" > $t\n"
	"else\n"
	"  echo ERR > $t\n"
	"fi\n"
	"mv -f $t " UPD_CHECK_FILE "\n";

/* Run the stock update script, record the outcome, reboot on success.
 * NOBIN means the build failed after `make clean` removed the binary, so the
 * radio would not start again if it were restarted now. */
static const char *run_script =
	"#!/bin/sh\n"
	"cd " UPD_SBITX_DIR " || { echo FAIL > " UPD_RESULT_FILE "; exit 0; }\n"
	"if HOME=" UPD_HOME " ./update > " UPD_LOG_FILE " 2>&1; then\n"
	"  echo OK > " UPD_RESULT_FILE "\n"
	"  sleep 5\n"
	"  " UPD_REBOOT_CMD "\n"
	"elif [ -x zbitx ]; then\n"
	"  echo FAIL > " UPD_RESULT_FILE "\n"
	"else\n"
	"  echo NOBIN > " UPD_RESULT_FILE "\n"
	"fi\n";

static int check_pending = 0;
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
	if ((p = strstr(log, "Build completed")) || strstr(log, "Stripping zbitx"))
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

void update_panel_check(const char *installed_version)
{
	char b[120];
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

int update_panel_start(void)
{
	if (running)
		return 0;

	mkdir(UPD_WORK_DIR, 0755);
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
			if (!strncmp(line, "OK|", 3)) {
				char *v = line + 3;
				char *bar = strchr(v, '|');
				if (bar) {
					*bar = 0;
					behind = atoi(bar + 1);
				}
				snprintf(ver, sizeof(ver), "%s", v);
				if (behind == 0)
					snprintf(out, sizeof(out), "Available: %s (up to date)", ver);
				else if (behind == 1)
					snprintf(out, sizeof(out), "Available: %s (1 new change)", ver);
				else
					snprintf(out, sizeof(out), "Available: %s (%d new changes)", ver, behind);
			}
			else
				snprintf(out, sizeof(out), "Available: can't reach GitHub");
			panel_push("UPD_NEW", out);
		}
		else if (t - check_started_ms > 90000) {
			check_pending = 0;
			panel_push("UPD_NEW", "Available: no answer from GitHub");
		}
	}

	if (!running)
		return;

	char result[32];
	if (read_line(UPD_RESULT_FILE, result, sizeof(result))) {
		running = 0;
		if (!strcmp(result, "OK")) {
			char ver[64], b[120];
			installed_version_from_source(ver, sizeof(ver));
			if (ver[0]) {
				snprintf(b, sizeof(b), "New version: %s", ver);
				panel_push("UPD_NEW", b);
			}
			panel_push("UPD_STAT",
				"Update complete.\n"
				"Rebooting the radio...\n"
				"This screen clears when it is back.");
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
