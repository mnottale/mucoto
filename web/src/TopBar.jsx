import React, { useEffect, useState } from 'react'
import { post } from './api.js'

export default function TopBar({ status, midi, refreshMidi, onError }) {
  const [bpm, setBpm] = useState('120')
  const [editing, setEditing] = useState(false)

  useEffect(() => {
    if (!editing && status) setBpm(String(Math.round(status.bpm * 100) / 100))
  }, [status, editing])

  const commitBpm = () => {
    setEditing(false)
    post(`/api/bpm/${parseFloat(bpm) || 120}`).catch(onError)
  }
  const setRoute = (inputId, output) =>
    post(`/api/midi/input/${inputId}/route`, { channel: -1, output: parseInt(output) })
      .then(refreshMidi).catch(onError)
  const learnRoute = (inputId, output) =>
    post('/api/learn', { action: 'default_route', input: inputId, output: parseInt(output) })
      .catch(onError)

  const measure = status?.rolling ? Math.floor(status.measures) + 1 : null
  const beat = status?.rolling ? (Math.floor(status.measures * 4) % 4) + 1 : null

  return (
    <div className="topbar">
      <span className="brand">MuCoTo</span>
      <label>
        BPM
        <input className="bpm" value={bpm} onFocus={() => setEditing(true)}
          onChange={(e) => setBpm(e.target.value)}
          onBlur={commitBpm}
          onKeyDown={(e) => e.key === 'Enter' && e.target.blur()} />
      </label>
      <span className={status?.rolling ? 'measures rolling' : 'measures'}>
        {measure !== null ? `${measure}.${beat}` : '–.–'}
      </span>
      <div className="routes">
        {midi.inputs.filter((i) => !i.looper).map((i) => (
          <span key={i.id} className="route">
            <span className={i.connected ? 'dev ok' : 'dev'} title={i.port}>
              {i.port.split(':')[0]}
            </span>
            →
            <select value={i.defaultRoute} onChange={(e) => setRoute(i.id, e.target.value)}>
              <option value={-1}>none</option>
              {midi.outputs.map((o) => (
                <option key={o.id} value={o.id}>{o.port.split(':')[0]}</option>
              ))}
            </select>
            <button className="learn" title="bind a CC to set this route"
              onClick={() => learnRoute(i.id, i.defaultRoute)}>L</button>
          </span>
        ))}
      </div>
      {status?.learning ? (
        <span className="learning">
          waiting for CC…
          <button onClick={() => post('/api/learn/cancel').catch(onError)}>cancel</button>
        </span>
      ) : null}
    </div>
  )
}
