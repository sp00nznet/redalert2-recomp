/* Sound without DirectSound: the game's IDirectSound and IDirectSoundBuffer
 * answered by the host and mixed into a WASAPI stream (audio_wasapi.c).
 * Opt-in (--audio wasapi, or audio=wasapi under [present] in ra2.ini);
 * DirectSound stays the default. */
#pragma once
#include <windows.h>
#include <dsound.h>
#include <stdint.h>

/* Open the output device and start the mixer (once). 0 when there is no
 * device: the mixer still runs, silent, at 48000 Hz, so the game's play
 * cursors still move. */
int audio_wasapi_start(void);

/* DirectSoundCreate's answer: an IDirectSound of ours. */
HRESULT audio_wasapi_create(LPDIRECTSOUND* out);

/* The output's gain, 0..1 (--mute is 0). The recording tap is before it. */
void audio_wasapi_gain(float gain);

/* The recording tap: the mix as stereo 16-bit at audio_wasapi_rate(), from
 * when audio_wasapi_tap(1) was called. Reads at most `frames`; returns how
 * many it read. */
int  audio_wasapi_rate(void);
void audio_wasapi_tap(int on);
int  audio_wasapi_tap_read(int16_t* out, int frames);

/* host.c: whether a guest timer callback is running on a thread other than
 * this one (a buffer it may be using is not freed then; host.c timeKillEvent) */
int  host_guest_timer_busy(void);
