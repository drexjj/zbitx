/*
 * Headless JJ daemon build marker.
 * The actual main() stays in sbitx_gtk.c and switches to headless mode
 * when the build script defines JJ_HEADLESS_DAEMON.
 */
const char *jjbitxd_build_flavor = "jjbitxd-headless";
