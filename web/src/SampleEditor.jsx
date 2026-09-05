import React, { useEffect, useRef, useState } from 'react'
import { getBin, post } from './api.js'

const W = 900
const H = 220

export default function SampleEditor({ name, status, graph, onClose, onError }) {
  const [data, setData] = useState(null)
  const [selA, setSelA] = useState(null) // frames
  const [selB, setSelB] = useState(null)
  const [snap, setSnap] = useState(true)
  const [player, setPlayer] = useState('')
  const canvasRef = useRef(null)
  const dragRef = useRef(false)

  const rate = status?.rate || 48000
  const bpm = status?.bpm || 120
  const measureFrames = (rate * 240) / bpm
  const players = (graph.nodes || []).filter((n) => n.type === 'player')

  const load = () => getBin(`/api/sample/${name}`).then((d) => {
    setData(d)
    setSelA(null)
    setSelB(null)
  }).catch(onError)
  useEffect(() => { load() }, [name])

  useEffect(() => {
    const cv = canvasRef.current
    if (!cv || !data) return
    const ctx = cv.getContext('2d')
    ctx.fillStyle = '#181818'
    ctx.fillRect(0, 0, W, H)
    // measure grid
    ctx.strokeStyle = '#333'
    for (let f = 0; f < data.length; f += measureFrames) {
      const x = (f / data.length) * W
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke()
    }
    // waveform
    ctx.strokeStyle = '#4c4'
    ctx.beginPath()
    const spp = data.length / W
    for (let x = 0; x < W; x++) {
      let mn = 1, mx = -1
      const s = Math.floor(x * spp)
      const e = Math.min(data.length, Math.ceil((x + 1) * spp))
      for (let i = s; i < e; i++) {
        if (data[i] < mn) mn = data[i]
        if (data[i] > mx) mx = data[i]
      }
      if (mn > mx) continue
      ctx.moveTo(x + 0.5, H / 2 - (mx * H) / 2)
      ctx.lineTo(x + 0.5, H / 2 - (mn * H) / 2 + 1)
    }
    ctx.stroke()
    // selection
    if (selA !== null && selB !== null) {
      const a = (Math.min(selA, selB) / data.length) * W
      const b = (Math.max(selA, selB) / data.length) * W
      ctx.fillStyle = 'rgba(120,160,255,0.25)'
      ctx.fillRect(a, 0, b - a, H)
    }
  }, [data, selA, selB, measureFrames])

  const frameAt = (e) => {
    const r = canvasRef.current.getBoundingClientRect()
    const x = Math.max(0, Math.min(W, e.clientX - r.left))
    return Math.floor((x / W) * (data?.length || 0))
  }
  const snapEnd = (a, b) => {
    if (!snap) return b
    const len = Math.abs(b - a)
    const m = Math.max(1, Math.round(len / measureFrames))
    return a + Math.sign(b - a || 1) * Math.min(m * measureFrames, data.length)
  }
  const down = (e) => { dragRef.current = true; setSelA(frameAt(e)); setSelB(null) }
  const move = (e) => {
    if (!dragRef.current || selA === null) return
    setSelB(snapEnd(selA, frameAt(e)))
  }
  const up = () => { dragRef.current = false }

  const selection = selA !== null && selB !== null
    ? [Math.round(Math.min(selA, selB)), Math.round(Math.max(selA, selB))]
    : null

  const cut = () => {
    if (!selection) return
    post(`/api/sample/${name}/cut`, { start: selection[0], end: selection[1] })
      .then(load).catch(onError)
  }
  const play = (loop) => {
    const p = player || players[0]?.name
    if (!p) return onError(new Error('no player node in the graph'))
    post(`/api/player/${p}/play`, { sample: name, loop: loop ? 1 : 0 }).catch(onError)
  }
  const stop = () => {
    const p = player || players[0]?.name
    if (p) post(`/api/player/${p}/stop`).catch(onError)
  }

  const dur = data ? (data.length / rate).toFixed(2) : '…'
  const selMeasures = selection
    ? ((selection[1] - selection[0]) / measureFrames).toFixed(2)
    : null

  return (
    <div className="modal" onClick={onClose}>
      <div className="editor" onClick={(e) => e.stopPropagation()}>
        <div className="editorbar">
          <h3>Sample: {name} <small>{dur}s</small></h3>
          <label>
            <input type="checkbox" checked={snap} onChange={(e) => setSnap(e.target.checked)} />
            snap to measures
          </label>
          {selMeasures && <span className="hint">{selMeasures} measures selected</span>}
          <select value={player} onChange={(e) => setPlayer(e.target.value)}>
            <option value="">player…</option>
            {players.map((p) => <option key={p.name}>{p.name}</option>)}
          </select>
          <button onClick={() => play(false)}>▶</button>
          <button onClick={() => play(true)}>⟳</button>
          <button onClick={stop}>■</button>
          <button disabled={!selection} onClick={cut}>cut to selection</button>
          <button onClick={onClose}>close</button>
        </div>
        <canvas ref={canvasRef} width={W} height={H}
          onMouseDown={down} onMouseMove={move} onMouseUp={up} onMouseLeave={up} />
      </div>
    </div>
  )
}
