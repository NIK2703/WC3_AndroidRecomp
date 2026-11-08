"""Offline waveform audit for the WINMM ring dumps.

The dump produced by h_wave_out_write() is the byte stream that AAudio will
actually play (ring append order), so a click that is audible at the speaker
must be visible here as a discontinuity.  Transport counters (short/xruns/
WOM_DONE age) cannot see it.

Metrics per 50 ms block (8832 bytes = 2208 stereo s16 frames):
  * zero / near-silent blocks
  * exact duplicate blocks and their spacing
  * seam prediction error vs the interior prediction error distribution
    (a step at a buffer boundary is what the ear hears as a click)
"""
import sys
import array
import struct
import collections

BLK = 8832          # 2208 frames * 2 ch * 2 bytes


def load_blocks(path):
    raw = open(path, 'rb').read()
    n = len(raw)
    if n % BLK == 0:
        stride, off, tag = BLK, 0, 'raw'
    elif (n - 8) % BLK == 0:
        stride, off, tag = BLK, 8, '8-byte leading header'
    elif n % (BLK + 8) == 0:
        stride, off, tag = BLK + 8, 8, 'per-block header'
    else:
        # tolerate a truncated tail
        stride, off, tag = BLK, 0, 'raw/truncated'
    blocks = []
    p = off
    while p + stride <= n:
        chunk = raw[p:p + BLK]
        if len(chunk) < BLK:
            break
        a = array.array('h')
        a.frombytes(chunk)
        if sys.byteorder != 'little':
            a.byteswap()
        blocks.append((chunk, a))
        p += stride
    return blocks, tag, n


def stats(a):
    s = 0
    sq = 0
    peak = 0
    zeros = 0
    for v in a:
        if v == 0:
            zeros += 1
        s += v
        vv = v * v
        sq += vv
        if v > peak:
            peak = v
        elif -v > peak:
            peak = -v
    n = len(a)
    rms = (sq / n) ** 0.5 if n else 0.0
    return rms, peak, zeros / float(n)


def pred_errors(a):
    """|s[i] - (2 s[i-1] - s[i-2])| for i>=2, both channels interleaved."""
    out = []
    for i in range(2, len(a)):
        e = a[i] - (2 * a[i - 1] - a[i - 2])
        out.append(e if e >= 0 else -e)
    return out


def percentile(v, p):
    if not v:
        return 0
    s = sorted(v)
    k = int((len(s) - 1) * p)
    return s[k]


def audit(path):
    blocks, tag, nbytes = load_blocks(path)
    nb = len(blocks)
    rms = []
    hashes = []
    zero_blocks = []
    for i, (chunk, a) in enumerate(blocks):
        r, peak, zf = stats(a)
        rms.append(r)
        hashes.append(hash(chunk))
        if r < 1.0:
            zero_blocks.append(i)

    # interior prediction-error baseline (sample every 8th to keep it fast)
    interior = []
    for chunk, a in blocks[::3]:
        e = pred_errors(a)
        interior.extend(e[::8])
    p50 = percentile(interior, 0.50)
    p99 = percentile(interior, 0.99)
    p999 = percentile(interior, 0.999)

    seam_bad = []
    seam_raw = []
    seam_err = []
    for i in range(1, nb):
        prev = blocks[i - 1][1]
        cur = blocks[i][1]
        # two channels: last two frames and the first frame
        for c in (0, 1):
            x2, x1 = prev[-4 + c], prev[-2 + c]
            x0 = cur[c]
            e = x0 - (2 * x1 - x2)
            se = e if e >= 0 else -e
            step = x0 - x1
            step = step if step >= 0 else -step
            seam_err.append(se)
            seam_raw.append(step)
            # a seam is anomalous when it is both absolutely large and far
            # above this signal's own interior p99
            if se > 3000 and se > 8 * max(p99, 1):
                seam_bad.append((i, c, se, step))

    # duplicates
    seen = collections.defaultdict(list)
    for i, h in enumerate(hashes):
        seen[h].append(i)
    dups = [(h, idx) for h, idx in seen.items() if len(idx) > 1]
    dups.sort(key=lambda x: -len(x[1]))
    dup_blocks = sum(len(idx) - 1 for _, idx in dups)
    spacings = collections.Counter()
    for _, idx in dups:
        for j in range(1, len(idx)):
            spacings[idx[j] - idx[j - 1]] += 1

    # longest silence run
    longest = run = 0
    start = best = -1
    for i, r in enumerate(rms):
        if r < 1.0:
            if run == 0:
                start = i
            run += 1
            if run > longest:
                longest, best = run, start
        else:
            run = 0

    print('=== %s ===' % path)
    print('bytes=%d layout=%s blocks=%d (%.2f s @50ms)' %
          (nbytes, tag, nb, nb * 0.05))
    print('silent blocks: %d (%.1f%%)  longest run=%d blocks (%.2f s) @%d' %
          (len(zero_blocks), 100.0 * len(zero_blocks) / max(nb, 1),
           longest, longest * 0.05, best))
    print('duplicate groups=%d duplicated blocks=%d (%.1f%%)  top spacings=%s'
          % (len(dups), dup_blocks, 100.0 * dup_blocks / max(nb, 1),
             spacings.most_common(8)))
    print('interior prediction error: p50=%d p99=%d p99.9=%d'
          % (p50, p99, p999))
    if seam_err:
        print('seam prediction error: median=%d p90=%d p99=%d max=%d'
              % (percentile(seam_err, 0.5), percentile(seam_err, 0.9),
                 percentile(seam_err, 0.99), max(seam_err)))
    print('seam anomalies (|err|>3000 and >8x interior p99): %d / %d seams'
          % (len(seam_bad), (nb - 1) * 2))
    if seam_bad:
        print('  first 10: %s' % seam_bad[:10])
    print('rms: min=%.0f median=%.0f p99=%.0f max=%.0f'
          % (min(rms) if rms else 0, percentile([int(x) for x in rms], 0.5),
             percentile([int(x) for x in rms], 0.99),
             max(rms) if rms else 0))
    print()
    return dict(kind='pcm', blocks=nb, nbytes=nbytes,
                silent=len(zero_blocks), dups=dup_blocks,
                seam_bad=len(seam_bad))


