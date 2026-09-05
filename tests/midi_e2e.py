"""End to end test of the MuCoTo looper via virtual alsa midi ports."""
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


def check(label, cond, extra=''):
    print(('OK  ' if cond else 'FAIL') + ' ' + label + (' ' + str(extra) if extra else ''))
    return cond


# virtual devices: pad (looper source), kbd (keyboard source), synth (instrument sink)
pad = mido.open_output('padsim', virtual=True)
kbd = mido.open_output('kbdsim', virtual=True)
synth = mido.open_input('synthsink', virtual=True)
time.sleep(0.5)

ports = api('/api/midi/ports')
find = lambda sub, key: next(p['name'] for p in ports if sub in p['name'] and p[key])
pad_name = find('padsim', 'input')
kbd_name = find('kbdsim', 'input')
synth_name = find('synthsink', 'output')

pad_id = api('/api/midi/input', {'port': pad_name})['id']
kbd_id = api('/api/midi/input', {'port': kbd_name})['id']
synth_id = api('/api/midi/output', {'port': synth_name})['id']
api(f'/api/midi/input/{pad_id}/looper', {'value': 1})
api(f'/api/midi/input/{kbd_id}/route', {'channel': -1, 'output': synth_id})
cfg = api('/api/midi/config')
check('config: 2 inputs 1 output connected',
      all(i['connected'] for i in cfg['inputs']) and cfg['outputs'][0]['connected'])

NOTE = 51


def press(note, hold=0.05):
    pad.send(mido.Message('note_on', note=note, velocity=100))
    time.sleep(hold)
    pad.send(mido.Message('note_off', note=note))
    time.sleep(0.15)


def pad_state():
    cfg = api('/api/midi/config')
    inp = next(i for i in cfg['inputs'] if i['id'] == pad_id)
    return next((p for p in inp['pads'] if p['note'] == NOTE), None)


# 1. press unassigned pad -> Selected (white)
press(NOTE)
st = pad_state()
check('press unassigned -> Selected', st and st['state'] == 1, st and st['state'])
status = api('/api/status')
check('current pad tracked', status['padInput'] == pad_id and status['padNote'] == NOTE)

# 2. learn: bind CC20 to "assign to instrument synth"
api('/api/learn', {'action': 'pad_instrument', 'output': synth_id})
check('learn armed', api('/api/status')['learning'] == 1)
pad.send(mido.Message('control_change', control=20, value=127))
time.sleep(0.3)
check('learn done', api('/api/status')['learning'] == 0)
# now actually press the newly bound CC to run the assignment
pad.send(mido.Message('control_change', control=20, value=127))
time.sleep(0.3)
st = pad_state()
check('CC assigns instrument -> Idle', st['state'] == 3 and st['target'] == 1
      and st['instrument'] == synth_id, st)

# 3. hold >1s, release -> Recording
pad.send(mido.Message('note_on', note=NOTE, velocity=100))
time.sleep(1.2)
pad.send(mido.Message('note_off', note=NOTE))
time.sleep(0.2)
st = pad_state()
check('hold+release -> Recording', st['state'] == 2, st['state'])

# 4. play two notes on the keyboard, they are routed to synth and recorded
t0 = time.time()
for n in (60, 64):
    kbd.send(mido.Message('note_on', note=n, velocity=99))
    time.sleep(0.25)
    kbd.send(mido.Message('note_off', note=n))
    time.sleep(0.1)
# 5. press again -> stop recording, clip stored
press(NOTE)
st = pad_state()
check('stop recording -> Idle with clip', st['state'] == 3
      and len(st['clip']['events']) == 4, st and len(st['clip']['events']))
check('clip length sane', st and 0.5 < st['clip']['length'] < 3.0,
      st and st['clip']['length'])

# drain synth of live-routed notes
time.sleep(0.3)
drained = list(synth.iter_pending())
check('keyboard was routed to synth live', len(drained) >= 4, len(drained))

# 6. single tap -> plays once
press(NOTE)
st = pad_state()
check('tap -> PlayingOnce', st['state'] == 4, st['state'])
got = []
end = time.time() + 3
while time.time() < end:
    m = synth.poll()
    if m and m.type.startswith('note'):
        got.append(m)
    time.sleep(0.005)
check('clip replayed to synth', len([m for m in got if m.type == 'note_on' and m.velocity > 0]) >= 2,
      [str(m) for m in got][:6])
st = pad_state()
check('back to Idle after play once', st['state'] == 3, st['state'])

# 7. double tap -> looping
press(NOTE, 0.05)
time.sleep(0.1)
press(NOTE, 0.05)
st = pad_state()
check('double tap -> Looping', st['state'] == 5, st['state'])
time.sleep(0.5)
check('transport rolling', api('/api/status')['rolling'] == 1)

# 8. tap again -> stop
press(NOTE)
st = pad_state()
check('tap while looping -> stop', st['state'] == 3, st['state'])
time.sleep(0.3)
check('transport stopped', api('/api/status')['rolling'] == 0)

# 9. clip editor api round trip
clip = api(f'/api/clip/{pad_id}/{NOTE}')
clip['events'] = clip['events'][:2]
api(f'/api/clip/{pad_id}/{NOTE}/set', clip)
check('clip edit roundtrip', len(api(f'/api/clip/{pad_id}/{NOTE}')['events']) == 2)

# 10. learn node param via CC
api('/api/graph/node', {'type': 'gain', 'name': 'gtest'})
api('/api/learn', {'action': 'node_param', 'node': 'gtest', 'param': 'gain',
                   'min': 0, 'max': 2})
kbd.send(mido.Message('control_change', control=21, value=127))
time.sleep(0.3)
kbd.send(mido.Message('control_change', control=21, value=64))
time.sleep(0.3)
g = api('/api/graph')
gv = next(n for n in g['nodes'] if n['name'] == 'gtest')['params']['gain']
check('CC drives node param', abs(gv - 2 * 64 / 127) < 0.01, gv)
api('/api/graph/node/gtest/delete')

# cleanup
api(f'/api/pad/{pad_id}/{NOTE}/clear')
api(f'/api/midi/input/{pad_id}/delete')
api(f'/api/midi/input/{kbd_id}/delete')
api(f'/api/midi/output/{synth_id}/delete')
print('done')
