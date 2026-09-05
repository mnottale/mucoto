# MuCoTo — Music Control Tower

C++26/WebUI MIDI/audio music edition software focused on live performance.
Full specification in [notes.md](notes.md).

## Build

Backend (needs gcc16 with `-freflection`, alsa, jsoncpp):

    cd src && make

Web UI:

    cd web && npm install && npm run build

## Run

    src/mucoto            # listens on :8000, serves web/dist
    src/mucoto :9000      # custom listen address
    src/mucoto -d         # --midi-debug: trace midi events + routing to stderr

Run from the repo root (or from `src/`); `node_specs.json` and `web/dist`
are located relative to the working directory. Overridable with the
`MUCOTO_WEB`, `MUCOTO_SPECS` and `MUCOTO_STATE` environment variables.

State is auto-loaded from `mucoto.json` at startup; save/load from the
Config tab (or `POST /api/save`, `/api/load`).

Web development with hot reload (proxies `/api` to :8000):

    cd web && npm run dev

## Layout

- `src/audio.{hh,cc}` — multithreaded audio DAG engine: alsa in (singleton,
  clocks the graph, software-clock fallback), alsa out, sound bank players,
  mixer, recorder, gain / quantizer / distortion / reverb effects.
- `src/midi.{hh,cc}` — alsa seq MIDI path: input→output routing with
  per-channel overrides, looper pad devices (Launchpad programmer-mode LED
  feedback), MIDI clip record/playback, MIDI learn.
- `src/main.cc` — REST API (annotation-based, `rest.hh`), JSON save/load,
  static file serving.
- `src/node_specs.json` — node parameter descriptions (name, bounds, docs)
  consumed by the web UI.
- `web/` — React UI: top bar (BPM, measures, routes + learn), graph editor,
  looper panel, sample bank, audio sample editor, MIDI piano-roll editor,
  MIDI device config.

## End to end tests

With the server running on :8123 (fresh state):

    venv/bin/python tests/midi_e2e.py
    venv/bin/python tests/audio_e2e.py
