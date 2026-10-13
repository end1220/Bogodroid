"""Validate GUI dismissal and persistent invisibility from real container replays."""
import argparse
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('root', type=Path)
args = parser.parse_args()
summary = {}
for name in ('finish', 'buttons', 'exit'):
    directory = args.root / name
    if not directory.is_dir():
        directory = args.root / f'seq-ui-{name}'
    text = (directory / 'log.txt').read_text(encoding='utf-8', errors='replace')
    if name != 'exit':
        assert 'viaGUI=1' in text, name
    assert not re.search(r'\[BD-SEGV\]|terminate called|Invalid Reference|fatal error', text, re.I), name
    snapshots = re.findall(r'UI hidden renderers=(\d+) badAlpha=(\d+) actions=(\d+) activeActions=(\d+)', text)
    assert len(snapshots) >= 4, (name, 'missing visibility checks')
    assert all(int(renderers) >= 19 and bad == '0' and actions == '6' and active == '0'
               for renderers, bad, actions, active in snapshots), (name, snapshots)
    assert 'hooked CanvasRenderer.SetColor_Injected(1)' in text
    summary[name] = dict(visibility_checks=len(snapshots), hidden_renderers=int(snapshots[-1][0]),
                         hidden_finish_actions=6, bad_alpha=0, active_actions=0)
    if name == 'finish':
        assert 'DOWN finish' in text and 'Finish visible=1' in text
        assert text.count('Finish A close visible=0 menu=2') == 1
        closed = text.split('Finish A close visible=0 menu=2', 1)[1]
        assert 'start down touch=' in closed and 'DOWN spawn TetrisMino' in closed and 'DOWN step' in closed
        assert 'a down touch=' not in text and 'a up touch=' not in text, 'dismissal leaked A into gameplay'
        summary[name]['a_dismissed_to_menu'] = True
        summary[name]['game_restarted'] = True
    elif name == 'buttons':
        for key in ('start', 'x', 'guide', 'a', 'dpright'):
            assert f'{key} down touch=' in text and f'{key} up touch=' in text, (name, key)
        assert 'skin message ' not in text, 'Select changed skin'
        assert '[BD-PAD] replay: back at ' in text, 'Select not exercised'
        blocked = re.findall(r'UI blocked color renderer=.*alpha=([\d.]+)', text)
        assert blocked and any(float(alpha) > 0 for alpha in blocked), 'no attempted visibility restore exercised'
        summary[name]['blocked_color_writes_logged'] = len(blocked)
    else:
        assert text.count('Exit visible=1') == 2
        assert text.count('Exit B close visible=0 menu=2') == 1
        assert text.count('Exit A confirm requested') == 1
        assert 'nativeRender returned false - Unity requested quit' in text
        assert '[BD-PREFS] saved ' in text
        assert 'unityloader exit=0 after ' in (directory / 'runner.txt').read_text()
        assert 'skin message ' not in text
        for key in ('a', 'b'):
            assert f'{key} down touch=' not in text and f'{key} up touch=' not in text
        summary[name].update(a_quit=True, b_cancel=True, exit_code=0, select_disabled=True)
print(json.dumps(summary, ensure_ascii=False, indent=2))
