# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Skorchekd. See LICENSE and NOTICE.
"""Client for the opt-in AtomicHeartMenu local test harness."""
import argparse, json, math, time
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('command', nargs='?', default='status')
p.add_argument('value', nargs='?', type=float, default=0)
p.add_argument('--save', type=Path)
p.add_argument('--directory', type=Path, help='Harness directory for a staged build')
a = p.parse_args()
root = a.directory or Path(__file__).resolve().parents[1] / 'bin' / 'AtomicHeartMenu.tests'
def read(): return json.loads((root / 'status.json').read_text(encoding='utf-8'))
state = read()
if not math.isfinite(a.value): p.error('finite value required')
if a.command != 'status':
    if not a.command.replace('_', '').isalnum(): p.error('invalid command name')
    session, request = state['session'], state['accepted_id'] + 1
    sampled = None
    temp = root / 'command.tmp'
    temp.write_text(f'{session} {request} {a.command} {a.value}\n', encoding='ascii')
    temp.replace(root / 'command.txt')
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        time.sleep(0.15)
        state = read()
        if state['session'] != session: raise SystemExit('Session changed; outcome uncertain')
        if state['processed_id'] >= request:
            if sampled is None: sampled = state['written_at_ms']
            elif state['sampled_at_ms'] >= sampled: break
    else: raise SystemExit('No dispatch and fresh observation within 8 seconds')
result = json.dumps(state, indent=2)
if a.save:
    a.save.parent.mkdir(parents=True, exist_ok=True)
    a.save.write_text(result + '\n', encoding='utf-8')
print(result)