ZERO_HASH = 'E2BA14A5'
WRITE_RE = None


def audit_log(path):
    """Signal-level baseline from the [wave] Write trace.

    The h= field is a fingerprint of the ring snapshot, i.e. of the bytes
    AAudio is about to play.  An exact repeat of a non-silent fingerprint
    N writes later is a repeated 50 ms fragment -- the audible stutter.  A
    repeat every 5 writes is one full turn of Miles' five-WAVEHDR pool.
    This is a content metric, not a transport metric.
    """
    import re
    global WRITE_RE
    if WRITE_RE is None:
        WRITE_RE = re.compile(
            r'\[wave\] Write t=(\d+).*?hdr=([0-9A-Fa-f]+).*?'
            r'gid=(\d+).*?flg=([0-9A-Fa-f]+)\s+h=([0-9A-Fa-f]+)')
    rows = []
    with open(path, 'rb') as f:
        for raw in f:
            line = raw.decode('utf-8', 'replace')
            m = WRITE_RE.search(line)
            if m:
                rows.append((int(m.group(1)), m.group(2).upper(),
                             int(m.group(3)), m.group(4).upper(),
                             m.group(5).upper()))
    if not rows:
        print('=== %s ===\nno [wave] Write rows\n' % path)
        return dict(kind='log', writes=0, zero=0, dups=0, cycle5=0)
    t0 = rows[0][0]
    last = {}
    dups = []
    zero = 0
    flags = collections.Counter()
    gids = collections.Counter()
    buckets = collections.defaultdict(lambda: [0, set()])
    for i, (t, hdr, gid, flg, h) in enumerate(rows):
        flags[flg] += 1
        gids[gid] += 1
        b = int((t - t0) / 5000)
        buckets[b][0] += 1
        buckets[b][1].add(h)
        if h == ZERO_HASH:
            zero += 1
        if h in last and h != ZERO_HASH:
            dups.append((i - last[h], i, h))
        if h != ZERO_HASH:
            last[h] = i
    spacings = collections.Counter(d for d, _, _ in dups)
    print('=== %s (log) ===' % path)
    print('writes=%d span=%.1f s  zero-hash=%d (%.1f%%)' %
          (len(rows), (rows[-1][0] - t0) / 1000.0, zero,
           100.0 * zero / len(rows)))
    print('exact non-silent repeats=%d (%.1f%%)  top spacings=%s' %
          (len(dups), 100.0 * len(dups) / len(rows),
           spacings.most_common(8)))
    cycle5 = spacings.get(5, 0)
    print('5-write (250 ms) pool-cycle repeats=%d (%.1f%% of all repeats)' %
          (cycle5, 100.0 * cycle5 / max(len(dups), 1)))
    print('pre-submit flags=%s' % dict(flags.most_common(5)))
    print('writer gids=%s' % dict(gids.most_common(5)))
    print('5 s buckets (writes / unique hashes): %s' %
          ', '.join('%d/%d' % (v[0], len(v[1]))
                    for k, v in sorted(buckets.items())[:12]))
    print()
    return dict(kind='log', writes=len(rows), zero=zero,
                dups=len(dups), cycle5=cycle5)


