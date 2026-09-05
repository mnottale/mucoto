import React, { useState } from 'react'
import { post, COLOR_CSS, padColor } from './api.js'

// pad states, mirrors midi::Pad::St
const ST = ['empty', 'selected', 'recording', 'idle', 'playing', 'looping']

export default function LooperPanel({ midi, graph, status, refresh, onEdit, onError }) {
  const [sel, setSel] = useState(null) // {input, note}
  const loopers = midi.inputs.filter((i) => i.looper)
  const recorders = (graph.nodes || []).filter((n) => n.type === 'recorder')
  const players = (graph.nodes || []).filter((n) => n.type === 'player')

  const padAt = (looper, note) => looper.pads.find((p) => p.note === note)
  const selLooper = sel && loopers.find((l) => l.id === sel.input)
  const selPad = selLooper && padAt(selLooper, sel.note)
  const isCurrent = (l, note) =>
    status && status.padInput === l.id && status.padNote === note

  const learn = (body) => post('/api/learn', body).catch(onError)

  return (
    <div className="looper">
      {!loopers.length && (
        <p className="hint">No looper device. Mark a MIDI input as looper in the Config tab.</p>
      )}
      <div className="loopergrids">
        {loopers.map((l) => (
          <div key={l.id} className="loopergrid">
            <h3 className={l.connected ? 'dev ok' : 'dev'}>{l.port}</h3>
            <div className="padgrid">
              {Array.from({ length: 8 }).map((_, r) => {
                const row = 8 - r
                return Array.from({ length: 8 }).map((_, c) => {
                  const note = row * 10 + c + 1
                  const pad = padAt(l, note)
                  const st = pad ? ST[pad.state] : 'empty'
                  const color = pad ? (pad.state === 1 ? 3 : pad.state === 2 ? 5 : padColor(pad, midi.outputs)) : 0
                  const hasContent = pad &&
                    (pad.target === 1 ? pad.clip.events.length : pad.sample)
                  return (
                    <div key={note}
                      className={'pad ' + st
                        + (sel?.input === l.id && sel?.note === note ? ' sel' : '')
                        + (isCurrent(l, note) ? ' current' : '')
                        + (pad && !hasContent && pad.state === 3 ? ' dim' : '')}
                      style={{ background: COLOR_CSS[color] || '#777' }}
                      title={`note ${note}`}
                      onClick={() => setSel({ input: l.id, note })} />
                  )
                })
              })}
            </div>
          </div>
        ))}
      </div>
      {sel && (
        <div className="paddetails">
          <h3>Pad {sel.note}</h3>
          {selPad ? (
            <p>
              state: {ST[selPad.state]}
              {selPad.target === 1 && ` — instrument ${
                midi.outputs.find((o) => o.id === selPad.instrument)?.port.split(':')[0]
                ?? selPad.instrument}, ${selPad.clip.events.length} events`}
              {selPad.target === 2 && ` — audio ${selPad.player || '?'}${
                selPad.sample ? ` / ${selPad.sample}` : ''}`}
            </p>
          ) : (
            <p className="hint">never pressed</p>
          )}
          <div className="padactions">
            {selPad?.target === 1 && selPad.clip.events.length > 0 && (
              <button onClick={() => onEdit({ kind: 'clip', input: sel.input, note: sel.note })}>
                edit clip
              </button>
            )}
            {selPad?.target === 2 && selPad.sample && (
              <button onClick={() => onEdit({ kind: 'sample', name: selPad.sample })}>
                edit sample
              </button>
            )}
            {selPad && (
              <button className="danger"
                onClick={() => post(`/api/pad/${sel.input}/${sel.note}/clear`)
                  .then(refresh).catch(onError)}>
                clear pad
              </button>
            )}
          </div>
        </div>
      )}
      <div className="learnbox">
        <h3>Assignment CCs (press a pad first, then the learnt CC)</h3>
        <div className="learnrow">
          <span>assign to instrument</span>
          <LearnSelect options={midi.outputs.map((o) => [o.id, o.port.split(':')[0]])}
            onLearn={(v) => learn({ action: 'pad_instrument', output: v })} />
        </div>
        <div className="learnrow">
          <span>assign to recorder</span>
          <LearnSelect options={recorders.map((n) => [n.name, n.name])}
            onLearn={(v) => learn({ action: 'pad_recorder', node: v })} />
        </div>
        <div className="learnrow">
          <span>assign to player</span>
          <LearnSelect options={players.map((n) => [n.name, n.name])}
            onLearn={(v) => learn({ action: 'pad_player', node: v })} />
        </div>
      </div>
    </div>
  )
}

function LearnSelect({ options, onLearn }) {
  const [val, setVal] = useState(options[0]?.[0] ?? '')
  const cur = options.find(([v]) => String(v) === String(val)) ? val : options[0]?.[0]
  return (
    <span>
      <select value={cur ?? ''} onChange={(e) => setVal(e.target.value)}>
        {options.map(([v, label]) => <option key={v} value={v}>{label}</option>)}
      </select>
      <button className="learn" disabled={cur === undefined}
        onClick={() => onLearn(typeof options[0]?.[0] === 'number' ? parseInt(cur) : cur)}>
        L
      </button>
    </span>
  )
}
