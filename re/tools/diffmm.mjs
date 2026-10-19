// Compare dumps of the same object: node diffmm.mjs a.bin b.bin [c.bin ...]
// Prints 4-byte words that differ, as hex and float.
import { readFileSync } from 'node:fs';

const files = process.argv.slice(2);
const bufs = files.map((f) => readFileSync(f));
const len = Math.min(...bufs.map((b) => b.length));
for (let i = 0; i + 4 <= len; i += 4) {
  const words = bufs.map((b) => b.readUInt32LE(i));
  if (words.every((w) => w === words[0])) continue;
  const cols = bufs.map((b) => `${b.readUInt32LE(i).toString(16).padStart(8, '0')}(${b.readFloatLE(i).toFixed(3)})`);
  console.log(`+0x${i.toString(16).padStart(3, '0')}  ${cols.join('  ')}`);
}
