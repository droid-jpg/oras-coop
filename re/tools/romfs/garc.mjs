// GARC reader: node garc.mjs <file.garc> [index outFile]  (no index: list files)
import { readFileSync, writeFileSync } from 'node:fs';
export function parseGarc(b) {
  if (b.toString('latin1', 0, 4) !== 'CRAG') return null;
  const dataOff = b.readUInt32LE(0x10);
  let p = b.readUInt32LE(4);
  if (b.toString('latin1', p, p + 4) !== 'OTAF') return null;
  const count = b.readUInt16LE(p + 8);
  p += b.readUInt32LE(p + 4);                     // FATB
  const fatb = p + 12;
  const out = [];
  let q = fatb;
  for (let i = 0; i < count; i++) {
    const bits = b.readUInt32LE(q); q += 4;
    const subs = [];
    for (let k = 0; k < 32; k++) if (bits & (1 << k)) { subs.push({ start: b.readUInt32LE(q), end: b.readUInt32LE(q + 4), len: b.readUInt32LE(q + 8) }); q += 12; }
    const s = subs[0];
    out.push(s ? b.subarray(dataOff + s.start, dataOff + s.start + s.len) : Buffer.alloc(0));
  }
  return out;
}
if (process.argv[1]?.endsWith('garc.mjs')) {
  const files = parseGarc(readFileSync(process.argv[2]));
  if (process.argv[3] === undefined) files.forEach((f, i) => console.log(i, f.length, f.subarray(0, 8).toString('hex')));
  else { writeFileSync(process.argv[4], files[Number(process.argv[3])]); console.log('ok'); }
}