def decode_device_log(path):
    """Device logs are sometimes UTF-16LE with a swapped byte-order marker.

    A plain utf-8 decode silently produces CJK mojibake (湡牤楯) and the
    interesting lines disappear.  Pick the encoding that actually contains a
    known audio tag instead of trusting the extension.
    """
    raw = open(path, 'rb').read()
    best = ''
    for enc in ('utf-8', 'utf-16-le', 'utf-16-be'):
        try:
            candidate = raw.decode(enc)
        except UnicodeDecodeError:
            continue
        score = candidate.count('[wave2]') + candidate.count('[timer]') + \
                candidate.count('[wave]')
        if score and score > best.count('[wave2]') + best.count('[timer]') + \
                best.count('[wave]'):
            best = candidate
    return best


def sync_check(log_path, dump_bytes, pcm_bytes_per_second=44100 * 4):
    """Reject a "win" that is actually producer starvation.

    A dump only contains the bytes we handed to AAudio.  If the guest stalls,
    the speaker hears silence that never appears in the dump, so the duplicate
    and silence percentages look better while the audio is worse.  Compare the
    dump duration with the wall-clock span of mixer ticks in the device log.
    Returns (span_s, dump_s, ratio) or None when the log has no timestamps.
    """
    import re
    text = decode_device_log(log_path)
    if not text:
        return None
    ticks = [int(v) for v in re.findall(r'\]\s*tick\s+t=(\d+)', text)]
    if len(ticks) < 2:
        ticks = [int(v) for v in re.findall(r'\[timer\][^\n]*?t=(\d+)', text)]
    if len(ticks) < 2:
        return None
    span = (ticks[-1] - ticks[0]) / 1000.0
    if span <= 0:
        return None
    dump_s = dump_bytes / float(pcm_bytes_per_second)
    return span, dump_s, dump_s / span


MILES_RE = None


def miles_state_check(log_path):
    """A-12.4 S0: verify Miles' two-thread SPSC contract directly.

    The lifted Mss32.dll keeps a ring at [dev+0x7C] with N=[dev+0x78],
    w=[dev+0x80] (producer = waveOutProc / WOM_DONE) and r=[dev+0x84]
    (consumer = WM_TIMER service).  waveout_hle.c logs the three indices and
    the guest thread id at every write / WOM_DONE / timer tick.

    w and r wrap modulo N, so a raw `r > w` is NOT a violation.  Reconstruct
    cumulative producer/consumer counters from modulo-N deltas and require
    0 <= W - R <= N at every observation.  Also reject a TimerProc that jumps
    to another guest thread and any waveOutWrite issued from inside
    waveOutProc (winmm forbids it: deadlock).
    """
    import re
    global MILES_RE
    if MILES_RE is None:
        MILES_RE = re.compile(
            r'\[wave2\]\s+miles\s+t=(\d+)\s+ev=(\w+)\s+'
            r'w=(\d+)\s+r=(\d+)\s+N=(\d+)\s+gid=(\d+)')
    text = decode_device_log(log_path)
    rows = MILES_RE.findall(text) if text else []
    violations = []
    events = collections.Counter()
    timer_gids = set()
    W = R = 0
    max_backlog = 0
    N_prev = None
    w_prev = r_prev = None
    for t, ev, ws, rs, Ns, gids in rows:
        ev = ev.lower()
        events[ev] += 1
        w, r, N, gid = int(ws), int(rs), int(Ns), int(gids)
        if ev == 'violation':
            violations.append('t=%s explicit waveOutWrite-from-waveOutProc' % t)
        if ev == 'timer':
            timer_gids.add(gid)
        if N <= 0:
            violations.append('t=%s N=%d (ring size not readable)' % (t, N))
            continue
        if N != N_prev:
            # Ring resized / device re-opened: restart the counters.
            W = R = w_prev = r_prev = 0
            N_prev = N
        if w_prev is not None:
            W += (w - w_prev) % N
            R += (r - r_prev) % N
            backlog = W - R
            max_backlog = max(max_backlog, backlog)
            if backlog < 0 or backlog > N:
                violations.append(
                    't=%s ev=%s w-r invariant broken: W-R=%d N=%d'
                    % (t, ev, backlog, N))
        w_prev, r_prev = w, r
    if not rows:
        violations.append('no [wave2] miles state rows in log')
    if len(timer_gids) > 1:
        violations.append('TimerProc gid changed: %s' %
                          sorted(timer_gids))
    if events['done'] == 0 or events['timer'] == 0:
        violations.append('incomplete actors: done=%d timer=%d'
                          % (events['done'], events['timer']))
    print('=== %s (miles state) ===' % log_path)
    print('rows=%d events=%s N=%s max W-R=%d timer_gids=%s'
          % (len(rows), dict(events), N_prev, max_backlog,
             sorted(timer_gids)))
    if violations:
        print('  violations (%d): %s' % (len(violations), violations[:6]))
    else:
        print('  SPSC invariant 0 <= W-R <= N holds at every observation')
    print()
    return violations


