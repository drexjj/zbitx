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

# Version string, pulled from src/sdr_ui.h (#define VER_STR "zbitx vX.YZ").
# := expands once at parse time by running the shell command.
VERSION := $(shell grep VER src/sdr_ui.h | awk 'FNR==1{print $$4}' | sed -e 's/"//g')

# ---- Source files -----------------------------------------------------------
COMMON_SRC := \
	src/vfo.c src/sbitx_sound.c src/fft_filter.c src/sbitx_main.c src/sbitx_utils.c \
	src/i2c.c src/si5351v2.c src/ini.c src/hamlib.c src/queue.c src/modems.c src/logbook.c \
	src/modem_cw.c src/cw_runtime.c src/radio_control.c src/hist_disp.c src/ntputil.c \
	src/telnet.c src/macros.c src/modem_ft8.c src/remote.c src/mongoose.c src/para_eq.c \
	src/webserver.c src/wifi_panel.c \
	src/iq_baseband.c src/hpsdr_p1.c

# FT8/FT4 codec: kgoba/ft8_lib (vendored in src/ft8_lib, upstream commit
# 9fec6ca, which adds non-standard/compound callsign support). It is compiled
# from source with the rest of zbitx, so there is no prebuilt library to keep
# in sync with the headers.
FT8_SRC := \
	src/ft8_lib/ft8/constants.c src/ft8_lib/ft8/crc.c src/ft8_lib/ft8/decode.c \
	src/ft8_lib/ft8/encode.c src/ft8_lib/ft8/ldpc.c src/ft8_lib/ft8/message.c \
	src/ft8_lib/ft8/text.c src/ft8_lib/fft/kiss_fft.c src/ft8_lib/fft/kiss_fftr.c

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

LIBS := -lwiringPi -lasound -lm -lfftw3 -lfftw3f -pthread -lncurses -lsqlite3 \
	-lnsl -lrt -lssl -lcrypto

# ---- Targets ----------------------------------------------------------------
.PHONY: all zbitx dirs db clean distclean test-iq hpsdr-bench test-hpsdr

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

zbitx: db
	@echo "compiling $@ version $(VERSION) in $(CURDIR)"
	@[ "$(OPT)" = "o" ] && rm -f *.gcda || true
	@[ "$(OPT)" = "g" ] && rm -f *.gcda || true
	$(CC) $(FLAGS) $(EXTRA_CFLAGS) $(MONGOOSE_FLAGS) -o $@ \
		$(COMMON_SRC) src/sbitx.c \
		$(FT8_SRC) \
		$(LIBS)
	@[ "$(STRIP_BIN)" = "yes" ] && { echo "Stripping $@"; strip $@; } || true
	@if [ -x $@ ]; then \
		sudo setcap 'cap_sys_nice,cap_sys_time+ep' $@ || :; \
	fi
	@echo "Build completed at $$(date)."
	@echo "Version $(VERSION) brought to you by volunteers at Radio & Electronics Hub"
	@echo "Please consider a small donation as a token of thanks.."

clean:
	rm -f zbitx tests/test_iq tests/hpsdr_bench

# ---- Bench tests (no radio needed) -------------------------------------------
# test-iq:     I/Q front end (iq_baseband.c) against synthetic IF tones
# hpsdr-bench: the HPSDR server with a fake audio thread, for
#              tests/hpsdr_client.py (or a real SDR app) to connect to
# test-hpsdr:  runs hpsdr-bench and the client checks against it
test-iq:
	$(CC) -O2 -Wall -o tests/test_iq tests/test_iq_baseband.c src/iq_baseband.c -lm
	./tests/test_iq

hpsdr-bench:
	$(CC) -O2 -Wall -o tests/hpsdr_bench tests/hpsdr_bench.c src/hpsdr_p1.c src/iq_baseband.c -lm -pthread

test-hpsdr: hpsdr-bench
	@./tests/hpsdr_bench 0 > tests/hpsdr_bench.log 2>&1 & pid=$$!; sleep 0.5; rc=0; \
	python3 tests/hpsdr_client.py --host 127.0.0.1 --freq 7074000 --seconds 3 --expect-tone 3000 || rc=1; \
	python3 tests/hpsdr_client.py --host 127.0.0.1 --rate 48000 --nrx 2 --seconds 2 --expect-tone 3000 || rc=1; \
	kill $$pid; cat tests/hpsdr_bench.log; exit $$rc

distclean: clean
	rm -f *.gcda