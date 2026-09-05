async function handle(r) {
  const t = await r.text()
  if (!r.ok) throw new Error(t || `HTTP ${r.status}`)
  if (!t) return null
  try { return JSON.parse(t) } catch { return t }
}

export const get = (p) => fetch(p).then(handle)

export const post = (p, body) =>
  fetch(p, { method: 'POST', body: JSON.stringify(body ?? {}) }).then(handle)

export const getBin = (p) =>
  fetch(p).then(async (r) => {
    if (!r.ok) throw new Error(await r.text())
    return new Float32Array(await r.arrayBuffer())
  })

// Launchpad palette entry -> css color, mirrors the backend palette.
export const PALETTE = [21, 45, 13, 37, 53, 9, 41, 17]
export const COLOR_CSS = {
  0: '#222',
  3: '#eee',
  5: '#e33',
  9: '#a63',
  13: '#dc3',
  17: '#8c4',
  21: '#3c3',
  37: '#3cb',
  41: '#38c',
  45: '#35e',
  53: '#c3c',
}
export const darker = (c) => c // +2 entries are just rendered dimmed in css

export function outputColor(outputs, outputId) {
  const idx = outputs.findIndex((o) => o.id === outputId)
  return PALETTE[(idx < 0 ? 0 : idx) % PALETTE.length]
}
export function hashStr(s) {
  let h = 0
  for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) >>> 0
  return h
}
export function padColor(pad, outputs) {
  if (pad.color) return pad.color // computed backend side, matches the leds
  if (pad.target === 1) return outputColor(outputs, pad.instrument)
  if (pad.target === 2)
    return PALETTE[hashStr(pad.player || pad.recorder) % PALETTE.length]
  return 0
}
