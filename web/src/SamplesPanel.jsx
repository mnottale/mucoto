import React from 'react'
import { post } from './api.js'

export default function SamplesPanel({ samples, refresh, onEdit, onError }) {
  return (
    <div className="samples">
      <h3>Sample bank</h3>
      <table>
        <tbody>
          {samples.map((s) => (
            <tr key={s.name}>
              <td>{s.name}</td>
              <td>{s.frames} frames</td>
              <td>
                <button onClick={() => onEdit({ kind: 'sample', name: s.name })}>edit</button>
                <button onClick={() =>
                  post(`/api/sample/${s.name}/delete`).then(refresh).catch(onError)}>
                  delete
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      {!samples.length && <p className="hint">No samples yet. Record with a looper pad or a recorder node.</p>}
    </div>
  )
}
