#!/usr/bin/env python3
"""
serve_web.py - serve the web build to a browser on this machine.

    python3 tools/serve_web.py [--build DIR ...] [--port 8000]

Then open http://localhost:8000/ and import your game folder on the page:
it is copied into the browser's storage, where the game reads it. This
server holds no game file and sends none; it serves the page (web/), the
fonts and ffmpeg.wasm's core beside it, and the builds (yr.js and yr.wasm
from build-web, ra2.js and ra2.wasm from build-web-game: whichever exist),
with the two headers the game needs (Cross-Origin-Opener-Policy and
-Embedder-Policy: it runs on threads, which need SharedArrayBuffer).
tools/pack_web.py makes the same site as a folder for any web host.
"""
import argparse, os, sys, mimetypes
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
mimetypes.add_type('application/wasm', '.wasm')
mimetypes.add_type('text/javascript', '.js')


class Handler(SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'ra2-recomp-web/2'

    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write('%s %s\n' % (self.address_string(), fmt % args))

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()

    def translate_path(self, path):
        """web/ first, then the build directories (yr.js, ra2.js, their wasm)."""
        path = path.split('?', 1)[0].split('#', 1)[0]
        parts = [p for p in path.split('/') if p and p not in ('.', '..')]
        if not parts:
            parts = ['index.html']
        for base in [os.path.join(ROOT, 'web')] + self.server.builds:
            p = os.path.join(base, *parts)
            if os.path.isfile(p):
                return p
        return os.path.join(ROOT, 'web', *parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', action='append', help='a build directory (default: build-web and build-web-game, those that exist)')
    ap.add_argument('--port', type=int, default=8000)
    ap.add_argument('--bind', default='127.0.0.1')
    ap.add_argument('-v', '--verbose', action='store_true', help='log every request')
    a = ap.parse_args()
    builds = a.build or [d for d in (os.path.join(ROOT, 'build-web'), os.path.join(ROOT, 'build-web-game')) if os.path.isdir(d)]
    builds = [os.path.realpath(b) for b in builds if any(os.path.isfile(os.path.join(b, f)) for f in ('yr.js', 'ra2.js'))]
    if not builds:
        sys.exit('no yr.js or ra2.js in a build directory: run ./build-web.sh first (--build DIR)')
    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    srv.daemon_threads = True
    srv.builds, srv.verbose = builds, a.verbose
    print('serving web/ and %s' % ', '.join(builds))
    print('open  http://%s:%d/  and import your game folder there' % ('localhost' if a.bind == '127.0.0.1' else a.bind, a.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
