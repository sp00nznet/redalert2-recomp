#!/usr/bin/env python3
"""
pack_web.py - the web build as a folder of static files, ready for any web
host: the page and its scripts, the fonts, ffmpeg.wasm's core, and the
builds (yr.js and yr.wasm for Yuri's Revenge, ra2.js and ra2.wasm for Red
Alert 2: whichever are built). No game file goes in: each player imports
their own copy on the page, into their browser.

    python3 tools/pack_web.py [--build DIR ...] [--out dist-web]

The game needs the two cross-origin isolation headers (it runs on threads).
The folder carries a `_headers` file (Netlify, Cloudflare Pages) and a
`vercel.json` (Vercel) that set them; on a host that cannot set headers
(GitHub Pages) the page's service worker adds them itself, with one extra
reload on the first visit.
"""
import argparse, os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HEADERS = ('Cross-Origin-Opener-Policy', 'same-origin'), ('Cross-Origin-Embedder-Policy', 'require-corp'), ('Cross-Origin-Resource-Policy', 'same-origin')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', action='append', help='a build directory (default: build-web and build-web-game, those that exist)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist-web'))
    a = ap.parse_args()
    builds = a.build or [d for d in (os.path.join(ROOT, 'build-web'), os.path.join(ROOT, 'build-web-game')) if os.path.isdir(d)]
    found = []
    for b in builds:
        for name in ('yr', 'ra2'):
            if os.path.isfile(os.path.join(b, name + '.js')) and os.path.isfile(os.path.join(b, name + '.wasm')):
                found.append((b, name))
    if not found:
        sys.exit('no yr.js or ra2.js with its wasm in %s: run ./build-web.sh first' % ', '.join(builds))
    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    shutil.copytree(os.path.join(ROOT, 'web'), a.out)
    for b, name in found:
        for f in (name + '.js', name + '.wasm'):
            shutil.copy2(os.path.join(b, f), os.path.join(a.out, f))
    print('games: ' + ', '.join(name for _, name in found))
    with open(os.path.join(a.out, '_headers'), 'w') as f:
        f.write('/*\n' + ''.join('  %s: %s\n' % h for h in HEADERS))
    with open(os.path.join(a.out, 'vercel.json'), 'w') as f:
        f.write('{ "headers": [ { "source": "/(.*)", "headers": [ ' + ', '.join('{ "key": "%s", "value": "%s" }' % h for h in HEADERS) + ' ] } ] }\n')
    total = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(a.out) for n in ns)
    print('%s: %d MB; upload the folder as is' % (a.out, total // 1000000))


if __name__ == '__main__':
    main()
