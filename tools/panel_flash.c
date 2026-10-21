/*
 * zbitx-panel-flash — send new firmware to the zBitx front panel over I2C.
 *
 *   zbitx-panel-flash [options] <zbitxfrontpanel.uf2 | .bin>
 *
 * The Pi and the panel's Pico share only the I2C bus (/dev/i2c-3), so the
 * image goes over that, in checksummed chunks, to firmware that can store it
 * and install it (panel firmware with fw_update.ino, built with an update
 * area). The protocol is described in the panel's fw_proto.h.
 *
 * The zBitx app must not be running: it polls the same panel and would read
 * the panel's answers. Stop it first (pkill zbitx) and restart the radio
 * afterwards.
 *
 * Before sending anything the image is checked: a UF2 must be for an RP2040,
 * the image must start with a valid RP2040 boot block, and it must contain
 * the "ZBITX-PANEL-FW" marker. The panel checks the MD5 of what it received
 * before it installs anything.
 *
 *   --check         only check the file and ask the panel; send nothing
 *   --info          only check the file and print its panel version
 *   --yes           don't ask for confirmation
 *   --if-different  do nothing if the panel already runs this version
 *   --report        tell the panel's update lock screen what happened when
 *                   the panel doesn't take over the screen itself (used by
 *                   the UPDATE button's script)
 *
 * Exit status: 0 sent and installed, 3 panel already up to date, 4 panel
 * can't take updates this way (needs one USB flash), 5 file is not a usable
 * panel image, 1 anything else.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <openssl/evp.h>

#include "../src/i2cbb.h"

#define PANEL_ADDR     0x0a
#define FWP_CMD        0x02
#define FWP_CHUNK      192
#define FLASH_BASE     0x10000000u
#define FLASH_MAX      (2u * 1024 * 1024)
#define UF2_FAMILY_RP2040 0xe48bff56u
#define MARKER         "ZBITX-PANEL-FW "

#define EX_SENT     0
#define EX_FAILED   1
#define EX_CURRENT  3
#define EX_PANEL    4
#define EX_IMAGE    5

static int report = 0;

static volatile sig_atomic_t interrupted = 0;
static void on_sigint(int s) { (void)s; interrupted = 1; }

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sleep_ms(int ms) { usleep(ms * 1000); }

/* ---- image loading and checks ------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) {
		fclose(f);
		return NULL;
	}
	uint8_t *b = malloc(n);
	if (b && fread(b, 1, n, f) != (size_t)n) {
		free(b);
		b = NULL;
	}
	fclose(f);
	*len = n;
	return b;
}

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* UF2 -> flat image starting at the flash base. Only RP2040 blocks. */
static uint8_t *uf2_to_bin(const uint8_t *u, size_t ulen, size_t *blen, const char **why)
{
	uint8_t *img = malloc(FLASH_MAX);
	size_t top = 0;
	int blocks = 0;
	if (!img) {
		*why = "out of memory";
		return NULL;
	}
	memset(img, 0xff, FLASH_MAX);
	for (size_t off = 0; off + 512 <= ulen; off += 512) {
		const uint8_t *b = u + off;
		if (rd32(b) != 0x0A324655 || rd32(b + 4) != 0x9E5D5157 || rd32(b + 508) != 0x0AB16F30) {
			*why = "not a UF2 file";
			goto bad;
		}
		uint32_t flags = rd32(b + 8), addr = rd32(b + 12), size = rd32(b + 16);
		if (flags & 0x1)          /* "not main flash" */
			continue;
		if (!(flags & 0x2000) || rd32(b + 28) != UF2_FAMILY_RP2040) {
			*why = "UF2 is not for an RP2040";
			goto bad;
		}
		if (size > 476 || addr < FLASH_BASE || addr + size > FLASH_BASE + FLASH_MAX) {
			*why = "UF2 block outside the Pico's flash";
			goto bad;
		}
		memcpy(img + (addr - FLASH_BASE), b + 32, size);
		if (addr - FLASH_BASE + size > top)
			top = addr - FLASH_BASE + size;
		blocks++;
	}
	if (!blocks) {
		*why = "UF2 has no flash blocks";
		goto bad;
	}
	*blen = top;
	return img;
bad:
	free(img);
	return NULL;
}

