"""Check real replay logs; accepts host staging or the container game directory."""

import argparse
import json
import re
import statistics
from pathlib import Path

# Run without -O: assertions intentionally fail the integration verification.
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('root', type=Path, nargs='?', default=Path(__file__).resolve().parents[1] / 'deploy/BrickGamePro/down-fix-validation/held-piece-100-30')
parser.add_argument('--initial-ms', type=float, default=100)
parser.add_argument('--repeat-ms', type=float, default=30)
parser.add_argument('--legacy-continuous-hold', action='store_true',
                    help='Check archived builds predating the per-piece hold rule (no spawn-gate case).')
args = parser.parse_args()
root = args.root
summary = {}
step_re = re.compile(r'DOWN step (\w+) self=(\S+) time=([\d.]+) kind=(\w+) held=(\d) y=([-\d.]+)->([-\d.]+) status=(\d+)')

def read_case(name):
    directory = root / name
    if not directory.is_dir():
        directory = root / f'seq-down-{name}'
    text = (directory / 'log.txt').read_text(encoding='utf-8', errors='replace')
    assert 'viaGUI=1' in text, f'{name}: stale GUI probe'
    assert not re.search(r'\[BD-SEGV\]|terminate called|Invalid Reference|fatal error', text, re.I), name
    return text

def steps(text):
    return [dict(type=m[1], object=m[2], time=float(m[3]), kind=m[4],
                 held=int(m[5]), before=float(m[6]), after=float(m[7]))
            for m in step_re.finditer(text)]

def verify_steps(name):
    text = read_case(name)
    observed = steps(text)
    assert observed, name
    for item in observed:
        assert item['before'] - item['after'] in (0, 1), (name, item)
    intervals = [b['time'] - a['time'] for a, b in zip(observed, observed[1:])
                 if b['kind'] == 'repeat']
    assert all(dt >= (args.repeat_ms - 1) / 1000 for dt in intervals), (name, intervals)
    released = False
    hold_objects = set()
    for line in text.splitlines():
        if 'DOWN edge down' in line:
            released = False
            hold_objects.clear()
        if 'DOWN edge up' in line:
            released = True
        if released and 'DOWN step' in line:
            assert 'kind=tap' in line, (name, 'repeat after UP')
        match = step_re.search(line)
        if match:
            hold_objects.add(match[2])
            if not args.legacy_continuous_hold:
                assert len(hold_objects) == 1, (name, 'held Down accelerated successor')
    summary[name] = dict(steps=len(observed), taps=sum(s['kind'] == 'tap' for s in observed),
                         objects=len({s['object'] for s in observed}),
                         repeat_interval_ms=[round(1000 * min(intervals), 1),
                                             round(1000 * max(intervals), 1)] if intervals else [])
    return text, observed

baseline = read_case('baseline')
times = [float(t) for t in re.findall(r'DOWN original \w+ time=([\d.]+) playing=1 status=3', baseline)]
gaps = [b - a for a, b in zip(times, times[1:])]
assert len(gaps) >= 5
summary['baseline'] = dict(moves=len(times), interval_ms=[round(min(gaps)*1000, 1), round(max(gaps)*1000, 1)],
                           median_ms=round(statistics.median(gaps)*1000, 1))
fixed, observed = verify_steps('fixed')
assert f'initial={args.initial_ms:.0f}ms repeat={args.repeat_ms:.0f}ms' in fixed
assert observed[0]['kind'] == 'tap' and observed[1]['kind'] == 'repeat'
first = observed[1]['time'] - observed[0]['time']
assert first >= (args.initial_ms - 1) / 1000
summary['fixed']['first_repeat_ms'] = round(first * 1000, 1)
summary['fixed']['repeat_interval_ms'] = [round(min(b['time']-a['time'] for a,b in zip(observed[1:], observed[2:]))*1000, 1),
                                          round(max(b['time']-a['time'] for a,b in zip(observed[1:], observed[2:]))*1000, 1)]
life, _ = verify_steps('lifecycle')
assert summary['lifecycle']['objects'] >= 2 and 'DOWN finish' in life
assert 'DOWN step' not in life.split('DOWN finish', 1)[1]
assert 'DOWN edge up' in life.split('DOWN finish', 1)[1]
if args.legacy_continuous_hold:
    verify_steps('five-final')
for name in (() if args.legacy_continuous_hold else ('spawn-gate', 'five-final')):
    text, observed = verify_steps(name)
    assert text.count('DOWN edge down') == 2 and text.count('DOWN edge up') == 2
    assert summary[name]['taps'] == 2 and summary[name]['objects'] == 2
    assert 'repeatReset=1 held=1' in text, (name, 'no held spawn')
    first_hold = text.split('DOWN edge down', 1)[1].split('DOWN edge up', 1)[0]
    spawned = re.search(r'DOWN spawn (\w+) self=(\S+) repeatReset=1 held=1', first_hold)
    assert spawned, (name, 'first hold did not land')
    assert all(s['object'] != spawned[2] for s in steps(first_hold))
    assert re.search(r'DOWN original .*status=0.*xy=.*->', first_hold[spawned.end():]), (name, 'no natural fall after held spawn')
taps, observed = verify_steps('taps-final')
assert len(observed) == 12 and all(s['kind'] == 'tap' for s in observed)
landings = 0
for match, item in zip(step_re.finditer(taps), observed):
    if item['before'] == item['after']:
        # The original collision path restores y and then spawns the next mino.
        # Reject a stationary tap without the corresponding spawn.
        following = taps[match.end():].split('DOWN step', 1)[0]
        assert 'DOWN spawn TetrisMino' in following, item
        landings += 1
summary['taps-final']['moves'] = len(observed) - landings
summary['taps-final']['landings'] = landings
assert taps.count('DOWN edge down') == 12 and taps.count('DOWN edge up') == 12
pause, _ = verify_steps('gui-pause')
assert re.search(r'DOWN original .*status=2.*xy=5\.0,.*->6\.0,', pause)
assert re.search(r'DOWN original .*status=4', pause)
paused_edges = re.findall(r'DOWN edge down ticks=\d+ queued=0(.*?)DOWN edge up', pause, re.S)
assert len(paused_edges) == 1 and 'DOWN step' not in paused_edges[0]
tank = read_case('tank-final')
assert 'dpdown down touch=' in tank and 'dpdown up touch=' in tank and 'DOWN step' not in tank
summary['tank-final'] = {'route': 'touch'}
print(json.dumps(summary, ensure_ascii=False, indent=2))
