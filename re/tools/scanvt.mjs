import { readFileSync } from 'node:fs';
const b = readFileSync(process.argv[2]); const base = 0x08299000;
// Pointers into code/rodata/data (likely vtables or function pointers) and heap pointers.
const hits = [];
for (let i = 0; i + 4 <= b.length; i += 4) {
  const v = b.readUInt32LE(i);
  if (v >= 0x00100000 && v < 0x00630000) hits.push(`${(base+i).toString(16)}: ${v.toString(16).padStart(8,'0')} ${v < 0x57a000 ? 'text' : v < 0x5ec000 ? 'rodata' : 'data'}`);
}
console.log(hits.join('\n'));
