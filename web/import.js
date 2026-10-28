/*
 * import.js - the player's own game folder, copied once into the browser's
 * storage (the Origin Private File System, as game/), where the game then
 * reads it. Nothing goes to the server: the page is static, the files stay
 * on the player's machine. web.c mounts game/ when it holds the game's
 * program; save/ beside it takes what the game writes.
 *
 * Sources: a folder picked with the File System Access API (Chrome, Edge),
 * or a folder chosen through an <input webkitdirectory> (every browser).
 */
var RA2Import = (function () {
    var EXES = ['gamemd.exe', 'game.exe', 'blowfish.dll', 'blowfish.tlb'];
    var SKIP_EXT = ['.exe', '.dll', '.old', '.zip', '.lnk', '.url', '.ico', '.msi', '.tlb', '.pdf', '.txt', '.htm', '.html'];
    var MOVIES = ['movies01.mix', 'movies02.mix', 'movmd03.mix'];

    function wanted(name, movies) {
        var low = name.toLowerCase(), dot = low.lastIndexOf('.'), ext = dot >= 0 ? low.slice(dot) : '';
        if (EXES.indexOf(low) >= 0) return true;
        if (SKIP_EXT.indexOf(ext) >= 0) return false;
        if (!movies && MOVIES.indexOf(low) >= 0) return false;
        return !low.startsWith('.');
    }

    async function root() { return navigator.storage.getDirectory(); }

    /* The games' programs in the imported folder: gamemd.exe (Yuri's
       Revenge), game.exe (Red Alert 2), those there. */
    async function exes() {
        try {
            var dir = await (await root()).getDirectoryHandle('game');
            var names = [];
            for await (var [n] of dir.entries()) names.push(n.toLowerCase());
            return ['gamemd.exe', 'game.exe'].filter(function (x) { return names.indexOf(x) >= 0; });
        } catch (e) { return []; }
    }
    async function imported() { return (await exes()).length > 0; }
    async function remove() {
        try { await (await root()).removeEntry('game', { recursive: true }); } catch (e) {}
    }

    /* A list of { path, size, open() -> Blob } from a picked directory handle. */
    async function listHandle(dir, prefix, out) {
        for await (var [name, h] of dir.entries()) {
            if (h.kind === 'directory') { await listHandle(h, prefix + name + '/', out); continue; }
            var f = await h.getFile();
            out.push({ path: prefix + name, size: f.size, open: function (file) { return function () { return file; }; }(f) });
        }
        return out;
    }
    function listFiles(files) {
        return Array.prototype.map.call(files, function (f) {
            var rel = f.webkitRelativePath || f.name;
            var slash = rel.indexOf('/');
            return { path: slash >= 0 ? rel.slice(slash + 1) : rel, size: f.size, open: function () { return f; } };
        });
    }
    /* The copy: entries in, game/ out; progress(done bytes, total bytes, name). */
    async function copy(entries, movies, progress) {
        var names = entries.filter(function (e) { return e.path.indexOf('/') < 0; }).map(function (e) { return e.path.toLowerCase(); });
        entries = entries.filter(function (e) { return e.path.indexOf('/') < 0 && wanted(e.path, movies); });   /* the folder's own files */
        if (names.indexOf('gamemd.exe') < 0 && names.indexOf('game.exe') < 0) throw new Error('that folder has no gamemd.exe or game.exe');
        if (names.indexOf('blowfish.dll') < 0) throw new Error('that folder has no Blowfish.dll');
        var total = entries.reduce(function (a, e) { return a + e.size; }, 0), done = 0;
        var r = await root();
        try { await r.removeEntry('game', { recursive: true }); } catch (e) {}
        var dir = await r.getDirectoryHandle('game', { create: true });
        for (var i = 0; i < entries.length; i++) {
            var e = entries[i];
            progress(done, total, e.path);
            var fh = await dir.getFileHandle(e.path, { create: true });
            var w = await fh.createWritable();
            await w.write(await e.open());
            await w.close();
            done += e.size;
        }
        /* Without the movies, their archives are still looked for at start,
         * so an empty archive stands in for each (a MIX with no entries). */
        if (!movies) {
            for (var k = 0; k < MOVIES.length; k++) {
                if (names.indexOf(MOVIES[k]) < 0) continue;
                var sh = await dir.getFileHandle(MOVIES[k], { create: true });
                var sw = await sh.createWritable();
                await sw.write(new Uint8Array(10));
                await sw.close();
            }
        }
        progress(total, total, '');
        return { files: entries.length, bytes: total };
    }

    return {
        imported: imported, exes: exes, remove: remove, copy: copy,
        listHandle: function (h) { return listHandle(h, '', []); }, listFiles: listFiles,
        canPick: typeof window.showDirectoryPicker === 'function',
    };
})();
