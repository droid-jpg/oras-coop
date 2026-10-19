import { readFileSync } from 'node:fs';
const b = readFileSync(process.argv[2]); const base = 0x00630000;
const s = 0x6F3000 - base;
for (let i = s; i < s + 0x140; i += 16) console.log((base+i).toString(16).padStart(8,'0') + '  ' + b.subarray(i, i+16).toString('hex').match(/../g).join(' ') + '  ' + b.subarray(i, i+16).toString('latin1').replace(/[^\x20-\x7e]/g,'.'));
