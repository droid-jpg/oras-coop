import { readFileSync } from 'node:fs';
const b = readFileSync(process.argv[2]); const base = 0x5EC000;
const obj = 0x08C6E880;
for (let i = 0; i + 4 <= b.length; i += 4) {
  const v = b.readUInt32LE(i);
  if (v >= obj - 0x1000 && v <= obj + 0x40) console.log(`0x${(base+i).toString(16)} -> 0x${v.toString(16)} (obj${v>=obj?'+':'-'}0x${Math.abs(v-obj).toString(16)})`);
}
