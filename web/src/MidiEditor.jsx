import React, { useEffect, useRef, useState } from 'react'
import { get, post } from './api.js'

const RH = 12 // row height per midi key
const RULER = 24

// split clip events into paired notes + everything else
function parseClip(clip) {
  const notes = []
  const others = []
  const open = {}
  for (const e of clip.events) {
    if (e.type === 0x90 && e.d2 > 0) {
      open[`${e.channel}:${e.d1}`] = { t: e.t, key: e.d1, channel: e.channel, vel: e.d2 }
    } else if (e.type === 0x80 || (e.type === 0x90 && e.d2 === 0)) {
      const k = `${e.channel}:${e.d1}`
      if (open[k]) {
        notes.push({ ...open[k], dur: Math.max(0.02, e.t - open[k].t) })
        delete open[k]
      }
    } else {
      others.push(e)
    }
  }
  for (const k in open) notes.push({ ...open[k], dur: 0.1 })
  return { notes, others }
}

function buildClip(notes, others, length) {
  const events = []
  for (const n of notes) {
    events.push({ t: n.t, type: 0x90, channel: n.channel, d1: n.key, d2: n.vel })
    events.push({ t: Math.min(length, n.t + n.dur), type: 0x80, channel: n.channel, d1: n.key, d2: 0 })
  }
  for (const e of others) events.push(e)
  events.sort((a, b) => a.t - b.t)
  return { events, length }
}

