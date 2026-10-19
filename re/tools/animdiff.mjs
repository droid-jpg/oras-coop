import { readFileSync } from 'node:fs';
const d = process.argv[2];
const idle = [0,1,2].map(i => readFileSync(`${d}/idle${i}.bin`));
const walk = [0,1,2].map(i => readFileSync(`${d}/walk${i}.bin`));
const w = (b, o) => b.readUInt32LE(o);
const fmt = (b, o) => `${w(b,o).toString(16).padStart(8,'0')}(${b.readFloatLE(o).toFixed(2)})`;
for (let o = 0; o + 4 <= idle[0].length; o += 4) {
  const iv = idle.map(b => w(b,o)), wv = walk.map(b => w(b,o));
  const idleStable = iv.every(v => v === iv[0]);
  const walkDiff = wv.every(v => v !== iv[0]);
  if (idleStable && walkDiff && (o < 0xE8 || o > 0x120)) // skip position/rotation fields we already know
    console.log(`+0x${o.toString(16).padStart(3,'0')}  idle=${fmt(idle[0],o)}  walk=${walk.map(b=>fmt(b,o)).join(' ')}`);
}
