# HPSDR Protocol 1 and I/Q output

zBitx can stream its receive I/Q to SDR apps that speak openHPSDR
Protocol 1, such as SparkSDR, SDR Console, piHPSDR, Thetis and linHPSDR.
The apps see it as a single-receiver Hermes-class radio on UDP port 1024.
The code is ported from maxibitx (https://github.com/MikeJohnshoy/maxibitx).

## How the I/Q is made

zBitx's own receiver (`rx_linear()`) works in the frequency domain and never
produces time-domain I/Q, so `iq_baseband.c` builds its own from the same
ADC samples:

    WM8731 real samples at 96 kHz (crystal-filter passband at the IF)
      -> complex NCO at -(IF + offset)
      -> 63-tap lowpass on I and Q (flat to +-15 kHz, image down > 80 dB)
      -> hpsdr_p1.c ring buffer -> pacer thread -> UDP

- The IF is `center_bin * 96000 / 2048`, which is 28125 Hz for the default
  `center_bin` of 600.
- The offset is the CW/CWR pitch that `radio_tune_to()` adds into clk2. With
  it applied, the center of the I/Q always sits on the dial frequency.
- The I/Q is not spectrally inverted: a signal above the dial appears above
  center in the app.
- The usable span is about 30 kHz, limited by the crystal filter. The I/Q is
  96 kHz wide, or 48 kHz if the app asks for that rate.
- The DSP only runs while an app is connected. While transmitting, the app
  receives silence, so the stream keeps its timing.

## Using it

1. Start zBitx as usual. The console shows
   `hpsdr: Protocol 1 server on UDP 1024`.
2. In the SDR app, choose Protocol 1 / Hermes and run discovery. The zBitx
   shows up as MAC `00:1C:C0:A2:22:5B`.
3. Pick a sample rate of 48 or 96 kHz. If the app asks for 192 or 384 kHz,
   the zBitx still sends 96 kHz.
4. Retuning in the app retunes the radio.
5. Protocol 1 has no way for the radio to report its frequency to the app.
   If you also want the app to follow the zBitx's knob, set up the app's CAT
   to Hamlib rigctld at `<zbitx>:4532`.

## Settings (data/hw_settings.ini, any line)

| Key                | Default | Meaning |
|--------------------|---------|---------|
| `hpsdr_enable`     | 1       | 0 turns the server off |
| `hpsdr_allow_mox`  | 0       | 1 lets the app's MOX key the radio. TX audio comes from the loopback, as with Hamlib PTT; the app's own TX I/Q is ignored |
| `hpsdr_iq_gain_db` | 0       | gain applied to the I/Q before it is packed into 24 bits |
| `hpsdr_board_id`   | 1       | board type reported in discovery (1 = Hermes) |
| `iq_swap`          | 0       | 1 swaps I and Q, if a unit's spectrum turns out mirrored |

## Testing without an SDR app

    make test-iq                 # I/Q front end, synthetic tones
    make test-hpsdr              # server + fake audio thread + client, on this machine
    python3 tests/hpsdr_client.py --host <zbitx-ip> --freq 7074000 --seconds 5 --wav iq.wav

The client checks discovery, packet rate, sample rate and sequence gaps, and
reports where the strongest signal sits relative to center (numpy needed for
that). `--wav` saves the I/Q as a stereo WAV, with I on the left channel and
Q on the right.

On the radio, the quickest orientation check is to tune the zBitx in USB to
a carrier, such as a broadcast station, and run the client. A carrier
1 kHz above the dial should show up at about +1000 Hz. If it shows at
-1000 Hz, set `iq_swap=1`.

## Limits

- RX only. The app's TX I/Q is not transmitted.
- Only one app at a time. While one app is streaming, discovery reports the
  radio as busy to any other app.
- If an app stops sending for 3 seconds without a STOP command, streaming
  ends.