export default function MidiEditor({ input, note, status, onClose, onError }) {
  const [notes, setNotes] = useState([])
  const [others, setOthers] = useState([])
  const [length, setLength] = useState(1)
  const [sel, setSel] = useState(null) // index into notes
  const [range, setRange] = useState(null) // [t0,t1] timeline cut selection
  const [snapN, setSnapN] = useState(4)
  const [zoom, setZoom] = useState(120) // px per second
  const [dirty, setDirty] = useState(false)
  const dragRef = useRef(null)
  const svgRef = useRef(null)

  const bpm = status?.bpm || 120
  const measureSec = 240 / bpm

  useEffect(() => {
    get(`/api/clip/${input}/${note}`).then((clip) => {
      const { notes, others } = parseClip(clip)
      setNotes(notes)
      setOthers(others)
      setLength(clip.length || 1)
    }).catch(onError)
  }, [input, note])

  const keys = notes.length ? notes.map((n) => n.key) : [60]
  const kmax = Math.min(127, Math.max(...keys) + 3)
  const kmin = Math.max(0, Math.min(...keys) - 3)
  const H = RULER + (kmax - kmin + 1) * RH
  const W = Math.max(600, length * zoom + 60)
  const x2t = (x) => x / zoom
  const t2x = (t) => t * zoom
  const y2k = (y) => kmax - Math.floor((y - RULER) / RH)
  const k2y = (k) => RULER + (kmax - k) * RH

  const mouse = (e) => {
    const r = svgRef.current.getBoundingClientRect()
    return { x: e.clientX - r.left + svgRef.current.parentElement.scrollLeft, y: e.clientY - r.top }
  }

  const startNoteDrag = (e, i, resize) => {
    e.stopPropagation()
    setSel(i)
    const m = mouse(e)
    dragRef.current = { kind: resize ? 'resize' : 'move', i, x0: m.x, y0: m.y, orig: { ...notes[i] } }
  }
  const startRulerDrag = (e) => {
    const m = mouse(e)
    dragRef.current = { kind: 'range', t0: x2t(m.x) }
    setRange([x2t(m.x), x2t(m.x)])
  }
  const onMove = (e) => {
    const d = dragRef.current
    if (!d) return
    const m = mouse(e)
    if (d.kind === 'range') {
      setRange([Math.min(d.t0, x2t(m.x)), Math.max(d.t0, x2t(m.x))])
      return
    }
    setDirty(true)
    setNotes((ns) => {
      const c = ns.slice()
      const n = { ...d.orig }
      if (d.kind === 'move') {
        n.t = Math.max(0, d.orig.t + x2t(m.x - d.x0))
        n.key = Math.max(0, Math.min(127, d.orig.key - Math.round((m.y - d.y0) / RH)))
      } else {
        n.dur = Math.max(0.02, d.orig.dur + x2t(m.x - d.x0))
      }
      c[d.i] = n
      return c
    })
  }
  const endDrag = () => { dragRef.current = null }

  const addNote = (e) => {
    const m = mouse(e)
    if (m.y < RULER) return
    const channel = notes[0]?.channel ?? 0
    setNotes((ns) => [...ns, {
      t: x2t(m.x), key: y2k(m.y), dur: measureSec / snapN, vel: 100, channel,
    }])
    setDirty(true)
  }
  const deleteSel = () => {
    if (sel === null) return
    setNotes((ns) => ns.filter((_, i) => i !== sel))
    setSel(null)
    setDirty(true)
  }
  const cutRange = () => {
    if (!range) return
    const [a, b] = range
    const d = b - a
    if (d <= 0) return
    const shift = (t) => (t < a ? t : t < b ? a : t - d)
    setNotes((ns) => ns.filter((n) => n.t < a || n.t >= b).map((n) => ({ ...n, t: shift(n.t) })))
    setOthers((os) => os.filter((e) => e.t < a || e.t >= b).map((e) => ({ ...e, t: shift(e.t) })))
    setLength((l) => Math.max(0.1, l - d))
    setRange(null)
    setDirty(true)
  }
  const snapAll = () => {
    const g = measureSec / snapN
    setNotes((ns) => ns.map((n) => ({
      ...n,
      t: Math.round(n.t / g) * g,
      dur: Math.max(g, Math.round(n.dur / g) * g),
    })))
    setDirty(true)
  }
  const save = () => {
    post(`/api/clip/${input}/${note}/set`, buildClip(notes, others, length))
      .then(() => setDirty(false)).catch(onError)
  }

  return (
    <div className="modal" onClick={onClose}>
      <div className="editor" onClick={(e) => e.stopPropagation()} onKeyDown={(e) => {
        if (e.key === 'Delete' || e.key === 'Backspace') deleteSel()
      }} tabIndex={0}>
        <div className="editorbar">
          <h3>Clip: pad {note} <small>{length.toFixed(2)}s</small></h3>
          <label>1/<input className="pnum" type="number" min={1} max={32} value={snapN}
            onChange={(e) => setSnapN(parseInt(e.target.value) || 4)} /> measure</label>
          <button onClick={snapAll}>snap all notes</button>
          <button disabled={!range || range[1] - range[0] <= 0} onClick={cutRange}>
            cut section
          </button>
          <button disabled={sel === null} onClick={deleteSel}>delete note</button>
          <button onClick={() => setZoom((z) => z * 1.5)}>+</button>
          <button onClick={() => setZoom((z) => z / 1.5)}>−</button>
          <button className={dirty ? 'primary' : ''} onClick={save}>save</button>
          <button onClick={onClose}>close</button>
          <span className="hint">double-click: add note, drag right edge: resize, drag ruler: select section</span>
        </div>
        <div className="pianoroll">
          <svg ref={svgRef} width={W} height={H}
            onMouseMove={onMove} onMouseUp={endDrag} onMouseLeave={endDrag}
            onDoubleClick={addNote} onClick={() => setSel(null)}>
            {/* key rows */}
            {Array.from({ length: kmax - kmin + 1 }).map((_, i) => {
              const k = kmax - i
              const black = [1, 3, 6, 8, 10].includes(k % 12)
              return <rect key={k} x={0} y={RULER + i * RH} width={W} height={RH}
                className={black ? 'row black' : 'row'} />
            })}
            {/* measure grid */}
            {Array.from({ length: Math.ceil(length / measureSec) + 1 }).map((_, i) => (
              <g key={i}>
                <line className="measure" x1={t2x(i * measureSec)} y1={RULER}
                  x2={t2x(i * measureSec)} y2={H} />
                <text className="mlabel" x={t2x(i * measureSec) + 3} y={16}>{i + 1}</text>
              </g>
            ))}
            {Array.from({ length: Math.ceil(length / measureSec) * snapN }).map((_, i) => (
              <line key={'s' + i} className="subgrid"
                x1={t2x((i * measureSec) / snapN)} y1={RULER}
                x2={t2x((i * measureSec) / snapN)} y2={H} />
            ))}
            {/* end of clip */}
            <line className="clipend" x1={t2x(length)} y1={0} x2={t2x(length)} y2={H} />
            {/* ruler */}
            <rect className="ruler" x={0} y={0} width={W} height={RULER}
              onMouseDown={startRulerDrag} onDoubleClick={(e) => e.stopPropagation()} />
            {range && (
              <rect className="rangesel" x={t2x(range[0])} y={0}
                width={t2x(range[1] - range[0])} height={H} />
            )}
            {/* notes */}
            {notes.map((n, i) => (
              <g key={i}>
                <rect className={sel === i ? 'note sel' : 'note'}
                  x={t2x(n.t)} y={k2y(n.key)} width={Math.max(4, t2x(n.dur))} height={RH - 1}
                  onMouseDown={(e) => startNoteDrag(e, i, false)}
                  onClick={(e) => e.stopPropagation()} />
                <rect className="notehandle"
                  x={t2x(n.t) + Math.max(4, t2x(n.dur)) - 5} y={k2y(n.key)}
                  width={5} height={RH - 1}
                  onMouseDown={(e) => startNoteDrag(e, i, true)} />
              </g>
            ))}
          </svg>
        </div>
      </div>
    </div>
  )
}
