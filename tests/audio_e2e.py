"""End to end test of the audio path: graph processing, recorder, sound bank,
looper audio pads (spec's 'example audio flow')."""
import json
import time
import urllib.request

import mido

BASE = 'http://localhost:8123'


def api(path, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(BASE + path, data=data,
                                 method='POST' if body is not None else 'GET')
    with urllib.request.urlopen(req) as r:
        t = r.read().decode()
        return json.loads(t) if t else None


def raw(path):
    with urllib.request.urlopen(BASE + path) as r:
        return r.read()


def check(label, cond, extra=''):
    print(('OK  ' if cond else 'FAIL') + ' ' + label + (' ' + str(extra) if extra else ''))
    return cond


# audio graph: player -> gain -> recorder
api('/api/graph/node', {'type': 'player', 'name': 'ap'})
api('/api/graph/node', {'type': 'gain', 'name': 'ag'})
api('/api/graph/node', {'type': 'recorder', 'name': 'arec'})
api('/api/graph/link', {'from': 'ap', 'to': 'ag'})
api('/api/graph/link', {'from': 'ag', 'to': 'arec'})

pad = mido.open_output('padsim2', virtual=True)
time.sleep(0.5)
ports = api('/api/midi/ports')
pad_name = next(p['name'] for p in ports if 'padsim2' in p['name'] and p['input'])
pad_id = api('/api/midi/input', {'port': pad_name})['id']
api(f'/api/midi/input/{pad_id}/looper', {'value': 1})

NOTE = 62


def press(note, hold=0.05):
    pad.send(mido.Message('note_on', note=note, velocity=100))
    time.sleep(hold)
    pad.send(mido.Message('note_off', note=note))
    time.sleep(0.15)


def pad_state():
    cfg = api('/api/midi/config')
    inp = next(i for i in cfg['inputs'] if i['id'] == pad_id)
    return next((p for p in inp['pads'] if p['note'] == NOTE), None)


# spec audio flow: press unassigned pad, press the two CCs bound to a recorder
# and a sound bank, hold 1s to record, press to stop -> sample in bank
press(NOTE)
check('pad selected', pad_state()['state'] == 1)

api('/api/learn', {'action': 'pad_recorder', 'node': 'arec'})
pad.send(mido.Message('control_change', control=30, value=127))
time.sleep(0.3)
api('/api/learn', {'action': 'pad_player', 'node': 'ap'})
pad.send(mido.Message('control_change', control=31, value=127))
time.sleep(0.3)
pad.send(mido.Message('control_change', control=30, value=127))
time.sleep(0.2)
pad.send(mido.Message('control_change', control=31, value=127))
time.sleep(0.3)
st = pad_state()
check('pad assigned to audio', st['target'] == 2 and st['recorder'] == 'arec'
      and st['player'] == 'ap' and st['state'] == 3, st)

pad.send(mido.Message('note_on', note=NOTE, velocity=100))
time.sleep(1.2)
pad.send(mido.Message('note_off', note=NOTE))
time.sleep(0.3)
st = pad_state()
check('recording', st['state'] == 2)
g = api('/api/graph')
rec = next(n for n in g['nodes'] if n['name'] == 'arec')
check('recorder node enabled', rec['params'].get('enabled') == 1, rec['params'])

time.sleep(1.0)  # record ~1s of processed graph audio
press(NOTE)
st = pad_state()
check('sample stored on pad', st['state'] == 3 and st['sample'] != '', st['sample'])
sample = st['sample']

samples = api('/api/samples')
info = next((s for s in samples if s['name'] == sample), None)
frames = info and info['frames']
check('sample in bank, ~1s at 48k', info and 30000 < frames < 100000, frames)

data = raw(f'/api/sample/{sample}')
check('binary sample download', len(data) == frames * 4, len(data))

# tap -> plays once through the player node, scheduler returns pad to idle
press(NOTE)
check('audio pad playing', pad_state()['state'] == 4)
time.sleep(frames / 48000 + 0.6)
check('audio pad back to idle', pad_state()['state'] == 3, pad_state()['state'])

# double tap -> loops
press(NOTE)
press(NOTE, 0.05)
check('audio pad looping', pad_state()['state'] == 5, pad_state()['state'])
check('rolling with audio loop', api('/api/status')['rolling'] == 1)
press(NOTE)
check('audio loop stopped', pad_state()['state'] == 3)

# sample editor: cut
api(f'/api/sample/{sample}/cut', {'start': 0, 'end': 1000})
samples = api('/api/samples')
check('cut sample', next(s['frames'] for s in samples if s['name'] == sample) == 1000)

# save / load keeps samples and pads
api('/api/save', {})
api('/api/load', {})
st = pad_state()
samples = api('/api/samples')
check('state roundtrip keeps sample+pad',
      st and st['sample'] == sample
      and any(s['name'] == sample for s in samples), st and st['sample'])

# cleanup
api(f'/api/midi/input/{pad_id}/delete')
api(f'/api/sample/{sample}/delete')
for n in ('ap', 'ag', 'arec'):
    api(f'/api/graph/node/{n}/delete')
print('done')