/* RP2040 boot block: 252 bytes + CRC-32/MPEG-2 of them. */
static int boot2_ok(const uint8_t *img, size_t len)
{
	if (len < 256)
		return 0;
	uint32_t c = 0xffffffff;
	for (int i = 0; i < 252; i++) {
		c ^= (uint32_t)img[i] << 24;
		for (int b = 0; b < 8; b++)
			c = (c & 0x80000000) ? (c << 1) ^ 0x04c11db7 : c << 1;
	}
	return c == rd32(img + 252);
}

/* Finds the marker and copies "ZBITX-PANEL-FW 5.16a" into id. */
static int find_marker(const uint8_t *img, size_t len, char *id, int idlen)
{
	size_t ml = strlen(MARKER);
	for (size_t i = 0; i + ml < len; i++) {
		if (img[i] == 'Z' && !memcmp(img + i, MARKER, ml)) {
			int k = 0;
			while (k < idlen - 1 && i + k < len && img[i + k] >= 32 && img[i + k] < 127)
				id[k] = img[i + k], k++;
			id[k] = 0;
			return 1;
		}
	}
	return 0;
}

/* gzip -9 the image (the panel's installer unpacks it). */
static uint8_t *gzip_image(const uint8_t *img, size_t len, size_t *glen)
{
	char tmp[] = "/tmp/zbitx-panel-XXXXXX";
	int fd = mkstemp(tmp);
	if (fd < 0)
		return NULL;
	FILE *f = fdopen(fd, "wb");
	fwrite(img, 1, len, f);
	fclose(f);

	char cmd[128];
	snprintf(cmd, sizeof(cmd), "gzip -9 -n -c %s", tmp);
	FILE *p = popen(cmd, "r");
	uint8_t *out = NULL;
	size_t cap = 0, n = 0;
	if (p) {
		for (;;) {
			if (n + 65536 > cap) {
				cap = cap ? cap * 2 : 262144;
				out = realloc(out, cap);
			}
			size_t r = fread(out + n, 1, 65536, p);
			if (r == 0)
				break;
			n += r;
		}
		if (pclose(p) != 0) {
			free(out);
			out = NULL;
		}
	}
	unlink(tmp);
	*glen = n;
	return out;
}

static void md5_hex(const uint8_t *d, size_t len, char out[33])
{
	unsigned char m[EVP_MAX_MD_SIZE];
	unsigned int ml = 0;
	EVP_Digest(d, len, m, &ml, EVP_md5(), NULL);
	for (unsigned i = 0; i < 16; i++)
		sprintf(out + i * 2, "%02x", m[i]);
	out[32] = 0;
}

/* ---- talking to the panel ----------------------------------------------- */

