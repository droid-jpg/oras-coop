import { readFileSync } from 'node:fs';
const b = readFileSync(process.argv[2]); const base = Number(process.argv[3]);
const want = [[1791, 'kidX'], [2925, 'kidY'], [1809, 'kidX_new']];
const pos = {};
for (const [, n] of want) pos[n] = [];
let nonzero = 0;
for (let i = 0; i + 4 <= b.length; i += 4) {
  const u = b.readUInt32LE(i); if (u) nonzero++;
  const v = b.readFloatLE(i);
  for (const [w, n] of want) if (Math.abs(v - w) < 0.01) pos[n].push(i);
}
console.log(`mapped/non-zero words: ${nonzero}`);
for (const n in pos) console.log(`${n}: ${pos[n].length} hits ${pos[n].slice(0, 12).map(i => (base + i).toString(16)).join(' ')}`);
const ys = new Set(pos.kidY);
for (const key of ['kidX', 'kidX_new']) for (const i of pos[key]) for (let d = -0x30; d <= 0x30; d += 4) if (d && ys.has(i + d)) console.log(`  ${key} @ ${(base + i).toString(16)} with kidY at ${d > 0 ? '+' : '-'}0x${Math.abs(d).toString(16)}`);
