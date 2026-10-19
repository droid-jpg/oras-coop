// Samples move-model fields over time (one GDB session; brief halts per sample).
// Usage: node sample_mm.mjs <mmAddr> <samples> <intervalMs>
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const mem = join(dirname(fileURLToPath(import.meta.url)), 'mem.mjs');
const [mmArg, nArg = '12', msArg = '300'] = process.argv.slice(2);
const mm = Number.parseInt(mmArg, 16);
const hex = (n) => '0x' + n.toString(16);

for (let i = 0; i < Number(nArg); i++) {
  const r = spawnSync(process.execPath, [mem, 'read', hex(mm), '0x130'], { encoding: 'utf8' });
  const bytes = Buffer.from(r.stdout.split('\n').filter(Boolean).map((l) => l.slice(10).replace(/ /g, '')).join(''), 'hex');
  const f = (o) => bytes.readFloatLE(o).toFixed(3);
  const u = (o) => bytes.readUInt32LE(o).toString(16).padStart(8, '0');
  console.log(`t${i}: flags4=${u(4)} flags8=${u(8)} pos=(${f(0xEC)},${f(0xF0)},${f(0xF4)}) v114=(${f(0x114)},${f(0x118)},${f(0x11C)}) w120=${u(0x120)} ${u(0x124)} ${u(0x128)}`);
  await new Promise((res) => setTimeout(res, Number(msArg)));
}