static uint16_t crc16(const uint8_t *d, int len)
{
	uint16_t crc = 0xFFFF;
	for (int i = 0; i < len; i++) {
		crc ^= (uint16_t)d[i] << 8;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

static int send_frame(uint8_t *f, int len)
{
	uint16_t c = crc16(f, len);
	f[len] = c & 0xff;
	f[len + 1] = c >> 8;
	return i2cbb_write_i2c_block_data(PANEL_ADDR, FWP_CMD, len + 2, f) < 0 ? -1 : 0;
}

/* One read from the panel; returns its text (NUL-terminated) or NULL. */
static const char *read_reply(void)
{
	static uint8_t buff[260];
	int n = i2cbb_read_rll(PANEL_ADDR, buff);
	if (n < 0 || n > 250)
		return NULL;
	buff[n] = 0;
	/* panel text may carry several lines; the status is always first */
	char *nl = strchr((char *)buff, '\n');
	if (nl)
		*nl = 0;
	return (const char *)buff;
}

/* Poll until a reply starting with FW (other than FWBUSY) arrives. */
static const char *wait_fw(int timeout_ms)
{
	long end = now_ms() + timeout_ms;
	while (now_ms() < end && !interrupted) {
		const char *r = read_reply();
		if (r && !strncmp(r, "FW", 2) && strcmp(r, "FWBUSY"))
			return r;
		sleep_ms(r ? 1 : 5);
	}
	return NULL;
}

/* --report: show text on the panel's update lock screen (UPD_* fields). */
static void panel_say(const char *label, const char *text)
{
	if (!report)
		return;
	char b[200];
	snprintf(b, sizeof(b), "%s %s}", label, text);
	i2cbb_write_i2c_block_data(PANEL_ADDR, '{', strlen(b), (const uint8_t *)b);
}

/* --report: final message for an outcome where the panel isn't flashed. The
 * lock screen then waits for the radio's reboot, as after a normal update. */
static void panel_finish(const char *text)
{
	panel_say("UPD_STAT", text);
	panel_say("UPD_STATE", "DONE");
}

static int app_running(void)
{
	return system("pgrep -x zbitx >/dev/null 2>&1") == 0;
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	int check_only = 0, yes = 0, info = 0, if_different = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--check")) check_only = 1;
		else if (!strcmp(argv[i], "--yes")) yes = 1;
		else if (!strcmp(argv[i], "--info")) info = 1;
		else if (!strcmp(argv[i], "--if-different")) if_different = 1;
		else if (!strcmp(argv[i], "--report")) report = 1;
		else if (argv[i][0] != '-' && !path) path = argv[i];
		else {
			fprintf(stderr, "usage: %s [--check|--info] [--yes] [--if-different] [--report] <panel firmware .uf2|.bin>\n", argv[0]);
			return 2;
		}
	}
	if (!path) {
		fprintf(stderr, "usage: %s [--check|--info] [--yes] [--if-different] [--report] <panel firmware .uf2|.bin>\n", argv[0]);
		return 2;
	}

	/* -- load and check the image -- */
	size_t flen = 0, len = 0;
	uint8_t *file = read_file(path, &flen);
	if (!file) {
		fprintf(stderr, "Can't read %s\n", path);
		return EX_IMAGE;
	}
	uint8_t *img = file;
	len = flen;
	if (flen >= 512 && rd32(file) == 0x0A324655) {
		const char *why = "";
		img = uf2_to_bin(file, flen, &len, &why);
		if (!img) {
			fprintf(stderr, "%s: %s\n", path, why);
			return EX_IMAGE;
		}
	}
	if (len < 65536 || len > FLASH_MAX) {
		fprintf(stderr, "%s: %zu bytes is not a plausible panel image\n", path, len);
		return EX_IMAGE;
	}
	if (!boot2_ok(img, len)) {
		fprintf(stderr, "%s: no valid RP2040 boot block at the start; not a Pico flash image\n", path);
		return EX_IMAGE;
	}
	char img_id[64];
	if (!find_marker(img, len, img_id, sizeof(img_id))) {
		fprintf(stderr, "%s: not zBitx front panel firmware (no \"" MARKER "\" marker).\n"
			"Only firmware built with fw_update.ino can be sent this way.\n", path);
		return EX_IMAGE;
	}
	const char *img_ver = img_id + strlen(MARKER);
	if (info) {
		printf("%s\n", img_ver);
		return EX_SENT;
	}

	size_t glen = 0;
	uint8_t *gz = gzip_image(img, len, &glen);
	if (!gz) {
		fprintf(stderr, "gzip failed\n");
		return 1;
	}
	char md5[33];
	md5_hex(gz, glen, md5);
	printf("Image:  %s, %zu KB (%zu KB compressed)\n", img_id, len / 1024, glen / 1024);

	/* -- find the panel -- */
	if (app_running()) {
		fprintf(stderr, "The zBitx app is running. Stop it first:  pkill zbitx\n");
		return EX_FAILED;
	}
	panel_say("UPD_STAT", "Checking the front panel...\nDo not power off the radio.");
	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	/* Ask the panel whether it can take an update. The answer comes back on
	 * a later read; resend the question every second in case it was missed,
	 * and keep a record of what the panel said instead, for the log. */
	const char *q = "FWQUERY 1}";
	char cap[80] = "";
	char seen[4][64];
	int nseen = 0, reads = 0, read_errors = 0, sends = 0, send_errors = 0;
	long end = now_ms() + 10000, next_send = 0;
	while (now_ms() < end && !cap[0] && !interrupted) {
		if (now_ms() >= next_send) {
			sends++;
			if (i2cbb_write_i2c_block_data(PANEL_ADDR, '{', strlen(q), (const uint8_t *)q) < 0)
				send_errors++;
			next_send = now_ms() + 1000;
		}
		const char *r = read_reply();
		reads++;
		if (!r)
			read_errors++;
		else if (!strncmp(r, "FWCAP ", 6))
			snprintf(cap, sizeof(cap), "%.79s", r);
		else {
			int dup = 0;
			for (int i = 0; i < nseen; i++)
				if (!strncmp(seen[i], r, 63))
					dup = 1;
			if (!dup && nseen < 4)
				snprintf(seen[nseen++], 64, "%.63s", r);
		}
		if (!cap[0])
			sleep_ms(20);
	}
	if (!cap[0] && send_errors == sends) {
		fprintf(stderr, "Can't reach the front panel on I2C\n");
		panel_finish("The front panel update didn't start.\nRebooting the radio...");
		return EX_FAILED;
	}
	if (!cap[0]) {
		fprintf(stderr, "No answer to the update query: %d queries (%d failed), "
			"%d reads (%d failed).\n", sends, send_errors, reads, read_errors);
		for (int i = 0; i < nseen; i++)
			fprintf(stderr, "  the panel said: \"%s\"\n", seen[i]);
		if (app_running())
			fprintf(stderr, "  NOTE: a zbitx process is running again; it reads the panel's answers.\n");
		fprintf(stderr, "The front panel didn't answer the update query. Its firmware is\n"
			"older than this feature: flash it once by USB (hold the knob in at\n"
			"power-up, copy the .uf2 to RPI-RP2). After that it can be updated here.\n");
		panel_finish("Front panel not updated: its firmware\nneeds one USB update first.\nRebooting the radio...");
		return EX_PANEL;
	}
	char panel_id[64] = "", panel_ver[32] = "";
	unsigned long area = 0, heap = 0;
	sscanf(cap + 6, "%63s %31s %lu %lu", panel_id, panel_ver, &area, &heap);
	printf("Panel:  %s %s, update area %lu KB, free RAM %lu bytes\n", panel_id, panel_ver, area / 1024, heap);
	if (area == 0) {
		fprintf(stderr, "This panel firmware was built without an update area\n"
			"(flash size option \"2MB (Sketch: 1536KB, FS: 512KB)\"). Flash it once by USB.\n");
		panel_finish("Front panel not updated: its firmware\nneeds one USB update first.\nRebooting the radio...");
		return EX_PANEL;
	}
	if (glen > area) {
		fprintf(stderr, "The compressed image (%zu KB) doesn't fit the panel's update area (%lu KB)\n",
			glen / 1024, area / 1024);
		panel_finish("Front panel not updated: the new\nfirmware is too big for it.\nRebooting the radio...");
		return EX_PANEL;
	}
	if (if_different && !strcmp(panel_ver, img_ver)) {
		printf("The panel already runs %s; nothing to do.\n", img_ver);
		panel_finish("Front panel is up to date.\nRebooting the radio...");
		return EX_CURRENT;
	}
	if (check_only) {
		printf("Check OK; nothing sent.\n");
		return EX_SENT;
	}
	if (!yes) {
		printf("Send it to the panel? [y/N] ");
		fflush(stdout);
		char a[8] = "";
		if (!fgets(a, sizeof(a), stdin) || (a[0] != 'y' && a[0] != 'Y'))
			return 1;
	}

	/* -- begin -- */
	uint8_t f[1 + 4 + 1 + FWP_CHUNK + 2];
	f[0] = 'B';
	f[1] = glen & 0xff; f[2] = (glen >> 8) & 0xff; f[3] = (glen >> 16) & 0xff; f[4] = (glen >> 24) & 0xff;
	memcpy(f + 5, md5, 32);
	const char *r = NULL;
	for (int tries = 0; tries < 5; tries++) {
		if (send_frame(f, 1 + 4 + 32) == 0)
			r = wait_fw(30000);   /* the first start formats the update area */
		if (r && (!strncmp(r, "FWACK", 5) || !strncmp(r, "FWERR", 5)))
			break;               /* FWNAK or nothing: send it again */
	}
	if (!r || strcmp(r, "FWACK 0")) {
		fprintf(stderr, "The panel didn't start the update: %s\n", r ? r : "no answer");
		if (r && strstr(r, "begin 98"))
			fprintf(stderr, "(not enough free RAM on the panel)\n");
		panel_finish("The front panel update didn't start.\nRebooting the radio...");
		return EX_FAILED;
	}

	/* -- data -- */
	long t0 = now_ms();
	uint32_t off = 0;
	int stalls = 0, resends = 0, last_pct = -1;
	while (off < glen) {
		if (interrupted) {
			f[0] = 'X';
			send_frame(f, 1);
			fprintf(stderr, "\nStopped. The panel restarts with its current firmware.\n");
			return 1;
		}
		int n = glen - off > FWP_CHUNK ? FWP_CHUNK : (int)(glen - off);
		f[0] = 'D';
		f[1] = off & 0xff; f[2] = (off >> 8) & 0xff; f[3] = (off >> 16) & 0xff; f[4] = off >> 24;
		f[5] = n;
		memcpy(f + 6, gz + off, n);
		send_frame(f, 6 + n);   /* a failed write shows up as a missing ACK */
		/* the panel writes flash at each 4 KB boundary; reads stall meanwhile */
		if ((off / 4096) != ((off + n) / 4096))
			sleep_ms(60);
		r = wait_fw(3000);
		uint32_t next = off;
		if (r && (!strncmp(r, "FWACK ", 6) || !strncmp(r, "FWNAK ", 6)))
			next = strtoul(r + 6, NULL, 10);
		else if (r && !strncmp(r, "FWERR", 5)) {
			fprintf(stderr, "\nThe panel stopped the update: %s\n"
				"It restarts with its current firmware.\n", r);
			return 1;
		}
		if (next > glen)          /* nonsense answer: treat as no progress */
			next = off;
		if (next > off)
			stalls = 0;
		else {
			resends++;
			if (++stalls > 20) {
				fprintf(stderr, "\nNo progress at byte %u; giving up. The panel restarts\n"
					"with its current firmware after 30 seconds.\n", off);
				return 1;
			}
		}
		off = next;               /* the panel says where to continue */
		int pct = (int)(100.0 * off / glen);
		if (pct != last_pct) {
			last_pct = pct;
			double secs = (now_ms() - t0) / 1000.0;
			printf("\rSending: %3d%%  %4u/%zu KB  %.0f s", pct, off / 1024, glen / 1024, secs);
			fflush(stdout);
		}
	}

	/* -- end -- */
	f[0] = 'E';
	r = NULL;
	for (int tries = 0; tries < 5; tries++) {
		if (send_frame(f, 1) == 0)
			r = wait_fw(30000);   /* the panel checks the MD5 before answering */
		if (r && (!strcmp(r, "FWDONE") || !strncmp(r, "FWERR", 5)))
			break;
	}
	double secs = (now_ms() - t0) / 1000.0;
	printf("\n");
	if (!r || strcmp(r, "FWDONE")) {
		fprintf(stderr, "The panel did not accept the image: %s\n", r ? r : "no answer");
		return 1;
	}
	printf("Sent %zu KB in %.0f s (%.1f KB/s, %d resends). The panel checked it and is\n"
		"installing it now; it restarts by itself in under a minute.\n"
		"Then restart the radio (sudo reboot) to bring the zBitx app back.\n",
		glen / 1024, secs, glen / 1024.0 / (secs > 0 ? secs : 1), resends);
	return 0;
}
