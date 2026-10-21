#ifndef UPDATE_PANEL_H
#define UPDATE_PANEL_H

/*
 * update_panel.c / update_panel.h
 *
 * Software update from the front panel (MENU -> UPDATE).
 *
 * Command flow (panel -> Pi), routed through cmd_exec() in sbitx_main.c:
 *   "UPDCHECK <panel version>"
 *              -> push the installed version to UPD_CUR, then fetch the
 *                 current branch from GitHub in the background and push the
 *                 available version (and how many commits behind) to UPD_NEW,
 *                 and the front panel's installed/available versions to
 *                 UPD_PNL. (Older panels send no version.)
 *   "UPDATE <panel version>"
 *              -> as below; after the Pi build, the newest published panel
 *                 firmware is installed over I2C if it differs.
 *   "UPDATE"   -> run ~/sbitx/update detached from this process. While it
 *                 runs, push UPD_STATE RUNNING plus a progress line to
 *                 UPD_STAT once a second; the panel stays locked on its update
 *                 screen as long as it hears this heartbeat.
 *
 * When the script finishes, push UPD_STATE DONE (the script reboots the Pi
 * five seconds later) or UPD_STATE FAIL. After the reboot, the new software's
 * zbitx_init() pushes PIVERSION, which tells the panel to unlock.
 *
 * Nothing here blocks: git and the build run as background shell processes
 * that write their results to files in ~/sbitx/data, and update_panel_poll()
 * (called from zbitx_poll() on the main thread, the only thread that may use
 * the bit-banged I2C bus) reads those files and pushes to the panel.
 */

/* Panel asked for versions (the update confirm screen just opened). */
void update_panel_check(const char *installed_version, const char *panel_version);

/* Panel confirmed: start the update. Returns 0 if started, -1 if not. */
int update_panel_start(const char *panel_version);

/* Non-zero while an update started by this process is running. */
int update_panel_running(void);

/* Call often from the main thread; rate-limits itself. */
void update_panel_poll(void);

#endif
