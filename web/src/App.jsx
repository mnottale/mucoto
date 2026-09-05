import React, { useCallback, useEffect, useState } from 'react'
import { get } from './api.js'
import TopBar from './TopBar.jsx'
import GraphPanel from './GraphPanel.jsx'
import LooperPanel from './LooperPanel.jsx'
import SamplesPanel from './SamplesPanel.jsx'
import ConfigPanel from './ConfigPanel.jsx'
import SampleEditor from './SampleEditor.jsx'
import MidiEditor from './MidiEditor.jsx'

const TABS = [
  ['graph', 'Audio graph'],
  ['looper', 'Looper'],
  ['samples', 'Samples'],
  ['config', 'Config'],
]

export default function App() {
  const [status, setStatus] = useState(null)
  const [specs, setSpecs] = useState({})
  const [graph, setGraph] = useState({ nodes: [], links: [] })
  const [midi, setMidi] = useState({ inputs: [], outputs: [], bindings: [] })
  const [samples, setSamples] = useState([])
  const [tab, setTab] = useState('graph')
  const [editor, setEditor] = useState(null) // {kind:'sample',name} | {kind:'clip',input,note}
  const [error, setError] = useState(null)

  const refreshGraph = useCallback(() => get('/api/graph').then(setGraph).catch(setError), [])
  const refreshMidi = useCallback(() => get('/api/midi/config').then(setMidi).catch(setError), [])
  const refreshSamples = useCallback(() => get('/api/samples').then(setSamples).catch(setError), [])

  useEffect(() => {
    get('/api/specs').then(setSpecs).catch(setError)
    refreshGraph()
    refreshMidi()
    refreshSamples()
    const fast = setInterval(() => get('/api/status').then(setStatus).catch(() => {}), 500)
    const slow = setInterval(() => {
      refreshMidi()
      refreshSamples()
      refreshGraph()
    }, 2000)
    return () => { clearInterval(fast); clearInterval(slow) }
  }, [])

  const err = (e) => setError(e)

  return (
    <div className="app">
      <TopBar status={status} midi={midi} refreshMidi={refreshMidi} onError={err} />
      <div className="tabs">
        {TABS.map(([id, label]) => (
          <button key={id} className={tab === id ? 'tab active' : 'tab'}
            onClick={() => setTab(id)}>{label}</button>
        ))}
        {error && (
          <span className="error" onClick={() => setError(null)}>
            {String(error.message || error)} ✕
          </span>
        )}
      </div>
      <div className="panel">
        {tab === 'graph' && (
          <GraphPanel graph={graph} specs={specs} refresh={refreshGraph}
            samples={samples} onError={err} />
        )}
        {tab === 'looper' && (
          <LooperPanel midi={midi} graph={graph} status={status}
            refresh={refreshMidi} onEdit={setEditor} onError={err} />
        )}
        {tab === 'samples' && (
          <SamplesPanel samples={samples} refresh={refreshSamples}
            onEdit={setEditor} onError={err} />
        )}
        {tab === 'config' && (
          <ConfigPanel midi={midi} refresh={refreshMidi} onError={err} />
        )}
      </div>
      {editor?.kind === 'sample' && (
        <SampleEditor name={editor.name} status={status} graph={graph}
          onClose={() => { setEditor(null); refreshSamples() }} onError={err} />
      )}
      {editor?.kind === 'clip' && (
        <MidiEditor input={editor.input} note={editor.note} status={status}
          onClose={() => { setEditor(null); refreshMidi() }} onError={err} />
      )}
    </div>
  )
}
