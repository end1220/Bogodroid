"""Check diagnostic container replays against actual mino movement/shape."""
import json
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
summary = {}
for case in ('tetris', 'five', 'pause', 'quick'):
    directory = root / case
    text = (directory / 'log.txt').read_text(errors='replace')
    assert 'unityloader exit=124' in (directory / 'runner.txt').read_text()
    for marker in ('[BD-SEGV]', 'terminate called', 'Invalid Reference', 'Assertion failed'):
        assert marker not in text, (case, marker)
    rotations = list(re.finditer(
        r'ROTATE step (\w+) self=(\S+) time=([\d.]+) status=(\d+) shape=(\S+)->(\S+) queued=(\d+)', text))
    edges = re.findall(r'ROTATE edge [ay] ticks=\d+ queued=(\d+)', text)
    expected = 6 if case == 'quick' else 4
    assert len(rotations) == expected, (case, len(rotations))
    assert len(edges) == expected + (case == 'pause'), (case, edges)
    if case == 'pause':
        assert edges[0] == '0', 'paused A must not queue a rotation'
    if case in ('tetris', 'five', 'pause'):
        for status in (1, 2):
            rotate = next(m for m in rotations if int(m[4]) == status)
            later = text[rotate.end():]
            moves = re.findall(
                rf'DOWN original \w+ time=([\d.]+) playing=1 status={status} '
                r'xy=([\d.-]+),[\d.-]+->([\d.-]+),[\d.-]+', later)
            assert any(0 <= float(t) - float(rotate[3]) < 0.8 and
                       (float(b) - float(a)) * (-1 if status == 1 else 1) > 0
                       for t, a, b in moves), (case, status, 'horizontal hold lost')
        # A preserved status is insufficient: verify real child world positions
        # changed, including both horizontal combo cases.
        if case != 'pause':
            for rotate in rotations:
                if int(rotate[4]) in (1, 2):
                    assert rotate[5] != rotate[6], (case, 'rotation had no visible effect')
    if case in ('tetris', 'five'):
        down_rotation = rotations[0]
        later_down = text[down_rotation.end():]
        assert re.search(r'DOWN step \w+.*kind=repeat held=1', later_down)
        assert not re.search(r'ROTATE original|status=4 xy=', text), 'duplicate touch rotation'
    summary[case] = dict(rotations=len(rotations), edges=len(edges),
                         changed_shapes=sum(m[5] != m[6] for m in rotations),
                         exit=124)
out = root / 'validation-summary.json'
out.write_text(json.dumps(summary, indent=2), encoding='utf-8')
print(out.read_text())
