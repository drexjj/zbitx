#ifndef WIFI_PANEL_H
#define WIFI_PANEL_H

/*
 * wifi_panel.c / wifi_panel.h
 *
 * Wireless-LAN (station mode) management for the ILI9488 front panel.
 *
 * Raspberry Pi OS Bookworm manages Wi-Fi through NetworkManager, so all the
 * heavy lifting here shells out to `nmcli`. Those calls (a scan in particular)
 * can block for several seconds, and the front-panel I2C poll loop
 * (zbitx_poll in sbitx_gtk.c) must never stall for that long, so every nmcli
 * operation runs on a short-lived background worker thread.
 *
 * The worker NEVER touches the bit-banged I2C bus itself — that bus is shared
 * and is only ever driven from the GTK/main thread. Instead the worker writes
 * its results into thread-safe buffers here, and the main thread drains them
 * from wifi_panel_poll(), which it calls once per zbitx_poll() pass and which
 * then pushes any pending text to the panel with the normal {LABEL value}
 * blocks.
 *
 * Command flow (panel -> Pi), all routed through cmd_exec() in sbitx_gtk.c:
 *   "WIFI scan"                 -> start an async scan; results land in WIFI_LIST
 *   "WIFI status"               -> push current SSID / IP / signal to the panel
 *   "WIFI connect <ssid>\t<psk>"-> join a network (psk may be empty for open)
 *   "WIFI connect <ssid>"       -> join a known/open network (no psk)
 *   "WIFI disconnect"           -> disconnect wlan0
 *   "WIFI forget <ssid>"        -> delete the saved connection profile
 *
 * The SSID and passphrase are separated by a TAB ('\t') because SSIDs may
 * legally contain spaces. The panel builds this string; see fields.ino.
 */

/* Called once at startup (from the same place hamlib/remote get started). */
void wifi_panel_init(void);

/*
 * Handle a "WIFI ..." command string (the part AFTER the leading "WIFI ").
 * Called from cmd_exec(). Returns immediately; slow work is threaded.
 */
void wifi_panel_command(const char *args);

/*
 * Drain any results the worker thread has produced and push them to the
 * front panel over I2C. MUST be called only from the GTK/main thread
 * (it is the only writer allowed on the bit-banged bus). Safe to call every
 * poll pass; it does nothing when there is nothing pending.
 */
void wifi_panel_poll(void);

#endif /* WIFI_PANEL_H */
