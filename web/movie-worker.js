/*
 * movie-worker.js - transcodes a Bink movie to a WebM (VP8 + Vorbis) with
 * ffmpeg.wasm's core (web/ffmpeg/, the @ffmpeg/core 0.10 build: the Bink
 * decoders, libvpx, libvorbis), off the page's thread, and streams the
 * output as it is written: ffmpeg runs on a thread of the core's own, so
 * this thread reads the growing file between the core's proxied writes.
 *
 * In: { key, bytes }. Out: { key, chunk } as the WebM grows, { key, progress }
 * now and then, then { key, done } or { key, error }; { log } lines besides.
 */
importScripts('ffmpeg/ffmpeg-core.js');

var core = null, main = null, duration = 0, progressKey = null, tail = [], ended = null, logged = 0;

function onLog(line) {
    if (typeof line !== 'string') return;
    if (line === 'FFMPEG_END') { if (ended) { var f = ended; ended = null; f(); } return; }
    tail.push(line);
    if (tail.length > 30) tail.shift();
    var m = /Duration: (\d+):(\d+):(\d+\.\d+)/.exec(line);
    if (m) duration = (+m[1]) * 3600 + (+m[2]) * 60 + (+m[3]);
    m = /time=(\d+):(\d+):(\d+\.\d+)/.exec(line);
    if (m && duration > 0 && progressKey) {
        var t = (+m[1]) * 3600 + (+m[2]) * 60 + (+m[3]);
        postMessage({ key: progressKey, progress: Math.min(1, t / duration) });
        return;
    }
    if (logged++ < 40 && !/^(frame=|size=)|Non-monotonous DTS|backward in time/.test(line)) postMessage({ log: line });
}

async function load() {
    /* a fresh core per job: ffmpeg's main ends with exit() */
    core = await createFFmpegCore({
        mainScriptUrlOrBlob: 'ffmpeg/ffmpeg-core.js',
        print: onLog,
        printErr: onLog,
        locateFile: function (p, prefix) { return 'ffmpeg/' + p; },
    });
    main = core.cwrap('main', 'number', ['number', 'number']);
}

/* ffmpeg's main runs on a thread of the core's own (a pthreads build): the
 * call returns at once and the run is over when the core logs FFMPEG_END. */
function run(args) {
    return new Promise(function (resolve, reject) {
        ended = resolve;
        var argv = core._malloc(args.length * 4);
        args.forEach(function (a, i) {
            var p = core._malloc(a.length + 1);
            core.writeAsciiToMemory(a, p);
            core.setValue(argv + 4 * i, p, 'i32');
        });
        try {
            main(args.length, argv);
        } catch (x) {
            if (!(x && typeof x === 'object' && 'status' in x && x.status === 0)) reject(x);
        }
    });
}

/* What the output file has grown by since last time, or null. */
function grown(name, sent) {
    var size;
    try { size = core.FS.stat(name).size; } catch (e) { return null; }
    if (size <= sent) return null;
    var fd = core.FS.open(name, 'r');
    var buf = new Uint8Array(size - sent);
    var n = core.FS.read(fd, buf, 0, buf.length, sent);
    core.FS.close(fd);
    return n > 0 ? buf.subarray(0, n) : null;
}

onmessage = async function (e) {
    var job = e.data, inName = job.key + '.bik', outName = job.key + '.webm', sent = 0, timer = null;
    function flush() {
        var more = grown(outName, sent);
        if (more) { sent += more.length; postMessage({ key: job.key, chunk: more }); }
    }
    try {
        await load();
        core.FS.writeFile(inName, job.bytes);
        duration = 0; tail = []; logged = 0;
        progressKey = job.key;
        /* VP8 at its fastest, a keyframe every second so a stream may start at
         * any cluster; the sound resampled with its timestamps smoothed (Bink's
         * audio packets are not quite monotonous), as Vorbis. The clusters are
         * written as they fill, so the page can play the file as it grows; at
         * the end ffmpeg patches in the duration and the cues, and the whole
         * file, with those, goes to the page for its cache. */
        var args = ['ffmpeg', '-nostdin', '-y', '-i', inName,
                    '-c:v', 'libvpx', '-deadline', 'realtime', '-cpu-used', '16', '-crf', '12', '-b:v', '3M',
                    '-pix_fmt', 'yuv420p', '-g', '15', '-threads', '1',
                    '-af', 'aresample=async=1000', '-c:a', 'libvorbis', '-q:a', '4',
                    '-f', 'webm', '-cluster_time_limit', '500', outName];
        timer = setInterval(flush, 250);
        await run(args);
        clearInterval(timer);
        flush();
        progressKey = null;
        var whole = core.FS.readFile(outName);
        try { core.FS.unlink(inName); core.FS.unlink(outName); } catch (x) {}
        core = null;
        postMessage({ key: job.key, done: true, size: sent, tail: tail.filter(function (l) { return !/^frame=/.test(l); }).slice(-4), file: whole }, [whole.buffer]);
    } catch (err) {
        if (timer) clearInterval(timer);
        progressKey = null;
        core = null;
        var text = err && typeof err === 'object' ? (err.message || ('status' in err ? 'ffmpeg exited with ' + err.status : JSON.stringify(err))) : String(err);
        postMessage({ key: job.key, error: text + '\n' + tail.join('\n') });
    }
};
