# The web host

`build-web/ra2.js` and `ra2.wasm` are the game compiled to WebAssembly with
Emscripten: the same lifted C and the same `runtime/win32hle` as the Linux
and macOS hosts, played in a browser tab. The game's files stay on your
machine: `tools/serve_web.py` serves your game folder to the page, which
fetches what the game reads, a piece at a time.

## Playing

```
./build-web.sh                                                     # build-web/yr.js + yr.wasm: Yuri's Revenge
BUILD_DIR=build-web-game CMAKE_ARGS=-DRA2_TARGET=game ./build-web.sh   # build-web-game/ra2.js + ra2.wasm: Red Alert 2
python3 tools/serve_web.py     # serves web/ and the builds there are, on localhost:8000
```

Open <http://localhost:8000/> and choose your game folder, the one with
`gamemd.exe` (and `game.exe`: the page offers each game it has a build for,
lit when the folder holds that game's program). It is copied once into the browser's storage, about 1 GB, or
2 GB with the movies (a choice on the page: without them the intro and the
campaign's cutscenes are skipped, and empty archives stand in for theirs),
and the game reads it from there. Nothing from the game is on the server or
sent to it: the page is static files, and each player brings their own copy.
The copy stays in that browser profile until "Remove the imported game
folder" on the page, or the site's data is cleared; settings and saves live
beside it.

`python3 tools/pack_web.py` writes the same site into `dist-web/`, a folder
of static files for any web host, with both games when both are built.
Mind what is in it: `yr.wasm` and `ra2.wasm` are the game's own code,
lifted and compiled, the same thing the native builds are. The project
never distributes those; each person builds them from their own copy. A
`dist-web/` on a public site would. Keep it on your own machine or a host
only you use, as you would the other builds. The game runs on threads, which need the
two cross-origin isolation headers: `dist-web/_headers` sets them on Netlify
and Cloudflare Pages, `vercel.json` on Vercel, and on a host that cannot set
headers, GitHub Pages among them, the page's service worker adds them, at
the cost of one extra reload on the first visit. The site must be on HTTPS
(or localhost).

Chrome, Edge and Firefox play it. Everything the game prints goes to the
browser's console.

Program arguments go in the page's query string: `?--filetrace`,
`?--inputtrace`, `?--sample` (where the game is, every 5 s), `?--watchdog&30`.

## Movies

The Bink movies (the intro, the menus' animations, the campaign's briefings
and cutscenes) play through the page: `web/movies.js` hands each movie's
bytes to `web/movie-worker.js`, which transcodes it to a WebM (VP8 and
Vorbis) with ffmpeg.wasm's core (`web/ffmpeg/`, the `@ffmpeg/core` 0.10
build, which has the Bink decoders) and streams the output as it is written,
so the movie starts after a moment and plays while the rest is still being
made (the transcode runs several times faster than the movie). A hidden
`<video>` plays it, and each of its frames is copied into the game's memory,
where the game's own Bink calls (`bink.c`'s browser branch) put it wherever
the game draws the movie. The finished WebM is kept in the browser's storage,
so a movie is transcoded once.

The core is the one published on npm as `@ffmpeg/core`, built from FFmpeg
with GPL and nonfree parts enabled; it stays in your browser, nothing is
redistributed from here.

## How it fits together

- **Memory.** One linear memory: a guest address is a program address
  (`RECOMP_FLAT_MEMORY`). The image goes at its own 0x00400000 and the
  program's own data, stack and heap start at 16 MB (`GLOBAL_BASE`), so
  `guest_mem.c`'s fixed reservations are ranges below that and its others are
  64 KB-aligned blocks from malloc.
- **Threads.** The game runs on a pthread (`PROXY_TO_PTHREAD`) and blocks as
  a Win32 program does; `CreateThread` and the multimedia timers are
  pthreads too (web workers). A worker cannot `sched_yield`, so the machine
  lock's hand-over sleeps instead (`host_lite.c`).
- **Files.** WASMFS: `/opfs` is the OPFS backend, the browser's storage:
  `/opfs/game` the imported game folder (web/import.js copies it there) and
  `/opfs/save` what the game writes, laid over the game folder as win32hle's
  overlay. Every lookup there is a message to another thread, so the path
  layer keeps directory listings until something is written. The fonts come
  through a fetch backend from beside the page. WASMFS cannot name a
  mounted backend's directory from `getcwd`, so `web.c` keeps the working
  directory itself.
- **Display.** SDL2's software renderer into the canvas: the GL renderer
  would need the WebGL context current on the game's thread, which SDL's
  EGL path (run on the page's thread) does not do.
- **Sound.** The DirectSound mixer fills a ring of stereo 16-bit frames in
  shared memory from a thread of its own; an AudioWorklet on the page plays
  the ring (`dsound.c`). SDL's own audio cannot be opened from a pthread.
- **Input.** SDL's DOM callbacks are queued to the game's thread and run
  from the pump (`screen.c`).
