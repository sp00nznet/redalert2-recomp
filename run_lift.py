#!/usr/bin/env python3
"""Red Alert 2: Yuri's Revenge lift driver: work/functions.json -> src/recomp/gen/.

Drives pcrecomp's shared `tools/lift/generate.py` + `lift32`, the same shape
as The Movies' and Force Commander's run_lift.py (docs/architecture.md):

* **Closure-limited lifting** (`generate.closure`). Lift the call-graph closure
  from the OEP and give every other catalogued function a stub that aborts
  naming itself, so a run says exactly what to lift next. Bring-up uses --all
  (gamemd.exe is ~1M instructions, which compiles in minutes); the closure is
  there for bisecting a bad lift.
* **Extents by walking the branches** (`generate.true_extent`), capped by
  reach rather than the catalog's clamp; its docstring has why.

Every vtable slot RTTI names is injected as an entry (work/rtti_seeds.json):
MSVC's vtordisp adjustor thunks are reachable only through a vtable, so no
disassembler pass names them, and an unresolved slot answers eax = 0.

    py -3 run_lift.py                      # OEP closure, 3000 functions
    py -3 run_lift.py --max 20000
    py -3 run_lift.py --roots 0x007CD80F,0x004A1C30
    py -3 run_lift.py --virtual --max 60000   # plus every vtable method
    py -3 run_lift.py --all
"""
import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks another toolkit checkout, the same knob CMakeLists.txt has:
# the lifter and the runtime header must come from the same tree.
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'tools')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from generate import (EXTENT_REACH, closure, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

EXE = os.path.join(_HERE, 'game', 'gamemd.exe')
CATALOG = os.path.join(_HERE, 'work', 'functions.json')
SEEDS = os.path.join(_HERE, 'work', 'rtti_seeds.json')
OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')
STATS = os.path.join(_HERE, 'work', 'lift_stats.json')

# Entries the catalog does not find, so a clean checkout lifts them too
# (docs/bringup.md 1 and 11). The CRT's static constructors are reachable only
# through the _initterm table; two window procedures are named only by a
# `mov reg, imm` behind a jump table. An unresolved ICALL answers eax = 0 and
# carries on, so a missing one shows up far from its cause.
RUN_SEEDS = [
    # static constructors
    0x0040FF90, 0x00410010, 0x0045B110, 0x004F4100, 0x005394C0, 0x0055F760,
    0x006A4AF0, 0x006E88B0, 0x006F2A50, 0x0071B740, 0x00747090, 0x0076F700,
    0x00777380,
    # window procedures (skirmish setup and its sub-dialogs)
    0x006163A0, 0x00618D40,
]

# Functions whose body is the host's instead of the lift: the host defines
# ra2_hook_<name>(void) and it runs like an import shim (arguments from
# g_esp, pops its own return address). src/runtime/host.c.
HOOKS = {
    # The debug printf. Compiled out of the retail build (a bare `ret`), so
    # the host gives it a body: --debuglog prints the game's own log, which
    # says what init is doing at a fraction of --argtrace's cost.
    0x004068E0: 'debuglog',
}

# ---- remaster patches: C emitted after one instruction ---------------------
# The lift stays faithful; a patch is a line of C added after an instruction
# the game repo names, for a fix the original game cannot have. The --original
# oracle runs the shipping code unpatched, so it remains the reference.

SIDEBAR_ROWS_MAX = 30


def sidebar_rows_patches(code, cs, lo=0x006A5000, hi=0x006AD000, globals_=(0x00886F9C, 0x00B0B4F8)):
    """The sidebar's cameo rows, capped at 30 so 4K fits its button array.

    The sidebar keeps its cameo buttons in a static array at 0x00B07E80: 240
    buttons, four tabs of 60 (0x006A4DC0 builds them; index = tab * 60 + i).
    Its height says how many rows of two to lay out, (height - 26 - top) / 50,
    and at 2160 lines that is about 41 rows: 82 buttons, past the tab's 60 and
    at the last tab past the array (docs/hires.md). Every place that computes
    the row count divides by 50 the same way -- imul by 0x51EB851F, sar 4, then
    `add r, (r >> 31)` to round toward zero -- so each is found by that shape,
    in the sidebar code that reads the sidebar's height (0x00886F9C) or its top
    (0x00B0B4F8), and the result register is capped right after the add. Up to
    1440 lines nothing changes (27 rows or fewer).
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    magic = b'\xb8\x1f\x85\xeb\x51'                      # mov eax, 0x51EB851F
    refs = tuple(g.to_bytes(4, 'little') for g in globals_)   # its height, its top
    out, i = {}, code.find(magic)
    while i != -1:
        va = cs + i
        if lo <= va < hi and any(r in code[max(0, i - 0x30):i] for r in refs):
            shr = None
            for ins in md.disasm(code[i:i + 0x30], va):
                if ins.mnemonic == 'shr' and ins.op_str.endswith(', 0x1f'):
                    shr = ins.op_str.split(',')[0]
                elif shr and ins.mnemonic == 'add' and ins.op_str.split(', ')[1] == shr:
                    reg = ins.op_str.split(',')[0]
                    out[ins.address] = ('if ((int32_t)%s > %d) %s = %d; /* remaster: sidebar rows, '
                                        'run_lift.py sidebar_rows_patches */' % (reg, SIDEBAR_ROWS_MAX, reg,
                                                                                 SIDEBAR_ROWS_MAX))
                    break
        i = code.find(magic, i + 1)
    return out


# HD voxels (docs/voxels.md). The finish stage of a unit's render, 0x00754510,
# turns its depth-sorted section records into pixels in the 256x256 buffer.
# Run three more times with each span's start moved half a pixel, and the four
# images interleave into one at twice the resolution: each pass is a complete,
# hole-free render, so the result is a true 2x sampling of the model. The
# rasterizers walk 8.8 fixed point in a 256-wide buffer, which a 2x projection
# would overflow; half-pixel starts stay inside it. Gated at run time by the
# host (src/runtime/hdvox.c): off, nothing here does anything.
def _hd_passes(arg):
    """C for the three extra runs of 0x00754510, ecx = esp + arg each time."""
    return ('{ extern int ra2_vox_hd_begin(uint32_t); extern void ra2_vox_hd_pass(int); '
            'extern uint32_t ra2_vox_hd_end(void); '
            'if (ra2_vox_hd_begin(eax)) { for (int _k = 1; _k < 4; _k++) { ra2_vox_hd_pass(_k); '
            'ecx = esp + 0x%X; RECOMP_CALL(sub_00754510); } eax = ra2_vox_hd_end(); } } '
            '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */' % arg)


def _hd_passes_7542f0():
    """C for the three extra runs of 0x007542F0, the finish stage voxel
    animations and debris use: ecx = esp+0x44 (its rect), edx = esp+0x20, one
    stack argument esp+0x1C (it returns with ret 4). Its outputs, esp+0x1C to
    esp+0x5C, are put back afterwards."""
    return ('{ extern int ra2_vox_hd_begin_at(uint32_t, uint32_t); extern void ra2_vox_hd_pass(int); '
            'extern uint32_t ra2_vox_hd_end(void); uint32_t _eax = eax; '
            'if (ra2_vox_hd_begin_at(esp + 0x1C, 0x40)) { for (int _k = 1; _k < 4; _k++) { ra2_vox_hd_pass(_k); '
            'ecx = esp + 0x44; edx = esp + 0x20; esp -= 4; MEM32(esp) = esp + 4 + 0x1C; '
            'RECOMP_CALL(sub_007542F0); } ra2_vox_hd_end(); } eax = _eax; } '
            '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */')


HD_VOXEL_PATCHES = {
    # 0x00756590: the span record handed to the rasterizer is at esp+0x20; its
    # starts, x at +0x18 and y at +0x1A, are 8.8 fixed point.
    0x0075683E: ('{ extern int16_t ra2_vox_dx, ra2_vox_dy; '
                 'MEM16(eax + 0x18) += ra2_vox_dx; MEM16(eax + 0x1A) += ra2_vox_dy; } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    # 0x00706ED0, just after the finish stage returned its rect (eax): the
    # extra passes, with the same argument (ecx = esp+0x4C). It preserves
    # ebx/esi/edi/ebp; ecx and edx are dead here.
    0x00706FEF: _hd_passes(0x4C),
    # 0x00706ED0 blits the 1x render onto the battlefield with 0x004AF2A0
    # (ecx the destination surface, edx the palette converter; on the stack
    # the source surface, its rect, the destination point, ...). Just before
    # and just after that call the host looks at the destination, to place
    # the 2x image where the 1x one went.
    0x00707233: ('{ extern void ra2_vox_hd_blit(uint32_t, uint32_t, uint32_t); ra2_vox_hd_blit(ecx, edx, esp); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    0x00707235: ('{ extern void ra2_vox_hd_blitted(void); ra2_vox_hd_blitted(); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    # 0x004373B0, the surface-to-surface blitter, after its `sub esp, 0x4C`:
    # its copy into the frame surface ends a frame, and the host publishes
    # the 2x layer it built during it.
    0x004373B0: ('{ extern void ra2_vox_frame_blit(uint32_t, uint32_t); ra2_vox_frame_blit(ecx, esp + 0x4C + 4); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    # 0x00706640: no voxel cache while HD voxels are on, so every unit is
    # rendered (and at 2x) each frame: a cache key of -1 is the path the
    # rules' DisableVoxelCache already takes.
    0x007067E4: ('{ extern int ra2_vox_hd_on; if (ra2_vox_hd_on) { eax = 0xFFFFFFFFu; MEM32(esp + 0x5C) = eax; } } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    # 0x0073B140: a unit's finished staging image (body, turret, barrel)
    # copied onto the battlefield by 0x004373B0: dest rect, staging surface,
    # source rect on the stack, the destination surface in ecx.
    0x0073B43F: ('{ extern void ra2_vox_unit_copy(uint32_t, uint32_t); ra2_vox_unit_copy(ecx, esp); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    0x0073B446: ('{ extern void ra2_vox_unit_copied(void); ra2_vox_unit_copied(); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    # Shadows. 0x00707280 renders a unit's shadow: sections through
    # 0x00753F90, then the same finish stage (0x00754510, ecx = esp+0x28), its
    # records plotted by 0x00756860, whose start is 8.8 fixed point at
    # [esp+0x5C] (x) and [esp+0x5E] (y) once both are stored.
    0x007568F0: ('{ extern int16_t ra2_vox_dx, ra2_vox_dy; '
                 'MEM16(esp + 0x5C) += ra2_vox_dx; MEM16(esp + 0x5E) += ra2_vox_dy; } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    # 0x00706BD0 draws a shadow from its own cache (a hit through 0x00707480,
    # a miss renders without blitting and goes back to the cache); key -1
    # (ebx, [esp+0x58]) renders and blits every time.
    0x00706C10: ('{ extern int ra2_vox_hd_on; if (ra2_vox_hd_on) { ebx = 0xFFFFFFFFu; MEM32(esp + 0x58) = ebx; } } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    0x00707387: _hd_passes(0x28),
    # Voxel animations and debris (VoxelAnimClass::Draw_It, 0x00749B70): a
    # shadow and then the body, each through 0x007542F0 and blitted onto the
    # battlefield by 0x004AF2A0. Opt-in (RA2_HD_VOXEL_ANIMS=1): no test yet
    # puts one on screen (docs/voxels.md).
    0x00749CFA: _hd_passes_7542f0(),
    0x00749D6D: ('{ extern void ra2_vox_anim_shadow_blit(uint32_t, uint32_t); ra2_vox_anim_shadow_blit(ecx, esp); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    0x00749D6E: ('{ extern void ra2_vox_anim_shadow_blitted(void); ra2_vox_anim_shadow_blitted(); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    0x00749DE6: _hd_passes_7542f0(),
    0x00749EE2: ('{ extern void ra2_vox_anim_blit(uint32_t, uint32_t, uint32_t); ra2_vox_anim_blit(ecx, edx, esp); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    0x00749EE4: ('{ extern void ra2_vox_anim_blitted(void); ra2_vox_anim_blitted(); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    # ...and blits the shadow with 0x004AF2A0 (the shadow converter in edx).
    0x00707431: ('{ extern void ra2_vox_shadow_blit(uint32_t, uint32_t); ra2_vox_shadow_blit(ecx, esp); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
    0x00707432: ('{ extern void ra2_vox_shadow_blitted(void); ra2_vox_shadow_blitted(); } '
                 '/* remaster: HD voxels, run_lift.py HD_VOXEL_PATCHES */'),
}


# The same sites in Red Alert 2 (game.exe), found by their shapes: the same
# instructions and stack offsets, but for the frame blitter's frame (0x48).
HD_VOXEL_SITES_GAME = {
    0x0075683E: 0x00719BEE, 0x00706FEF: 0x006D25E3, 0x00707233: 0x006D27E6, 0x00707235: 0x006D27E8,
    0x004373B0: 0x00434B40, 0x007067E4: 0x006D1E18, 0x0073B43F: 0x006FF66C, 0x0073B446: 0x006FF670,
    0x007568F0: 0x00719CA0, 0x00706C10: 0x006D2220, 0x00707387: 0x006D28F9, 0x00707431: 0x006D29A1,
    0x00707432: 0x006D29A2, 0x00749CFA: 0x0070D14A, 0x00749D6D: 0x0070D1BB, 0x00749D6E: 0x0070D1BC,
    0x00749DE6: 0x0070D234, 0x00749EE2: 0x0070D32E, 0x00749EE4: 0x0070D330,
}
HD_VOXEL_PATCHES_GAME = {
    HD_VOXEL_SITES_GAME[va]: c.replace('sub_00754510', 'sub_007178E0').replace('sub_007542F0', 'sub_007176C0')
                              .replace('esp + 0x4C + 4', 'esp + 0x48 + 4')
    for va, c in HD_VOXEL_PATCHES.items()
}


def apply_patches(body, patches):
    """Add each patch's C after the line that lifts its instruction."""
    for va, c in patches.items():
        tag = '/* 0x%08X:' % va
        k = body.find(tag)
        if k != -1:
            e = body.index('\n', k)
            body = body[:e + 1] + '    ' + c + '\n' + body[e + 1:]
    return body


# Red Alert 2 itself (game.exe, the same install) is a second target: its own
# catalog in work/game, its lift in src/recomp/gen_game, its own seeds, hooks
# and patches. Everything address-bound the host needs comes to it through
# recomp_target.h, written beside the lifted C.
RUN_SEEDS_GAME = [
    0x00524FF0,   # a static constructor (the _initterm table at 0x007C9000)
    0x007492D0,   # a callback named only by an immediate (0x007458D8: mov edx, 0x7492d0)
]
HOOKS_GAME = {
    0x004068F0: 'debuglog',            # the bare-ret debug printf, 1,592 callers
}


def targets():
    return {
        'gamemd': dict(exe=EXE, catalog=CATALOG, seeds=SEEDS, out=OUT, stats=STATS,
                       run_seeds=RUN_SEEDS, hooks=HOOKS, exe_name='gamemd.exe', ini_name='RA2MD.INI',
                       title="Yuri's Revenge", game_frame=0x00A8ED84, stamp=0x3BDF544E,
                       patches=lambda code, cs: {**sidebar_rows_patches(code, cs), **HD_VOXEL_PATCHES}),
        'game': dict(exe=os.path.join(_HERE, 'game', 'game.exe'),
                     catalog=os.path.join(_HERE, 'work', 'game', 'functions.json'),
                     seeds=os.path.join(_HERE, 'work', 'game', 'rtti_seeds.json'),
                     out=os.path.join(_HERE, 'src', 'recomp', 'gen_game'),
                     stats=os.path.join(_HERE, 'work', 'game', 'lift_stats.json'),
                     run_seeds=RUN_SEEDS_GAME, hooks=HOOKS_GAME, exe_name='game.exe', ini_name='RA2.INI',
                     title='Red Alert 2', game_frame=0x00A40D2C, stamp=0x3B1EBBED,
                     # Red Alert 2's sidebar: its code at 0x0067B000..0x00683000, its
                     # height 0x0083962C and top 0x00ABCD64, the globals the same divide
                     # by 50 reads as Yuri's Revenge's does.
                     patches=lambda code, cs: {**sidebar_rows_patches(code, cs, 0x0067B000, 0x00683000,
                                                                     (0x0083962C, 0x00ABCD64)),
                                               **HD_VOXEL_PATCHES_GAME}),
    }


def write_target_header(out, name, t):
    """recomp_target.h: what the host needs to know about the binary it runs."""
    debuglog = [a for a, h in t['hooks'].items() if h == 'debuglog']
    with open(os.path.join(out, 'recomp_target.h'), 'w', newline='\n') as f:
        f.write('/* Generated by run_lift.py --target %s: the binary this build runs. */\n#pragma once\n' % name)
        f.write('#define RA2_TARGET_%s 1\n' % name.upper())
        f.write('#define RA2_EXE_NAME "%s"\n' % t['exe_name'])
        f.write('#define RA2_INI_NAME "%s"\n' % t['ini_name'])
        f.write('#define RA2_TITLE "%s"\n' % t['title'])
        f.write('#define RA2_HOOK_DEBUGLOG_VA 0x%08Xu\n' % (debuglog[0] if debuglog else 0))
        f.write('#define RA2_GAME_FRAME_VA 0x%08Xu   /* the game\'s frame count */\n' % t['game_frame'])
        f.write('#define RA2_EXE_STAMP 0x%08Xu   /* its PE timestamp: the build lifted */\n' % t['stamp'])


def exe_stamp(path):
    """The PE header's TimeDateStamp: which build of the game an exe is."""
    with open(path, 'rb') as f:
        head = f.read(4096)
    nt = int.from_bytes(head[0x3C:0x40], 'little')
    return int.from_bytes(head[nt + 8:nt + 12], 'little')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--target', choices=('gamemd', 'game'), default='gamemd',
                    help="gamemd: Yuri's Revenge (the default); game: Red Alert 2")
    ap.add_argument('--exe')
    ap.add_argument('--catalog')
    ap.add_argument('--out')
    ap.add_argument('--roots', default='', help='comma-separated extra root VAs')
    ap.add_argument('--max', type=int, default=3000, help='closure size cap')
    ap.add_argument('--all', action='store_true', help='lift every function')
    ap.add_argument('--virtual', action='store_true',
                    help='root the closure at every RTTI vtable method too')
    ap.add_argument('--split', type=int, default=400, help='functions per .c file')
    # More entries a run found, on top of RUN_SEEDS (seed_from_log.py JSON).
    ap.add_argument('--seeds', default=None,
                    help='seed_from_log.py JSON of unresolved targets from runs')
    args = ap.parse_args()
    tgt = targets()[args.target]
    args.exe = args.exe or tgt['exe']
    args.catalog = args.catalog or tgt['catalog']
    args.out = args.out or tgt['out']
    if args.seeds is None:
        args.seeds = os.path.join(os.path.dirname(tgt['catalog']), 'run_seeds.json')
    if not os.path.exists(args.catalog):
        sys.exit('no catalog at %s -- run disasm32.py first (README, Step by step)' % args.catalog)

    # The patches below are at this build's addresses; another build lifts,
    # but they land in the wrong code, and it crashes in game (issue #2).
    stamp = exe_stamp(args.exe)
    if stamp != tgt['stamp']:
        sys.exit('%s is not the build this project supports: its PE timestamp is 0x%08X, '
                 'the Steam release\'s is 0x%08X (docs/RECON.md). A different release '
                 '(EA App, Origin, a CD, a CnCNet- or mod-patched exe) needs its own '
                 'addresses; use the Steam build\'s %s.' % (args.exe, stamp, tgt['stamp'], tgt['exe_name']))
    info = analyze_pe(args.exe)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] base=0x%08X code=0x%08X-0x%08X IAT=%d' % (info.image_base, cs, ce, len(iat)))

    cat = json.load(open(args.catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    print('[*] catalog: %d functions inside .text' % len(byaddr))

    # Vtable slots. Bound each by the next known entry: a slot landing mid-code
    # handed `ce` makes the extent walk descend the whole of .text.
    want = {a for a in tgt['run_seeds'] if cs <= a < ce and a not in byaddr}
    for path in [tgt['seeds']] + ([args.seeds] if args.seeds else []):
        if os.path.exists(path):
            want |= {e['address'] for e in json.load(open(path))
                     if cs <= e['address'] < ce and e['address'] not in byaddr}
    known = sorted(set(byaddr) | want)
    nxt = {a: (known[i + 1] if i + 1 < len(known) else ce) for i, a in enumerate(known)}
    for a in want:
        byaddr[a] = {'address': a, 'end': min(nxt[a], a + 0x100), 'calls_to': [],
                     'entry_kind': 'start'}
    print('[*] vtable slots not in the catalog: %d injected' % len(want))

    entry = info.image_base + info.entry_point_rva
    roots = [entry] + [int(x, 0) for x in args.roots.split(',') if x.strip()]
    if args.virtual and os.path.exists(tgt['seeds']):
        roots += sorted(e['address'] for e in json.load(open(tgt['seeds'])))
    if args.all:
        chosen = set(byaddr)
    else:
        chosen = set(closure(byaddr, roots, args.max))
    print('[*] lifting %d of %d functions (%s)'
          % (len(chosen), len(byaddr), 'all' if args.all else 'closure, cap %d' % args.max))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(args.exe, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]

    # Known targets become RECOMP_CALL; anything else a decoded `call` names
    # becomes RECOMP_ICALL, which reports at run time instead of failing the build.
    # precise carry: adc/sbb take their carry from the flag state that set it.
    # The default reads a `_cf` that add/sub/cmp never write, which breaks
    # every 64-bit add and subtract. The Movies lost its menu video to it
    # (its docs/bringup.md); off by default in lift32 only because Fury3 leans
    # on the imprecision.
    patches = tgt['patches'](code, cs)
    print('[*] remaster patches: %d' % len(patches))
    lifter = Lifter(iat_map=iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)
    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):                 # a smaller lift must not leave stale chunks
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(args.out, fn))
    entries, chunk, idx, errors, dirty = [], [], 0, 0, 0
    ordered = sorted(byaddr)
    starts = {a for a in ordered if byaddr[a].get('entry_kind') != 'alias'}
    t_split = time.time()
    splits = find_splits(md, code, cs, ce, starts)
    starts -= splits          # still dispatchable; just not a wall inside its parent
    print('[*] split entries (the middle of the function before them): %d, %.0fs'
          % (len(splits), time.time() - t_split))
    t0 = time.time()

    def flush(force=False):
        nonlocal chunk, idx
        if chunk and (force or len(chunk) >= args.split):
            write_chunk(args.out, idx, chunk)
            idx += 1
            chunk = []

    def lift_one(addr, name, end, reached):
        nonlocal errors

        if addr in tgt['hooks']:
            hook = tgt['hooks'][addr]
            chunk.append(('extern void ra2_hook_%s(void);\nvoid %s(void) { ra2_hook_%s(); }\n'
                          % (hook, name, hook), addr, name))
            entries.append((addr, name))
            return
        try:
            lo = min(reached) if reached else addr   # a chunk can sit below the entry
            insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                              if end > addr else ([], None))
            if leaders is not None:
                leaders.add(addr)
            body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                    else 'void %s(void) { }\n' % name)
            body = apply_patches(body, {va: c for va, c in patches.items() if reached and va in reached})
        except Exception as e:                      # noqa: BLE001 -- counted, not hidden
            body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
            errors += 1
        chunk.append((body, addr, name))
        entries.append((addr, name))
        if len(chunk) >= args.split:
            flush()
            print('[*]   %d/%d (%d err)' % (len(entries), len(chosen), errors), flush=True)

    # A direct branch that leaves a body backward to an address nothing
    # catalogued is a tail call the catalog missed (__mtterm: jmp 0x00AD6FB7)
    # or a jump into shared code in a neighbour (hand-written x87 math:
    # __ffexpm1 jne 0x00AE01F0). A direct call can name one too: the catalog
    # dropped 0x00C10170, called directly, because a false start inside the
    # jump table before it decoded over it. Either way the target has to be dispatchable,
    # or the RECOMP_ITAIL cannot resolve. So each round's outside targets
    # become entries and are lifted in the next round, until none are new.
    todo, added = sorted(chosen), 0
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind, called = set(), set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind, called=called)
            outside |= {t for t in behind if t not in reached} | called
            dirty += not clean
            lift_one(addr, name, end, reached)
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t, 'end': ce, 'calls_to': [], 'entry_kind': 'start'}
            chosen.add(t)
        added += len(todo)
    print('[*] branch and call targets outside the catalog, added as entries: %d' % added)

    stubs = [a for a in ordered if a not in chosen]
    for a in stubs:
        name = 'sub_%08X' % a
        chunk.append(('void %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n' % (name, a), a, name))
        entries.append((a, name))
        flush()
    flush(force=True)

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* gamemd.exe - AUTO-GENERATED by run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\n'
                'void recomp_not_lifted(uint32_t va);\n'
                '#define RECOMP_NOT_LIFTED(va) recomp_not_lifted(va)\n\n')
        for a, n in entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* gamemd.exe - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t ra2_entry_va = 0x%08Xu;\n' % (len(entries), entry))

    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out))
    stats = {'lifted': len(chosen), 'stubs': len(stubs), 'errors': errors,
             'no_terminator': dirty, 'files': idx, 'lines': lines}
    json.dump(stats, open(tgt['stats'], 'w'), indent=1)
    write_target_header(args.out, args.target, tgt)
    print('=' * 60)
    print('  lifted %d   not-lifted stubs %d   errors %d   no terminator %d'
          % (len(chosen), len(stubs), errors, dirty))
    print('  %s lines of C in %d files, %.1fs' % (format(lines, ','), idx, time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
