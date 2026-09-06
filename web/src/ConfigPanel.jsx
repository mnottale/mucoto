import React, { useEffect, useState } from 'react'
import { get, post, COLOR_CSS, PALETTE } from './api.js'

// program change slider for one midi output, cc learnable
function ProgramControl({ output, onError }) {
  const [program, setProgram] = useState(0)
  const [channel, setChannel] = useState(0)

  const send = (value) => {
    setProgram(value)
    post(`/api/midi/output/${output.id}/program`,
      { channel, value: parseInt(value) }).catch(onError)
  }
  const learn = () =>
    post('/api/learn', { action: 'program', output: output.id,
      outChannel: channel, min: 0, max: 127 }).catch(onError)

  return (
    <span className="programctl" title="program change">
      pgm
      <input type="range" min={0} max={127} value={program}
        onChange={(e) => send(e.target.value)} />
      <input className="pnum" type="number" min={0} max={127} value={program}
        onChange={(e) => send(e.target.value)} />
      ch
      <input className="pnum chnum" type="number" min={0} max={15} value={channel}
        onChange={(e) => setChannel(parseInt(e.target.value) || 0)} />
      <button className="learn" title="bind a CC to the program value" onClick={learn}>
        L
      </button>
    </span>
  )
}

export default function ConfigPanel({ midi, refresh, onError }) {
  const [ports, setPorts] = useState([])
  const [file, setFile] = useState('')

  const loadPorts = () => get('/api/midi/ports').then(setPorts).catch(onError)
  useEffect(() => { loadPorts() }, [])

  const act = (p) => p.then(refresh).catch(onError)

  return (
    <div className="config">
      <div className="col">
        <h3>Alsa MIDI ports <button onClick={loadPorts}>refresh</button></h3>
        <table>
          <tbody>
            {ports.map((p) => (
              <tr key={p.name}>
                <td>{p.name}</td>
                <td>
                  {p.input ? (
                    <button onClick={() => act(post('/api/midi/input', { port: p.name }))}>
                      + input
                    </button>
                  ) : null}
                  {p.output ? (
                    <button onClick={() => act(post('/api/midi/output', { port: p.name }))}>
                      + output
                    </button>
                  ) : null}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <div className="col">
        <h3>MIDI inputs</h3>
        <table>
          <tbody>
            {midi.inputs.map((i) => (
              <tr key={i.id}>
                <td className={i.connected ? 'dev ok' : 'dev'}>{i.port}</td>
                <td>
                  <label>
                    <input type="checkbox" checked={!!i.looper}
                      onChange={(e) =>
                        act(post(`/api/midi/input/${i.id}/looper`, { value: e.target.checked ? 1 : 0 }))} />
                    looper
                  </label>
                </td>
                <td>
                  <button onClick={() => act(post(`/api/midi/input/${i.id}/delete`))}>✕</button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <h3>MIDI outputs (instruments)</h3>
        <table>
          <tbody>
            {midi.outputs.map((o, idx) => (
              <tr key={o.id}>
                <td>
                  <span className="chip"
                    style={{ background: COLOR_CSS[PALETTE[idx % PALETTE.length]] }} />
                </td>
                <td className={o.connected ? 'dev ok' : 'dev'}>{o.port}</td>
                <td><ProgramControl output={o} onError={onError} /></td>
                <td>
                  <button onClick={() => act(post(`/api/midi/output/${o.id}/delete`))}>✕</button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <button onClick={() => act(post('/api/midi/rescan'))}>reconnect devices</button>
        <h3>Bindings</h3>
        <table>
          <tbody>
            {midi.bindings.map((b, i) => (
              <tr key={i}>
                <td>CC {b.cc} ch {b.channel}</td>
                <td>{b.action} {b.node} {b.param}
                  {b.output >= 0 ? ` out ${b.output}` : ''}</td>
                <td>
                  <button onClick={() => act(post(`/api/binding/${i}/delete`))}>✕</button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <h3>State</h3>
        <input placeholder="mucoto.json" value={file}
          onChange={(e) => setFile(e.target.value)} />
        <button onClick={() => post('/api/save', { file }).catch(onError)}>save</button>
        <button onClick={() => post('/api/load', { file }).then(refresh).catch(onError)}>
          load
        </button>
      </div>
    </div>
  )
}
