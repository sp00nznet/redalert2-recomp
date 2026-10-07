#!/usr/bin/env python3
"""Red Alert 2: Yuri's Revenge conformance harness (REPO_RULES section 9).

Two fixed corpora, one pass/fail count each, compared against the committed
baseline in conformance.json; a regression fails the run:

* **Boot milestones**: a headless run of build/ra2.exe, scored by the
  lines the host prints at each stage. The original game is the ground truth:
  each milestone is something it does on every start.
* **Lift health**: from the generated tree. Lift errors, bodies with no
  terminator, and RECOMP_ITAIL labels that cannot resolve at run time
  (their target is not in the dispatch table).

The game is not in the repo. Without game/ and build/ra2.exe this skips
with a message and exits 0, so it can sit in CI without the corpus.

    py -3 tools/conformance.py              # run, compare, print the table
    py -3 tools/conformance.py --update     # ...and accept the result as the baseline
"""
import argparse
import glob
import json
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# RA2_TARGET=game: Red Alert 2 itself (game.exe), its build and its own baseline.
GAME = os.environ.get('RA2_TARGET') == 'game'
# RA2_EXE: another build of the host to test (a clang-cl build, an A/B variant).
HOST = os.environ.get('RA2_EXE') or os.path.join(ROOT, 'build-game' if GAME else 'build', 'ra2.exe')
GEN = os.path.join(ROOT, 'src', 'recomp', 'gen_game' if GAME else 'gen')
STATS = os.path.join(ROOT, 'work', *(['game'] if GAME else []), 'lift_stats.json')
BASELINE = os.path.join(ROOT, 'conformance-game.json' if GAME else 'conformance.json')
# Off Windows the host is a cross build (build.sh) that runs under Wine:
# CrossOver's Steam bottle on a Mac, wine on Linux, or RA2_WINE, the launcher
# command.
WINE = [] if os.name == 'nt' else shlex.split(os.environ.get('RA2_WINE') or (
    '/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine --bottle Steam'
    if sys.platform == 'darwin' else 'wine'))
# The game folder's wsock32.dll is IPXEmu, the network games' IPX; Wine would
# load its own, which has none (error 10047 creating the IPX socket).
if WINE:
    os.environ.setdefault('WINEDLLOVERRIDES', 'wsock32=n,b')


def host_path(p):
    """A path as the host takes it: Wine's Z: is the Mac's (or Linux's) /."""
    return p if os.name == 'nt' else 'Z:' + os.path.abspath(p).replace('/', '\\')

# (name, what the host prints when it is reached). Order is boot order.
MILESTONES = [
    ('image mapped and imports bound', r'guest exe '),
    ('entry point entered', r'entering 0x00785AA0' if GAME else r'entering 0x007CD80F'),
    ('window created', r'\[headless\] CreateWindowExA\('),
    ('DirectDraw created', r'\[headless\] DirectDrawCreate -> 0x00000000'),
    ('primary surface created', r'primary -> 0x00000000'),
    ('first frame blitted', r'\[headless\] frame 1 blitted'),
    # Bink copies the intro straight into the primary; --record samples it.
    ('intro video plays (5+ distinct frames sampled)', None),
    # Dialog 0xE2, after the intro: the menu's own movie starts (Red Alert 2
    # logs no such line, so for it the menu's dialog opening).
    ('main menu reached', r'\[dialog\] open 0xE2' if GAME else r'\[game\] Looping movie'),
]


def distinct_frames(out):
    return len(set(re.findall(r'\[record\] frame \d+ (?:at \S+ )?checksum ([0-9A-F]{8})', out)))


def boot(seconds):
    try:
        p = subprocess.run(WINE + [HOST, '--headless', '--run', '--mute', '--debuglog', '--watchdog', str(seconds),
                                   '--record', host_path(os.path.join(ROOT, 'work', 'conformance.mp4'))],
                           cwd=ROOT, capture_output=True, text=True, errors='replace',
                           timeout=seconds + 60)
        out, code = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        out, code = (e.stdout or '') + (e.stderr or ''), 'timeout'
        out = out if isinstance(out, str) else out.decode(errors='replace')
    passed = [name for name, pat in MILESTONES
              if (re.search(pat, out) if pat else distinct_frames(out) >= 5)]
    last = [l for l in out.splitlines() if l.startswith(('===', '[not-lifted]', '[watchdog]'))]
    return passed, code, last[:2]


def lift_health():
    stats = json.load(open(STATS))
    disp = set(re.findall(r'\{ 0x([0-9A-F]{8})u,', open(os.path.join(GEN, 'recomp_dispatch.c')).read()))
    unresolved = sum(1 for fn in glob.glob(os.path.join(GEN, 'recomp_0*.c'))
                     for t in re.findall(r'L_([0-9A-F]{8}): RECOMP_ITAIL', open(fn).read())
                     if t not in disp)
    return {'lifted': stats['lifted'], 'errors': stats['errors'],
            'no_terminator': stats['no_terminator'], 'unresolved_itail': unresolved}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--update', action='store_true', help='accept this run as the baseline')
    ap.add_argument('--seconds', type=int, default=300,
                    help='headless run length (the intro alone is four minutes)')
    args = ap.parse_args()
    if not (os.path.exists(HOST) and os.path.isdir(os.path.join(ROOT, 'game'))):
        print('conformance: skipped -- needs game/ (your copy) and build/ra2.exe '
              '(README, Building from source)')
        return 0

    passed, code, last = boot(args.seconds)
    health = lift_health()
    now = {'milestones': len(passed), 'of': len(MILESTONES), **health}
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else None

    print('boot milestones: %d/%d  (exit %s)' % (len(passed), len(MILESTONES), code))
    for name, _ in MILESTONES:
        print('  [%s] %s' % ('x' if name in passed else ' ', name))
    for l in last:
        print('  stopped: ' + l)
    print('lift: %(lifted)d functions, %(errors)d errors, %(no_terminator)d with no '
          'terminator, %(unresolved_itail)d unresolvable ITAIL labels' % health)

    worse = []
    if base:
        if now['milestones'] < base['milestones']:
            worse.append('milestones %d -> %d' % (base['milestones'], now['milestones']))
        for k in ('errors', 'no_terminator', 'unresolved_itail'):
            if now[k] > base[k]:
                worse.append('%s %d -> %d' % (k, base[k], now[k]))
    if args.update or not base:
        json.dump(now, open(BASELINE, 'w'), indent=1)
        print('baseline written to ' + os.path.basename(BASELINE))
    if worse:
        print('REGRESSION: ' + '; '.join(worse))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
