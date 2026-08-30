# Makefile for zbitx
# Equivalent to the shell build script.
#
# Usage:
#   make                 # build zbitx (debug, default)
#   make zbitx           # build zbitx (debug)
#   make OPT=o zbitx     # optimized build (-march=native -O3 -flto)
#   make OPT=g zbitx     # profile-generate build
#   make OPT=u zbitx     # profile-use build
#   make clean           # remove binaries
#   make distclean       # also remove profiling data

CC      := gcc

# ---- Source files -----------------------------------------------------------
COMMON_SRC := \
	src/vfo.c src/sbitx_sound.c src/fft_filter.c src/sbitx_gtk.c src/sbitx_utils.c \
	src/i2c.c src/si5351v2.c src/ini.c src/hamlib.c src/queue.c src/modems.c src/logbook.c \
	src/modem_cw.c src/settings_ui.c src/hist_disp.c src/ntputil.c \
	src/telnet.c src/macros.c src/modem_ft8.c src/remote.c src/mongoose.c src/para_eq.c \
	src/webserver.c src/eq_ui.c

FT8_LIB := src/ft8_lib/libft8.a

# ---- Flags ------------------------------------------------------------------
# Default (debug) flags. Override the optimization mode with OPT=o|g|u.
FLAGS       := -g
EXTRA_CFLAGS :=
STRIP_BIN   := no

ifeq ($(OPT),o)
	FLAGS     := -march=native -O3 -flto=auto
	STRIP_BIN := yes
endif
ifeq ($(OPT),g)
	FLAGS := -march=native -O3 -flto=auto -fprofile-generate
endif
ifeq ($(OPT),u)
	FLAGS     := -march=native -O3 -flto=auto -fprofile-use
	STRIP_BIN := yes
endif

MONGOOSE_FLAGS := -DMG_ENABLE_OPENSSL=1 -DMG_ENABLE_MBEDTLS=0 -DMG_ENABLE_LINES=1 \
	-DMG_TLS=MG_TLS_OPENSSL -DMG_ENABLE_SSI=0 -DMG_ENABLE_IPV6=0

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)

LIBS := -lwiringPi -lasound -lm -lfftw3 -lfftw3f -pthread -lncurses -lsqlite3 \
	-lnsl -lrt -lssl -lcrypto

# ---- Targets ----------------------------------------------------------------
.PHONY: all zbitx dirs db clean distclean

all: zbitx

dirs:
	@mkdir -p ./audio ./data ./web

db: dirs
	@if test -f data/sbitx.db; then \
		echo "database is intact"; \
	else \
		echo "database doesn't exist, it will be created"; \
		cd data && sqlite3 sbitx.db < create_db.sql; \
	fi

# --- zbitx (headless daemon; compiles sbitx.c with the daemon define) --------
zbitx: EXTRA_CFLAGS := -DJJ_HEADLESS_DAEMON=1
zbitx: db
	@[ "$(OPT)" = "o" ] && rm -f *.gcda || true
	@[ "$(OPT)" = "g" ] && rm -f *.gcda || true
	$(CC) $(FLAGS) $(EXTRA_CFLAGS) $(MONGOOSE_FLAGS) -o $@ \
		$(COMMON_SRC) src/sbitx.c \
		$(FT8_LIB) \
		$(LIBS) $(GTK_CFLAGS) $(GTK_LIBS)
	@[ "$(STRIP_BIN)" = "yes" ] && { echo "Stripping $@"; strip $@; } || true
	@if [ -x $@ ]; then \
		sudo setcap 'cap_sys_nice,cap_sys_time+ep' $@ || :; \
	fi
	@echo "Build completed."

clean:
	rm -f zbitx

distclean: clean
	rm -f *.gcda
