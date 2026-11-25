#!/usr/bin/env python3
"""
ableton_slice.py - cut a long Live export back into single sample files.

Why: samples from Ableton packs are often protected AIFF-C ("able"); only Live
can open them. Live CAN export them. Dropping a folder of samples on ONE audio
track and exporting that track gives one long WAV with all the samples in it.
This tool cuts that WAV back into one file per sample, named like the original.

It never reads or decodes the protected files: only their headers (frame count
and sample rate, which are not protected). The cut points are computed:
samples sit in Live's (natural) name order, each one starting on the next bar line (Live's grid)
after the previous one ends. The export length is then checked against the
prediction to the frame; if it does not match, nothing is written.

Python 3.6+, standard library only.

  python3 ableton_slice.py FOLDER [--export LONG.wav] [--bpm 120]
                           [--out DIR] [--dry-run] [--force]
                           [--locked-only] [--stash-export]
"""
import argparse
import os
import re
import struct
import sys


# ---------------------------------------------------------------- AIFF header
def _ext80(b):
    e = struct.unpack('>H', b[:2])[0]
    m = struct.unpack('>Q', b[2:10])[0]
    return m * 2.0 ** ((e & 0x7FFF) - 16383 - 63)


def read_aiff_comm(path):
    """-> (channels, frames, bits, rate, compression) from the COMM chunk."""
    with open(path, 'rb') as f:
        d = f.read(65536)
    if d[:4] != b'FORM' or d[8:12] not in (b'AIFF', b'AIFC'):
        raise ValueError('not an AIFF file')
    p = 12
    while p + 8 <= len(d):
        cid = d[p:p + 4]
        sz = struct.unpack('>I', d[p + 4:p + 8])[0]
        if cid == b'COMM':
            ch, fr, bits = struct.unpack('>hIh', d[p + 8:p + 16])
            rate = _ext80(d[p + 16:p + 26])
            comp = d[p + 26:p + 30] if d[8:12] == b'AIFC' and sz >= 22 else b'NONE'
            return ch, fr, bits, rate, comp
        p += 8 + sz + (sz & 1)
    raise ValueError('no COMM chunk')


# ----------------------------------------------------------------- WAV header
class Wav(object):
    pass


def read_wav_info(path):
    w = Wav()
    w.path = path
    with open(path, 'rb') as f:
        h = f.read(12)
        if h[:4] != b'RIFF' or h[8:12] != b'WAVE':
            raise ValueError('not a WAV file')
        w.fmt = None
        w.data_off = None
        while True:
            ch = f.read(8)
            if len(ch) < 8:
                break
            cid, sz = ch[:4], struct.unpack('<I', ch[4:])[0]
            if cid == b'fmt ':
                w.fmt = f.read(sz + (sz & 1))[:sz]
                continue
            if cid == b'data':
                w.data_off = f.tell()
                size_file = os.path.getsize(path) - w.data_off
                w.data_len = min(sz, size_file) if sz != 0xFFFFFFFF else size_file
                break
            f.seek(sz + (sz & 1), 1)
    if w.fmt is None or w.data_off is None:
        raise ValueError('WAV has no fmt or data chunk')
    tag, w.channels, w.rate, _, w.block, w.bits = struct.unpack('<HHIIHH', w.fmt[:16])
    if tag not in (1, 3, 0xFFFE):
        raise ValueError('WAV format %#x is not PCM or float' % tag)
    w.frames = w.data_len // w.block
    return w


def write_wav_slice(src, dst, first, count):
    """Copy `count` frames from frame `first` of src into a new WAV (same format)."""
    nbytes = count * src.block
    fmt = src.fmt
    hdr = b'RIFF' + struct.pack('<I', 4 + 8 + len(fmt) + (len(fmt) & 1) + 8 + nbytes
                                + (nbytes & 1)) + b'WAVE'
    hdr += b'fmt ' + struct.pack('<I', len(fmt)) + fmt + (b'\0' if len(fmt) & 1 else b'')
    hdr += b'data' + struct.pack('<I', nbytes)
    allzero = True
    tmp = dst + '.part'
    with open(src.path, 'rb') as fi, open(tmp, 'wb') as fo:
        fo.write(hdr)
        fi.seek(src.data_off + first * src.block)
        left = nbytes
        while left > 0:
            blk = fi.read(min(left, 1 << 20))
            if not blk:
                raise IOError('export is shorter than expected')
            if allzero and blk.strip(b'\0'):
                allzero = False
            fo.write(blk)
            left -= len(blk)
        if nbytes & 1:
            fo.write(b'\0')
    os.replace(tmp, dst)
    return not allzero


