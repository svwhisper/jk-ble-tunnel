"""Per-phone-session timeline from passive capture.jsonl files (read-only).

One row per phone session on Node B (app connected -> app disconnected).
All times are host receipt times in seconds after the phone connected; they
are not radio timestamps. A's UDP lines arrive duplicated and B's USB arrives
in fragments, so B is reassembled into lines and A is deduplicated on the
device uptime stamp.

Columns:
  start   phone connect (B)          link  A link state when the app's 0x97
  dur     phone connected (s)              opener reached B: 1=down, 2=up
  scan    A started scanning         boot  A sent opener trilogy to the bank
  cache   B sent cached burst        dev   first live devinfo sent to phone
  strm    first live bytes aligned and forwarded to the phone
  end     disconnect reason (0x213 = phone closed it)
  A-notes A failures for that bank during the session
"""
import json, re, sys
from datetime import datetime

ANSI = re.compile(r'\x1b\[[0-9;]*m')
LOG = re.compile(r'[IWE] \((\d+)\) (\w+): (.*)')


def lines(path):
    """Yield (time, source, tag, text) for each complete log line."""
    buf, seen = '', set()
    for raw in open(path):
        r = json.loads(raw)
        d = r['data']
        if not isinstance(d, str):
            continue
        t = datetime.fromisoformat(r['at'])
        d = ANSI.sub('', d)
        src = r['source']
        if src == 'B-USB':
            buf += d
            *done, buf = buf.split('\n')
            for ln in done:
                m = LOG.search(ln)
                if m:
                    yield t, 'B', m.group(2), m.group(3).strip()
        elif src.startswith('UDP') or src == 'A-UDP':
            for ln in d.split('\n'):
                m = LOG.search(ln)
                if m and m.group(0) not in seen:
                    seen.add(m.group(0))
                    yield t, 'A', m.group(2), m.group(3).strip()
        elif src == 'MQTT':
            for ln in d.split('\n'):
                if 'llevent' in ln:
                    yield t, 'M', 'llevent', ln


def sessions(path):
    open_s, out = {}, []
    for t, src, tag, txt in lines(path):
        if src == 'B':
            m = re.match(r'app connected -> identity (\d+)', txt)
            if m:
                open_s[int(m.group(1))] = {'id': int(m.group(1)), 't0': t, 'ev': []}
                continue
            m = re.match(r'app disconnected from identity (\d+) h=\d+ reason=(0x\w+)', txt)
            if m and int(m.group(1)) in open_s:
                s = open_s.pop(int(m.group(1)))
                s['t1'], s['reason'] = t, m.group(2)
                out.append(s)
                continue
        # attribute the event to every open session for the bank it names
        m = (re.search(r'(?:identity|id[ =]|bms )(\d+)', txt) or
             re.search(r'"bms":(\d+)', txt))
        if m and int(m.group(1)) in open_s:
            open_s[int(m.group(1))]['ev'].append((t, src, tag, txt))
    for s in open_s.values():
        s['t1'], s['reason'] = None, 'open'
        out.append(s)
    return sorted(out, key=lambda s: s['t0'])


def first(s, src, pat):
    for t, sr, tag, txt in s['ev']:
        if sr == src and re.search(pat, txt):
            return (t - s['t0']).total_seconds(), txt
    return None, None


def row(s):
    f = lambda v: '   -' if v is None else f'{v:4.1f}'
    link = first(s, 'B', r'opener 0x97 owed link=(\d)')[1]
    link = re.search(r'link=(\d)', link).group(1) if link else '-'
    notes = []
    for t, sr, tag, txt in s['ev']:
        if sr == 'A' and re.search(r'failed|timed out|LL disconnect|timeout', txt):
            notes.append(f"{(t - s['t0']).total_seconds():.1f}:" +
                         re.sub(r'ble_owner: |bms \d+ ', '', txt)[:34])
    dur = (s['t1'] - s['t0']).total_seconds() if s['t1'] else None
    cols = [first(s, 'A', r'scanning for|connecting to')[0],
            first(s, 'A', r'opener trilogy')[0],
            first(s, 'B', r'replay deliver')[0],
            first(s, 'B', r'diag dev id=\d+ src=live')[0],
            first(s, 'B', r'stream start id=\d+ aligned')[0]]
    return (f"{s['t0']:%m-%d %H:%M:%S} TUN{s['id']} {f(dur)} {link:>4} "
            + ' '.join(f(c) for c in cols)
            + f" {s['reason']:>6}  " + '; '.join(notes))


if __name__ == '__main__':
    print('start          bank  dur link scan boot cache  dev strm    end  A-notes')
    for p in sys.argv[1:]:
        print(f'# {p}')
        for s in sessions(p):
            print(row(s))
