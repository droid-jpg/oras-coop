import { readFileSync } from 'node:fs';
const b = readFileSync(process.argv[2]); const base = 0x00630000;
// mapped ranges
let ranges = [], cur = null;
for (let p = 0; p < b.length; p += 0x1000) {
  let nz = false; for (let i = p; i < p + 0x1000; i += 4) if (b.readUInt32LE(i)) { nz = true; break; }
  if (nz) { if (cur && cur[1] === p) cur[1] = p + 0x1000; else { cur = [p, p + 0x1000]; ranges.push(cur); } }
}
console.log('non-zero ranges:'); for (const [s, e] of ranges) console.log(`  0x${(base+s).toString(16)} - 0x${(base+e).toString(16)} (${((e-s)/1024)|0} KB)`);
const txt = b.toString('latin1');
const re = /[A-Za-z0-9_\/.]{3,}\.(cro|crs|crr|so)\b|Dll[A-Za-z0-9_]{2,}|CRO0|CRR0/g;
const seen = new Map(); let m;
while ((m = re.exec(txt))) if (!seen.has(m[0])) seen.set(m[0], base + m.index);
console.log('module-ish strings:'); for (const [s, a] of [...seen].slice(0, 60)) console.log(`  0x${a.toString(16)}  ${s}`);
const v = 0x7FAC30 - base; console.log('around vtable 0x7FAC30:'); for (let i = v; i < v + 0x20; i += 4) console.log(`  0x${(base+i).toString(16)}: ${b.readUInt32LE(i).toString(16).padStart(8,'0')}`);