def gate(m):
    """Content criteria.  Transport counters are deliberately absent."""
    bad = []
    if m['kind'] == 'log':
        if not m['writes']:
            bad.append('no write trace')
        if m['dups']:
            bad.append('%d exact repeats (cycle5=%d)' % (m['dups'], m['cycle5']))
        if m['zero'] * 20 > m['writes']:
            bad.append('%d zero buffers (>5%%)' % m['zero'])
    else:
        if not m['blocks']:
            bad.append('no PCM blocks')
        if m['dups']:
            bad.append('%d duplicate blocks' % m['dups'])
        if m['silent'] * 20 > m['blocks']:
            bad.append('%d silent blocks (>5%%)' % m['silent'])
        if m['seam_bad']:
            bad.append('%d seam discontinuities' % m['seam_bad'])
    return bad


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('paths', nargs='*', help='PCM dumps or [wave] Write logs')
    ap.add_argument('--gate', action='store_true',
                    help='exit 1 when the content criteria fail (CI mode)')
    ap.add_argument('--sync-log', metavar='RUN_TXT',
                    help='device log with [wave2] tick t=<ms>; gate fails when '
                         'the dump is shorter than the real audio time '
                         '(starvation hides silence from the PCM audit)')
    ap.add_argument('--miles-state', metavar='RUN_TXT',
                    help='device log with [wave2] miles w/r/N/gid rows; gate '
                         'fails when the SPSC contract w-r<=N, a stable '
                         'TimerProc gid or the no-waveOutWrite-in-callback '
                         'rule is broken (A-12.4 S0)')
    args = ap.parse_args()
    failed = []
    if args.miles_state:
        try:
            for v in miles_state_check(args.miles_state):
                failed.append('%s: miles state: %s' % (args.miles_state, v))
        except Exception as e:
            failed.append('%s: miles state FAILED %s' % (args.miles_state, e))
    for p in args.paths:
        try:
            m = (audit_log(p) if p.lower().endswith(('.txt', '.log'))
                 else audit(p))
            if args.sync_log and m['kind'] == 'pcm':
                sync = sync_check(args.sync_log, m['nbytes'])
                if sync:
                    span, dump_s, ratio = sync
                    print('SYNC: audio active %.2f s | dump %.2f s | '
                          'ratio %.2f %s' %
                          (span, dump_s, ratio,
                           'OK' if ratio >= 0.90 else 'STARVATION'))
                    m['sync_ratio'] = ratio
            if args.gate:
                bad = gate(m)
                if args.sync_log and m['kind'] == 'pcm':
                    if 'sync_ratio' not in m:
                        bad.append('no sync timestamps in log')
                    elif m['sync_ratio'] < 0.90:
                        bad.append('dump/audio-time ratio %.2f < 0.90: '
                                   'producer starvation (silence not in dump)'
                                   % m['sync_ratio'])
                if bad:
                    failed.append('%s: %s' % (p, '; '.join(bad)))
        except Exception as e:
            failed.append('%s: FAILED %s' % (p, e))
    if args.gate or args.miles_state:
        for line in failed:
            print('GATE FAIL ' + line)
        print('GATE %s (%d input(s))' % ('FAILED' if failed else 'PASS',
                                        len(args.paths)))
        sys.exit(1 if failed else 0)
