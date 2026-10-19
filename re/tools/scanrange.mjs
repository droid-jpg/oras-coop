import { readFileSync } from 'node:fs';
const b = readFileSync(process.argv[2]); const base = Number(process.argv[3]);
const lo = Number(process.argv[4]), hi = Number(process.argv[5]);
for (let i = 0; i + 4 <= b.length; i += 4) {
  const v = b.readUInt32LE(i);
  if (v >= lo && v < hi) console.log(`+0x${i.toString(16)} (${(base+i).toString(16)}): ${v.toString(16)}`);
}
