/*
 * movies.js - the Bink movies, for the browser build.
 *
 * The game's bink.c (win32hle, its __EMSCRIPTEN__ branch) hands each movie's
 * bytes to ra2MovieOpen. The movie is transcoded once, by movie-worker.js
 * (ffmpeg.wasm), into an MP4 (H.264 + AAC) or, where the browser cannot play
 * that, a WebM (VP8 + Vorbis), kept in the browser's Origin Private File
 * System; then a <video> plays it, unseen, and each of its frames is drawn
 * into a canvas and copied, as RGBA, into the game's memory, where
 * BinkCopyToBuffer puts it wherever the game draws the movie (the intro
 * full-screen, the menus' animation in its window). The sound is the
 * video's own. A state block in the game's memory (uint32s: ready, frame,
 * ended, progress, w, h, rgba) tells the game's thread where things stand.
 */
(function () {
    var worker = null, jobs = {}, queue = Promise.resolve();
    var movies = {};

    function u32(st) { return new Uint32Array(wasmMemory.buffer, st, 7); }
    function setStatus(s) { if (RA2.ra2Status) RA2.ra2Status(s); }

    function getWorker() {
        if (worker) return worker;
        worker = new Worker('movie-worker.js');
        worker.onmessage = function (e) {
            var d = e.data;
            if (d.log !== undefined) { log('[ffmpeg] ' + d.log); return; }
            var job = jobs[d.key];
            if (!job) return;
            if (d.progress !== undefined) { if (job.progress) job.progress(d.progress); return; }
            if (d.chunk) { job.chunk(d.chunk); return; }
            delete jobs[d.key];
            if (d.error) job.reject(new Error(d.error)); else { log('[ffmpeg] done, ' + d.size + ' bytes: ' + (d.tail || []).join(' | ').slice(0, 400)); job.resolve(d.file); }
        };
        worker.onerror = function (e) { log('[movie] worker: ' + e.message); };
        return worker;
    }
    /* The transcode: chunk(bytes) as the WebM grows; resolves when it is whole. */
    function transcode(key, bytes, progress, chunk) {
        return new Promise(function (resolve, reject) {
            jobs[key] = { resolve: resolve, reject: reject, progress: progress, chunk: chunk };
            getWorker().postMessage({ key: key, bytes: bytes }, [bytes.buffer]);
        });
    }

    /* movies-v2: the earlier cache held the stream as it was written, which
     * has no duration, so its end was never seen */
    async function cacheDir() {
        var root = await navigator.storage.getDirectory();
        try { await root.removeEntry('movies', { recursive: true }); } catch (e) {}
        return root.getDirectoryHandle('movies-v2', { create: true });
    }
    async function cacheGet(name) {
        try { var fh = await (await cacheDir()).getFileHandle(name); return await fh.getFile(); } catch (e) { return null; }
    }
    async function cachePut(name, bytes) {
        try {
            var fh = await (await cacheDir()).getFileHandle(name, { create: true });
            var w = await fh.createWritable();
            await w.write(bytes);
            await w.close();
        } catch (e) { log('[movie] cannot cache ' + name + ': ' + e); }
    }

    function hash(bytes) {                                 /* FNV-1a of the first 4 KB */
        var h = 0x811c9dc5;
        for (var i = 0; i < bytes.length && i < 4096; i++) { h ^= bytes[i]; h = Math.imul(h, 0x01000193) >>> 0; }
        return h.toString(16);
    }

    /* The movie as a <video> source: the cached WebM, or, the first time, a
     * MediaSource fed the WebM as ffmpeg writes it, so the movie starts while
     * the rest is still being made; the whole is cached when it is done. */
    async function prepare(m, bytes) {
        var name = m.key + '.webm';
        var file = await cacheGet(name);
        if (file) return { url: URL.createObjectURL(file), live: false };
        setStatus('Preparing a movie for the first time...');
        var ms = new MediaSource(), sb = null, pending = [], finished = false, failed = null, head = [], headLen = 0, opened = false;
        function append() {
            if (!sb || sb.updating || !pending.length) return;
            try { sb.appendBuffer(pending.shift()); } catch (e) { failed = e; log('[movie] append: ' + e.name + ' ' + e.message); }
        }
        /* The SourceBuffer needs the codecs the file really has (ffmpeg may
         * leave a soundless movie without an audio track), so it waits for
         * the first cluster: the tracks come before it. */
        function tracks() {
            var all = new Uint8Array(headLen), o = 0;
            head.forEach(function (c) { all.set(c, o); o += c.length; });
            var cluster = -1, vorbis = false;
            for (var i = 0; i + 3 < all.length; i++) {
                if (all[i] === 0x1F && all[i + 1] === 0x43 && all[i + 2] === 0xB6 && all[i + 3] === 0x75) { cluster = i; break; }
            }
            if (cluster < 0 && !finished) return null;
            var upto = cluster < 0 ? all.length : cluster;
            for (var j = 0; j + 7 < upto; j++)
                if (all[j] === 0x41 && all[j + 1] === 0x5F && all[j + 2] === 0x56 && all[j + 3] === 0x4F && all[j + 4] === 0x52 && all[j + 5] === 0x42 && all[j + 6] === 0x49 && all[j + 7] === 0x53) { vorbis = true; break; }   /* "A_VORBIS" */
            return { codecs: vorbis ? 'video/webm; codecs="vp8, vorbis"' : 'video/webm; codecs="vp8"', data: all };
        }
        var t0 = performance.now(), chunks = 0;
        function start() {
            if (sb || !opened) return;
            var t = tracks();
            if (!t) return;
            try {
                log('[movie] ' + m.key + ': tracks after ' + chunks + ' chunks, ' + Math.round(performance.now() - t0) + ' ms, ' + t.codecs);
                sb = ms.addSourceBuffer(t.codecs);
                sb.addEventListener('updateend', function () {
                    if (pending.length) append();
                    else if (finished && ms.readyState === 'open') { try { ms.endOfStream(); } catch (e) {} }
                });
                pending.unshift(t.data);
                head = []; headLen = 0;
                append();
            } catch (e) { failed = e; log('[movie] MediaSource: ' + e.name + ' ' + e.message); }
        }
        ms.addEventListener('sourceopen', function () { opened = true; start(); });
        var done = transcode(m.key, bytes, function (p) {
            u32(m.st)[3] = Math.round(p * 1000);
            setStatus('Preparing a movie: ' + Math.round(p * 100) + '%');
        }, function (chunk) {
            if (++chunks === 1) log('[movie] ' + m.key + ': first chunk after ' + Math.round(performance.now() - t0) + ' ms, ' + chunk.length + ' bytes');
            if (!sb) { head.push(chunk); headLen += chunk.length; start(); }
            else { pending.push(chunk); append(); }
        });
        done.then(function (file) {
            finished = true;
            m.finished = true;
            setStatus('');
            if (!sb) start();
            if (sb && !sb.updating && !pending.length && ms.readyState === 'open') { try { ms.endOfStream(); } catch (e) {} }
            if (file) cachePut(name, file);
        }).catch(function (e) {
            log('[movie] ' + e.message.split('\n')[0]);
            finished = true;
            if (ms.readyState === 'open') { try { ms.endOfStream('decode'); } catch (x) {} }
        });
        m.live = done;
        return { url: URL.createObjectURL(ms), live: true };
    }

    function pump(m) {
        var v = m.video;
        if (!v || m.closed) return;
        var ctx = m.ctx;
        ctx.drawImage(v, 0, 0, m.w, m.h);
        var img = ctx.getImageData(0, 0, m.w, m.h);
        new Uint8Array(wasmMemory.buffer, m.rgba, m.w * m.h * 4).set(img.data);
        var st = u32(m.st);
        var frame = Math.floor(v.currentTime * m.fps) + 1;
        if (frame > m.frames) frame = m.frames;
        Atomics.store(st, 1, frame);
        if (v.ended || frame >= m.frames) Atomics.store(st, 2, 1);
    }
    /* A movie whose data is all there but that stops short of its last frame
     * (a file with no duration, a rounding of its length) has ended too. */
    function watch(m) {
        var last = -1, still = 0;
        m.timer = setInterval(function () {
            var v = m.video;
            if (!v || m.closed) return;
            if (u32(m.st)[2]) return;
            if (!v.paused && v.currentTime === last && (m.finished || !m.isLive)) {
                if (++still >= 3) { Atomics.store(u32(m.st), 1, m.frames); Atomics.store(u32(m.st), 2, 1); }
            } else still = 0;
            last = v.currentTime;
        }, 500);
    }
    function schedule(m) {
        var v = m.video;
        if (m.closed) return;
        if (v.requestVideoFrameCallback) v.requestVideoFrameCallback(function () { pump(m); schedule(m); });
        else requestAnimationFrame(function () { pump(m); schedule(m); });
    }

    /* (id, bytes at ptr..ptr+len, state block, w, h, fps, frames): copies the bytes now; the rest is async */
    RA2.ra2MovieOpen = function (id, ptr, len, st, w, h, fps, frames) {
        var bytes = new Uint8Array(wasmMemory.buffer, ptr, len).slice();
        var m = { id: id, st: st, w: w, h: h, fps: fps, frames: frames, rgba: u32(st)[6], key: hash(bytes) + '_' + len, closed: false };
        movies[id] = m;
        var canvas = document.createElement('canvas');
        canvas.width = w; canvas.height = h;
        m.ctx = canvas.getContext('2d', { willReadFrequently: true });
        prepare(m, bytes).then(function (src) {
            if (m.closed) return;
            var v = document.createElement('video');
            v.src = src.url; v.preload = 'auto'; v.playsInline = true; v.volume = m.volume === undefined ? 1 : m.volume;
            v.style.display = 'none';
            document.body.appendChild(v);
            m.video = v;
            m.isLive = src.live;
            watch(m);
            v.addEventListener('ended', function () { Atomics.store(u32(m.st), 1, m.frames); Atomics.store(u32(m.st), 2, 1); });
            v.addEventListener('waiting', function () { m.waiting = true; });
            v.addEventListener('playing', function () { m.waiting = false; });
            v.addEventListener('error', function () {
                log('[movie] playback error ' + (v.error ? v.error.code + ' ' + v.error.message : '') + (src.live ? ' (live)' : ' (cached)'));
                Atomics.store(u32(m.st), 0, 2);
            });
            v.addEventListener('canplay', function onReady() {
                v.removeEventListener('canplay', onReady);
                log('[movie] ' + m.key + ': can play');
                setStatus('');
                var p = v.play();
                if (p && p.catch) p.catch(function (e) { log('[movie] play: ' + e); });
                schedule(m);
                Atomics.store(u32(m.st), 0, 1);
            });
        }).catch(function (e) {
            log('[movie] ' + e);
            setStatus('Movie skipped: ' + e.message);
            Atomics.store(u32(m.st), 0, 2);
        });
        return 1;
    };
    /* (id, what, value): 1 pause/resume (value 1 pauses), 2 volume (0..1000), 3 seek (frame), 4 close */
    RA2.ra2MovieCtl = function (id, what, value) {
        var m = movies[id];
        if (!m) return;
        var v = m.video;
        if (what === 1) { if (v) { if (value) v.pause(); else v.play().catch(function () {}); } }
        else if (what === 2) { m.volume = value / 1000; if (v) v.volume = m.volume; }
        else if (what === 3) {
            if (v) {
                v.currentTime = Math.max(0, (value - 1) / m.fps);
                Atomics.store(u32(m.st), 1, value);
                Atomics.store(u32(m.st), 2, 0);
                v.play().catch(function () {});
            }
        } else if (what === 4) {
            m.closed = true;
            if (m.timer) clearInterval(m.timer);
            if (v) { v.pause(); v.removeAttribute('src'); v.load(); v.remove(); }
            delete movies[id];
        }
    };
})();
