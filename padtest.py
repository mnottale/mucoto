import mido
import rtmidi

# Initialize MIDI input
midi_in = rtmidi.MidiIn()

# Get list of available ports
ports = midi_in.get_ports()

print("Available MIDI input ports:")
for idx, name in enumerate(ports):
  print(f"{idx}: {name}")

out = mido.open_output('Launchpad Mini MK3:Launchpad Mini MK3 LPMiniMK3 MI')
inp = mido.open_input('Launchpad Mini MK3:Launchpad Mini MK3 LPMiniMK3 MI')

HDR = [0x00, 0x20, 0x29, 0x02, 0x0D]
out.send(mido.Message('sysex', data=HDR + [0x0E, 1]))   # programmer mode

def rgb(pad, r, g, b):                                   # 0-127 each
    out.send(mido.Message('sysex', data=HDR + [0x03, 3, pad, r, g, b]))

# palette colors, add 2 for a darker version
red = 5
yellow = 13
green = 21
cyan = 37
magenta = 53
blue = 45
brown = 9
white = 3
grey = 1

def spec_static(pad, colour):    return [0, pad, colour]
def spec_flash(pad, b, a):       return [1, pad, b, a]
def spec_pulse(pad, colour):     return [2, pad, colour]
def spec_rgb(pad, r, g, b):      return [3, pad, r, g, b]

def paint(*specs):
    data = HDR + [0x03]
    for s in specs:
        data += s
    out.send(mido.Message('sysex', data=data))

paint(
    spec_static(11, 13),
    spec_flash(12, 21, 23),
    spec_pulse(13, 37),
    spec_rgb(14, 127, 0, 64),
)
for msg in inp:
    print(msg)
    if msg.type == 'note_on' and msg.velocity:
        rgb(msg.note, 127, 0, 40)
    elif msg.type in ('note_off',) or (msg.type == 'note_on' and not msg.velocity):
        rgb(msg.note, 0, 0, 0)
