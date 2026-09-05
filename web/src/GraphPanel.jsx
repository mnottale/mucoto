import React, { useRef, useState } from 'react'
import { post } from './api.js'

const NODE_W = 130

function nodePos(node, i, local) {
  if (local[node.name]) return local[node.name]
  const x = node.params?._x
  const y = node.params?._y
  if (x !== undefined || y !== undefined) return { x: x || 0, y: y || 0 }
  return { x: 40 + (i % 4) * 190, y: 40 + Math.floor(i / 4) * 130 }
}

function inputCount(node, spec, links) {
  if (!spec) return 1
  if (spec.inputs === -1)
    return links.filter((l) => l.to === node.name).length + 1
  return spec.inputs
}

export default function GraphPanel({ graph, specs, refresh, samples, onError }) {
  const [sel, setSel] = useState(null) // node name
  const [selLink, setSelLink] = useState(null)
  const [pending, setPending] = useState(null) // {from, fromChannel}
  const [local, setLocal] = useState({}) // node -> {x,y} while dragging
  const [pvals, setPvals] = useState({}) // "node|param" -> value while sliding
  const [newType, setNewType] = useState('gain')
  const dragRef = useRef(null)
  const svgRef = useRef(null)
  const pvalTimer = useRef({})

  const nodes = graph.nodes || []
  const links = graph.links || []
  const selNode = nodes.find((n) => n.name === sel)
  const selSpec = selNode && specs[selNode.type]

  const mouse = (e) => {
    const r = svgRef.current.getBoundingClientRect()
    return { x: e.clientX - r.left, y: e.clientY - r.top }
  }

  const addNode = () => {
    let i = 1
    while (nodes.find((n) => n.name === `${newType}_${i}`)) i++
    const name = `${newType}_${i}`
    post('/api/graph/node', { type: newType, name })
      .then(() => post(`/api/node/${name}/param`, { name: '_x', value: 60 + Math.random() * 300 }))
      .then(() => post(`/api/node/${name}/param`, { name: '_y', value: 60 + Math.random() * 200 }))
      .then(refresh)
      .catch(onError)
  }
  const deleteNode = (name) =>
    post(`/api/graph/node/${name}/delete`).then(() => { setSel(null); refresh() }).catch(onError)
  const deleteLink = (l) =>
    post('/api/graph/link/delete', l).then(() => { setSelLink(null); refresh() }).catch(onError)

  const startDrag = (e, node, i) => {
    e.stopPropagation()
    setSel(node.name)
    setSelLink(null)
    const p = nodePos(node, i, local)
    dragRef.current = { name: node.name, dx: mouse(e).x - p.x, dy: mouse(e).y - p.y }
  }
  const onMove = (e) => {
    if (!dragRef.current) return
    const m = mouse(e)
    const { name, dx, dy } = dragRef.current
    setLocal((l) => ({ ...l, [name]: { x: m.x - dx, y: m.y - dy } }))
  }
  const endDrag = () => {
    const d = dragRef.current
    dragRef.current = null
    if (!d || !local[d.name]) return
    const p = local[d.name]
    post(`/api/node/${d.name}/param`, { name: '_x', value: p.x })
      .then(() => post(`/api/node/${d.name}/param`, { name: '_y', value: p.y }))
      .then(refresh)
      .catch(onError)
  }

  const clickOut = (node, ch) => {
    setPending({ from: node.name, fromChannel: ch })
  }
  const clickIn = (node, slot) => {
    if (!pending) return
    post('/api/graph/link', { ...pending, to: node.name, toInput: slot })
      .then(() => { setPending(null); refresh() })
      .catch((e) => { setPending(null); onError(e) })
  }

  const setParam = (node, name, value, isString) => {
    const key = `${node}|${name}`
    setPvals((v) => ({ ...v, [key]: value }))
    clearTimeout(pvalTimer.current[key])
    pvalTimer.current[key] = setTimeout(() => {
      setPvals((v) => { const c = { ...v }; delete c[key]; return c })
    }, 1500)
    post(`/api/node/${node}/${isString ? 'sparam' : 'param'}`,
      { name, value: isString ? value : parseFloat(value) }).catch(onError)
  }
  const learnParam = (node, p) =>
    post('/api/learn', { action: 'node_param', node, param: p.name, min: p.min, max: p.max })
      .catch(onError)

  const paramsOf = (node, spec) => {
    const res = (spec?.params || []).map((p) => ({ ...p }))
    if (spec?.dynamicParams) {
      const n = inputCount(node, spec, links)
      for (let i = 0; i < n; i++)
        res.push({ ...spec.dynamicParams, name: spec.dynamicParams.prefix + i })
    }
    return res
  }
  const valueOf = (node, p) =>
    pvals[`${node.name}|${p.name}`] ?? node.params?.[p.name] ?? p.default

  // geometry
  const geom = {}
  nodes.forEach((n, i) => {
    const spec = specs[n.type]
    const nin = inputCount(n, spec, links)
    const nout = n.outputChannels
    geom[n.name] = {
      ...nodePos(n, i, local),
      nin, nout,
      h: 26 + 16 * Math.max(nin, nout, 1),
    }
  })
  const portY = (g, i) => g.y + 26 + 16 * i + 6

  return (
    <div className="grapheditor">
      <div className="graphcanvas" onMouseMove={onMove} onMouseUp={endDrag}>
        <div className="toolbar">
          <select value={newType} onChange={(e) => setNewType(e.target.value)}>
            {Object.keys(specs).map((t) => <option key={t}>{t}</option>)}
          </select>
          <button onClick={addNode}>+ node</button>
          {pending && <span className="hint">click an input port… (esc)</span>}
          {selLink && (
            <button onClick={() => deleteLink(selLink)}>
              delete link {selLink.from}→{selLink.to}
            </button>
          )}
        </div>
        <svg ref={svgRef} className="graphsvg" tabIndex={0}
          onClick={() => { setPending(null); setSelLink(null); setSel(null) }}
          onKeyDown={(e) => e.key === 'Escape' && setPending(null)}>
          {links.map((l, i) => {
            const a = geom[l.from]; const b = geom[l.to]
            if (!a || !b) return null
            const x1 = a.x + NODE_W; const y1 = portY(a, l.fromChannel)
            const x2 = b.x; const y2 = portY(b, l.toInput)
            const isSel = selLink && JSON.stringify(selLink) === JSON.stringify(l)
            return (
              <path key={i} className={isSel ? 'link sel' : 'link'}
                d={`M ${x1} ${y1} C ${x1 + 50} ${y1} ${x2 - 50} ${y2} ${x2} ${y2}`}
                onClick={(e) => { e.stopPropagation(); setSelLink(l); setSel(null) }} />
            )
          })}
          {nodes.map((n, i) => {
            const g = geom[n.name]
            const spec = specs[n.type]
            return (
              <g key={n.name} transform={`translate(${g.x} ${g.y})`}>
                <rect className={sel === n.name ? 'node sel' : 'node'}
                  width={NODE_W} height={g.h} rx={6}
                  onMouseDown={(e) => startDrag(e, n, i)}
                  onClick={(e) => e.stopPropagation()} />
                <text className="nodetitle" x={NODE_W / 2} y={16}>{n.name}</text>
                {Array.from({ length: g.nin }).map((_, s) => (
                  <circle key={'i' + s} className="port in" cx={0} cy={26 + 16 * s + 6} r={6}
                    onClick={(e) => { e.stopPropagation(); clickIn(n, s) }} />
                ))}
                {Array.from({ length: g.nout }).map((_, s) => (
                  <circle key={'o' + s}
                    className={pending?.from === n.name && pending?.fromChannel === s
                      ? 'port out sel' : 'port out'}
                    cx={NODE_W} cy={26 + 16 * s + 6} r={6}
                    onClick={(e) => { e.stopPropagation(); clickOut(n, s) }} />
                ))}
              </g>
            )
          })}
        </svg>
      </div>
      <div className="sidepanel">
        {selNode && (
          <>
            <h3>{selNode.name} <small>({selNode.type})</small></h3>
            <p className="hint">{selSpec?.description}</p>
            {paramsOf(selNode, selSpec).map((p) => (
              <div key={p.name} className="paramrow" title={p.description}>
                <label>{p.name}</label>
                <input type="range" min={p.min} max={p.max}
                  step={(p.max - p.min) / 200 || 1}
                  value={valueOf(selNode, p)}
                  onChange={(e) => setParam(selNode.name, p.name, e.target.value)} />
                <input type="number" className="pnum" value={valueOf(selNode, p)}
                  onChange={(e) => setParam(selNode.name, p.name, e.target.value)} />
                <button className="learn" title="MIDI learn"
                  onClick={() => learnParam(selNode.name, p)}>L</button>
              </div>
            ))}
            {(selSpec?.sparams || []).map((p) => (
              // key includes the node so switching selection remounts the
              // uncontrolled input (defaultValue would keep the old node's text)
              <div key={selNode.name + ':' + p.name} className="paramrow" title={p.description}>
                <label>{p.name}</label>
                {p.values ? (
                  <select value={selNode.sparams?.[p.name] ?? p.default}
                    onChange={(e) => setParam(selNode.name, p.name, e.target.value, true)}>
                    {p.values.map((v) => <option key={v}>{v}</option>)}
                  </select>
                ) : (
                  <input defaultValue={selNode.sparams?.[p.name] ?? p.default}
                    onBlur={(e) => setParam(selNode.name, p.name, e.target.value, true)} />
                )}
              </div>
            ))}
            {selNode.type === 'player' && (
              <PlayerControls node={selNode.name} samples={samples} onError={onError} />
            )}
            <button className="danger" onClick={() => deleteNode(selNode.name)}>
              delete node
            </button>
          </>
        )}
        {!selNode && <p className="hint">Select a node to edit its parameters.<br />
          Click an output port then an input port to create a link.</p>}
      </div>
    </div>
  )
}

function PlayerControls({ node, samples, onError }) {
  const [sample, setSample] = useState('')
  const [loop, setLoop] = useState(false)
  return (
    <div className="playerctl">
      <select value={sample} onChange={(e) => setSample(e.target.value)}>
        <option value="">sample…</option>
        {samples.map((s) => <option key={s.name}>{s.name}</option>)}
      </select>
      <label><input type="checkbox" checked={loop} onChange={(e) => setLoop(e.target.checked)} />loop</label>
      <button disabled={!sample}
        onClick={() => post(`/api/player/${node}/play`, { sample, loop: loop ? 1 : 0 }).catch(onError)}>
        ▶
      </button>
      <button onClick={() => post(`/api/player/${node}/stop`).catch(onError)}>■</button>
    </div>
  )
}