# ------------------------------------------------------------------- planning
def natural_key(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r'(\d+)', s)]


ORDERS = [
    ('name order (A-Z, case-sensitive)', lambda names: sorted(names)),
    ('name order (case-insensitive)', lambda names: sorted(names, key=str.lower)),
    ('natural order (2 before 10)', lambda names: sorted(names, key=natural_key)),
]


def plan(frames_by_name, order_names, bpm, out_rate, in_rates):
    """Predicted (start, length) per sample on a bar grid of `bpm` (4/4).
    Each sample starts on the first bar line at or after the end of the one
    before; the first starts at 0."""
    bar = out_rate * 240.0 / bpm
    out = []
    end = 0.0
    for i, n in enumerate(order_names):
        ln = int(round(frames_by_name[n] * out_rate / in_rates[n]))
        start = 0 if i == 0 else int(round(-(-(end - 0.5) // bar) * bar))
        out.append((n, start, ln))
        end = start + ln
    return out, int(end)


def gap_failures(wav, p):
    """How many gaps between planned samples are NOT digital silence.
    A sample is followed by a silent stretch until the next bar line; the
    resampler of the export rings for a few hundred frames at the edges, so
    1500 frames at each side are left out. A wrong grid cuts through audio."""
    bad = checked = 0
    margin = 1500
    with open(wav.path, 'rb') as f:
        for (n, s, l), (n2, s2, l2) in zip(p, p[1:]):
            a, b = s + l + margin, s2 - margin
            if b - a < 200:
                continue
            checked += 1
            f.seek(wav.data_off + a * wav.block)
            left = (b - a) * wav.block
            while left > 0:
                blk = f.read(min(left, 1 << 20))
                if not blk:
                    break
                if blk.strip(b'\0'):
                    bad += 1
                    break
                left -= len(blk)
    return bad, checked


def find_plan(files, comms, wav, bpm_arg):
    """All (order, tempo) combinations that fit the export. The LENGTH alone
    cannot tell the order (every sample takes a whole number of bars wherever
    it stands) and can fit a wrong tempo by chance; the AUDIO can: between two
    samples there must be digital silence. Returns the best-scoring plans,
    one per distinct set of cut points."""
    frames = {n: comms[n][1] for n in files}
    rates = {n: comms[n][3] for n in files}
    bpms = [bpm_arg] if bpm_arg else [120.0] + [b for b in range(40, 301) if b != 120]
    hits = []
    seen = set()
    for oname, ofn in ORDERS:
        names = ofn(files)
        for b in bpms:
            p, total = plan(frames, names, b, wav.rate, rates)
            if abs(total - wav.frames) > 2:
                continue
            key = tuple((n, s_, l_) for n, s_, l_ in p)
            if key in seen:
                continue
            seen.add(key)
            bad, checked = gap_failures(wav, p)
            hits.append((oname, b, p, total, bad, checked))
    if not hits:
        return []
    best = min(h[4] for h in hits)
    return [h for h in hits if h[4] == best]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folder', help='folder with the original .aif files (the ones dropped in Live)')
    ap.add_argument('--export', help='the long WAV exported from Live (found by itself if the '
                                     'folder has one WAV that is not a copy of a sample)')
    ap.add_argument('--bpm', type=float, help="the Live set's tempo (default: 120, then tried 40-300)")
    ap.add_argument('--out', help='where to write the single files (default: the folder itself)')
    ap.add_argument('--dry-run', action='store_true', help='check and show the plan, write nothing')
    ap.add_argument('--force', action='store_true', help='overwrite existing .wav files')
    ap.add_argument('--locked-only', action='store_true',
                    help="write only the protected ('able') samples; the others are still "
                         'counted, as they are in the export')
    ap.add_argument('--stash-export', action='store_true',
                    help="afterwards move the long WAV to a hidden '.export_source' folder "
                         "(the library skips it); nothing is deleted")
    a = ap.parse_args()

    folder = os.path.abspath(a.folder)
    files = sorted(f for f in os.listdir(folder)
                   if f.lower().endswith(('.aif', '.aiff', '.aifc')) and not f.startswith('.'))
    if not files:
        sys.exit('No .aif files in %s' % folder)
    comms = {}
    for f in files:
        try:
            comms[f] = read_aiff_comm(os.path.join(folder, f))
        except Exception as e:
            sys.exit('%s: %s' % (f, e))

    exp = a.export
    if not exp:
        stems = set(os.path.splitext(f)[0] for f in files)
        cand = [f for f in os.listdir(folder) if f.lower().endswith('.wav')
                and os.path.splitext(f)[0] not in stems and not f.startswith('.')]
        if len(cand) != 1:
            sys.exit('Say which file is the Live export: --export FILE.wav '
                     '(found %d candidates)' % len(cand))
        exp = os.path.join(folder, cand[0])
    wav = read_wav_info(exp)
    print('Export: %s  (%d ch, %d Hz, %d bit, %.2f s)'
          % (os.path.basename(exp), wav.channels, wav.rate, wav.bits, wav.frames / float(wav.rate)))
    print('Samples: %d .aif files, %d protected ("able")'
          % (len(files), sum(1 for f in files if comms[f][4] == b'able')))

    hits = find_plan(files, comms, wav, a.bpm)
    if not hits:
        exp_s = wav.frames / float(wav.rate)
        tot = sum(comms[f][1] / comms[f][3] for f in files)
        print('\nNO MATCH: the export length does not fit these files on a bar grid.')
        print('  export %.3f s; the samples alone %.3f s (without gaps).' % (exp_s, tot))
        print('  Likely causes: the folder has more or fewer files than were in Live; Warp was '
              'on (clips stretched);\n  the clips are not in one row; the set is not 4/4; or '
              'the render start/end was not at the clips.')
        sys.exit(2)
    if len(hits) > 1:
        print('\nMore than one reading fits equally well (%s). Say the tempo with --bpm.'
              % ', '.join('%s @ %g BPM' % (h[0], h[1]) for h in hits[:6]))
        sys.exit(2)
    oname, bpm, p, total, bad, checked = hits[0]
    print('Match: %s, %g BPM grid (4/4), predicted length %d frames, export %d (off by %d).'
          % (oname, bpm, total, wav.frames, wav.frames - total))

    if bad:
        print('WARNING: %d of %d gaps between samples are not silent: the cut points may be wrong. '
              'Check a few results by ear.' % (bad, checked))
    else:
        print('Checked: all %d gaps between samples are digital silence.' % checked)

    outdir = os.path.abspath(a.out) if a.out else folder
    todo = [(n, s, l) for (n, s, l) in p
            if not a.locked_only or comms[n][4] == b'able']
    if not a.dry_run:
        os.makedirs(outdir, exist_ok=True)
    wrote = skipped = silent = 0
    for n, s, l in todo:
        dst = os.path.join(outdir, os.path.splitext(n)[0] + '.wav')
        if os.path.exists(dst) and not a.force:
            skipped += 1
            continue
        if a.dry_run:
            wrote += 1
            continue
        if s + l > wav.frames:  # rounding at the very end
            l = wav.frames - s
        if not write_wav_slice(wav, dst, s, l):
            silent += 1
            print('  warning: %s is silent in the export' % n)
        wrote += 1
    print('%s: %d files%s%s' % ('Would write' if a.dry_run else 'Written', wrote,
                                (', %d already there (use --force to replace)' % skipped) if skipped else '',
                                (', %d SILENT (mute / empty clip in Live?)' % silent) if silent else ''))
    if not a.dry_run and a.stash_export and os.path.dirname(os.path.abspath(exp)) == folder:
        sd = os.path.join(folder, '.export_source')
        os.makedirs(sd, exist_ok=True)
        os.replace(exp, os.path.join(sd, os.path.basename(exp)))
        print('Export moved to %s' % sd)
    elif not a.dry_run and os.path.dirname(os.path.abspath(exp)) == outdir:
        print('Note: the long export is still in the library folder; move it away '
              '(or use --stash-export), or it will be listed as one huge sample.')
    if not a.dry_run:
        print('Next: in TR-Brick, Library > Rescan Samples.')


if __name__ == '__main__':
    main()
