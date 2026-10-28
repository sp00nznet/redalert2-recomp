# Notice

The MIT licence in [LICENSE](LICENSE) covers what is ours in this repository:
the host, the setup scripts, the tools, the analysis summaries and the
documentation. It does not cover the games.

*Command & Conquer: Red Alert 2* and *Yuri's Revenge* are © 2000-2001
Westwood Studios / Electronic Arts, and are not ours to license. Their
executables, data, movies, music and art are not in this repository and are
excluded by `.gitignore`.

- You need your own copy of the game to use any of this. Setup links or copies
  your install into `game/`; the pipeline reads it from there.
- Lifted code is a derivative work of the binary it came from. Anything the
  pipeline writes into `work/` or `src/recomp/gen*/` carries whatever licence
  the original carries, not this one. The MIT grant stops at our own work.

## ffmpeg.wasm (the web build's movies)

`web/ffmpeg/` holds the `@ffmpeg/core` 0.10.0 build of FFmpeg for
WebAssembly (https://github.com/ffmpegwasm/ffmpeg.wasm), used by the web
build to transcode the game's Bink movies in the browser. FFmpeg is licensed
under the LGPL/GPL; that build enables GPL and nonfree components. It runs
only in the player's browser.
