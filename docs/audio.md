# Sound

The game plays its sound through DirectSound: one `IDirectSound` from
`DirectSoundCreate` (imported by ordinal), and secondary buffers it locks,
fills and plays. Bink plays a movie's sound through the same object (the game
hands it over with `BinkSetSoundSystem(BinkOpenDirectSound, ds)`).

The host has two backends:

| | DirectSound (default) | WASAPI (`--audio wasapi`) |
|---|---|---|
| Who mixes | Windows' DirectSound (itself on WASAPI since Vista) | the host, `src/runtime/audio_wasapi.c` |
| Out of focus | every buffer gets `DSBCAPS_GLOBALFOCUS` (the presenter's window is the one in front) | always plays |
| `--mute` | the process's waveOut volume at 0, which Windows keeps for the exe after it exits | the mixer's gain at 0; nothing outlives the run |
| `--record` | picture only | picture and sound (the mix before `--mute`'s gain) |

Pick it with `--audio wasapi` or, for every run, in `ra2.ini` beside `ra2.exe`:

```ini
[present]
audio=wasapi
```

`--audio dsound` (or `audio=dsound`) is DirectSound. The default stays
DirectSound until the WASAPI mix has been listened to on more machines; to
flip it, change the `"dsound"` default in `host.c` (the `audio=` read in
`main`).

## What the game calls

Measured with `--audio-trace` (each `IDirectSound`/`IDirectSoundBuffer`
vtable slot replaced by a counting thunk, so the game's own calls and Bink's
are both seen) on `skirmish-build`, `movies-credits`, `movies-sneakpeeks` and
`campaign-allied`, Yuri's Revenge on DirectSound:

- **The device**: `SetCooperativeLevel` once, `CreateSoundBuffer`, `GetCaps`.
  No `QueryInterface`, no `DuplicateSoundBuffer`, nothing of DirectSound 8
  or 3D.
- **The primary**: created with no format, then `SetFormat`, `Play`, `Stop`.
- **The game's buffers**: 22050 Hz 16-bit, mono (45056 bytes) and stereo
  (86016 bytes), `CTRLVOLUME | CTRLPAN | CTRLFREQUENCY`; 21 made in a 230 s
  skirmish. Per buffer: `Lock`/`Unlock`, `Play`, `Stop`,
  `SetCurrentPosition`, and `SetVolume`/`SetPan`/`SetFrequency` about 50
  times a second each while playing; `GetStatus` to see one finish.
- **Bink's buffer**: 44100 Hz 16-bit stereo, 131072 bytes, with
  `GETCURRENTPOSITION2`. Bink polls `GetCurrentPosition` in a loop: 22
  million calls in a 90 s movie run. Each call is a lock and a few reads.

`[audio-trace] first ...` names the module each method was first called from
(`ra2.exe` is the lifted game, through the native bridge; `binkw32.dll` is
Bink). `[audio-trace] count v:Method n` every 10 s: v is the vtable (0 the
device, 1 the primary, 2 the secondaries).

## The WASAPI backend

Ported from pcrecomp's `runtime/win32hle/dsound.c` (the same 11 + 21
methods and the same mixer), as native COM objects:

- A buffer is host memory the game locks and writes, as on Windows. The
  mixer reads it at the buffer's frequency (linear interpolation), volume and
  pan, and moves its play cursor in real time; the write cursor is 10 ms
  ahead of it, as DirectSound's.
- The mix is at the device's rate and format (shared mode, from
  `GetMixFormat`, float or 16-bit), on a thread of its own woken by WASAPI's
  event, with a 20 ms buffer.
- No device (none, or unplugged): the mix runs at 48000 Hz into nothing, paced
  by the clock, so the game's cursors still move; every 5 s it tries the
  default device again.
- A buffer released while a guest timer callback is running (on another
  thread) waits in a graveyard until none is: the host's rule since closing a
  movie faulted in DirectSound (`timeKillEvent` in `host.c`).
- At start the session's channel volumes go back to full: DirectSound's
  `--mute` set them, and Windows kept them for the exe (the Volume mixer shows
  only the master volume, which is the player's and is left alone).

## Sound in --record

With `--audio wasapi`, `--record` taps the mix (before the gain, so a muted
run still records its sound) into a ring, and a thread writes it as 16-bit
stereo through a named pipe that ffmpeg reads as its second input, at the
clock's pace, with silence where the mixer has nothing. The recorder now
writes 30 pictures a second exactly (it was every 33 ms, 1% fast), so the two
stay together.

The playtest cases `audio-wasapi-skirmish` and `audio-wasapi-movie` run with
the backend and fail on a recording with no sound or a silent one (`ffmpeg
-af volumedetect`, `max_volume` below -60 dB).

Limits: a picture the recorder could not read (the primary locked, a mode
change) is skipped, so a long recording's picture can fall behind its sound by
that much. If the device changes rate mid-run, a recording keeps the rate it
started with.
