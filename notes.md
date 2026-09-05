# Music Control Tower

MuCoTo is a C++/WebUI MIDI/audio music edition software with a focus on live performances.

## Audio path

Multithreaded C++ audio processing engine.

Implements an audio processing directed acyclic graph.

Nodes come in four types: input, output, mixer and effect.

Nodes can have float and string named parameters that can be read and written (int are sent as float parameters).

A node output can be sent to multiple nodes.

### Input nodes

#### Alsa in singleton

Reads from an alsa audio device.

Parameters: device, format(S16_LE, S32_LE), rate, channels, buffer_size

Input: None
Output: N channel audio

#### Sound bank player

Input: None
Output: 1 channel audio

Play a recorded sample from those stored, keyed by name. Each bank can play multiple samples at the same time, looping or not.
All sound banks share the same sample bank.

### Mixer node

Input: N 1 channel audio connections
Output: the sum of all channels weighted by their gains
Parameters: gain_0, gain_1, ... one per input channel

### Output node

#### Alsa output

Input: 1 or 2 one channel audio
Output: None
Parameters: device, format

#### Recorder

Store audio data for further use
Input: 1 channel audio
Parameters: duration (max buffer duration), enabled(recording active), clear (clear data at set to 1 time)

Recorded samples can be sent to the sample bank or exfiltered

### Effect nodes

Effect nodes have 1 channel audio input and one channel output.

#### Gain
#### Quantizer (for that cool 8bit effect)
#### Reverb
#### Distortion

### Data flow

Internally sound data is represented as float values at the input sample rate (so conversion at the alsa input/alsa output nodes).

A pool of threads is initialized and listens on a channel for tasks.

The singleton alsa input clocks everything.
When a N channel sample buffer is acquired, active sound bank players are queried for that same buffer size. This forms the input layer of the graph.
The graph is then walked by the alsa reader thread: each time a node with inputs available is encountered, a request is sent to the channel to process this node. This request will be picked by one thread from the pool and handled by making the node process the data.


### Avoiding parameter discovery

To avoid a discovery API, a JSON spec file describing the parameters of each node type is fine (name, type, bounds, short description).

## Main app and MIDI path

The main C++ app and MIDI path exposes a REST API for the web interface. It also drives the audio library.

Use my src/rest.hh C++26 annotation based API to build the REST routes (examples at the end), it includes the webserver inmplementation (cppgow).

### Midi inputs and outputs

Multiple midi (keyboards) inputs are supported. Each MIDI channel of each input can be routed to any MIDI output instrument (a default route and a per-channel route override).

Multiple MIDI (instruments) outputs are supported.

### Looper device

(see padtest.py for sample python code for the device I intend to use as reference)

A MIDI input can be treated as a "looper" device. Intended for pad grid devices (like a 8x8 button matrix), those devices do not send their notes to midi outputs but are used to create and play MIDI notes bank and sound banks.

Each of the main keys (notes) of a looper device is associated with a target MIDI instrument+a notes sample, or a audio sound bank player + sample name and an audio recorder node.

Hiting a key plays the associated sample once (either MIDI or audio), hitting twice plays in a loop. Hold for more than 1s starts recording when the key is released. Press again to stop recording.

Feedback is sent to the looper device by lighting the RGB led under each key. Color is the target instrument or audio, flashing when playing, red when recording.

Example flow:

I press once an unassigned pad. It turns white. I press control change label 2 bound to instrument 2, color turns green. Then I hold the same button for 1s and release. It turns red, MIDI input is recorded and sent to MIDI output instrument 2. I press it again, it turns solid green.
Then I press it twice, the recorded pattern starts playing on instrument 2 in a loop.

Example audio flow:

I press an unasigned pad. It turns white. I press the two CC associated with a recorder and a sound bank to assign. Then I hold for 1s and release, the recorder node is enabled. I press again to stop recording, the recorder node sample data is moved to the sound bank, generating a name for the sample, and that name become associated with the pad.

### Audio

The service exposes an API to build, run, parameterize a single audio graph.

### MIDI learn

Some actions can be bound to MIDI control change messages. Those CC messages can come from any MIDI input including loopers.

The system can learn associations by using the next CC messages that comes in after learn mode is activated for a specific target.

Learnable actions:
- audio graph node float parameter change
- set default route of a MIDI input to a specific MIDI output
- assign current looper bank button to a MIDI output instrument
- assign current looper bank button to an audio recorder
- assign current looper bank button to an audio soundbank player

### Saving and loading

The service can save its entire state to a (JSON) file. Including:

- the audio graph and current parameter values
- all sound banks
- midi input/output device names
- midi banks
- looper device assignation
- learnt MIDI associations

## Web interface

In react.

### Configuration panel

Configure input and output midi devices.

### Main view

Made of a top bar and two panels.

#### Top bar

Shows/set BPM and shows measures when rolling.

Shows also MIDI input/outputs and assignments (with MIDI learning capability).

#### Audio graph panel

Graph can be fully edited (add/remove nodes and links).

All parameters can be modified. All float parameters can be MIDI learned.

### Looper state panel

Replicates what is shown on the looper LEDs.

### Audio sample editor

Allows cutting the sample with helpers to select an integral number of measures.

### MIDI sample editor

Allows moving/adding/removing/resizing notes.

Allows cutting sections of the timeline.

Shows measures.

Has a snap all notes to 1/Nth of a measure feature.

