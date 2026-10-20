#!/usr/bin/env python3
"""Summarize Tank timer-reset replays and startup durations from container logs."""
import argparse
import json
import re
import statistics
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path)
a=p.parse_args()
def read(case):
    return (a.root/case/'log.txt').read_text(encoding='utf-8',errors='replace')
def intervals(log,direction):
    times=[float(t) for t in re.findall(r'TANK move direction='+str(direction)+r' time=([0-9.]+)',log)]
    return [b-a for a,b in zip(times,times[1:])]
result={'movement':{},'splash_ms':{}}
for d,name in enumerate(['right','left','up','down']):
    base=intervals(read('baseline'),d)
    if not base: base=intervals(read('up-baseline'),d)
    fast=intervals(read('fast'),d)
    assert len(base)>=4 and len(fast)>=4,(name,base,fast)
    b,f=statistics.median(base),statistics.median(fast)
    assert 1.5 < b/f < 2.6,(name,b,f)
    result['movement'][name]={'baseline_ms':round(b*1000),'fast_ms':round(f*1000),'speed_ratio':round(b/f,2)}
for case in ['baseline','fast','splash-early-ready','exit']:
    log=read(case)
    start=float(re.search(r'\+([0-9.]+)ms startup image visible',log)[1])
    end=float(re.search(r'\+([0-9.]+)ms startup animation finished',log)[1])
    assert end-start>=3000,(case,start,end)
    result['splash_ms'][case]=round(end-start)
runner=(a.root/'exit'/'runner.txt').read_text(encoding='utf-8')
assert 'unityloader exit=0 ' in runner
result['exit']=0
(a.root/'validation-summary.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
