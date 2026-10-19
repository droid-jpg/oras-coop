// Samples a character's action/animation fields: node sample_anim.mjs <mmHex> <samples> <gapMs>
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const mem = join(dirname(fileURLToPath(import.meta.url)), 'mem.mjs');
const [mmArg, n = '8', gap = '120'] = process.argv.slice(2);
const mm = Number.parseInt(mmArg, 16);
const rd = (off, len) => {
  const r = spawnSync(process.execPath, [mem, 'read', '0x' + (mm + off).toString(16), '0x' + len.toString(16)], { encoding: 'utf8' });
  return Buffer.from(r.stdout.split('\n').filter(Boolean).map((l) => l.slice(10).replace(/ /g, '')).join(''), 'hex');
};
for (let i = 0; i < Number(n); i++) {
  const a = rd(0x820, 0x10), b = rd(0x900, 0x10), f = rd(0x08, 4), p = rd(0xEC, 12);
  console.log(`t${i}: flags8=${f.readUInt32LE(0).toString(16)} +824=${a.readUInt32LE(4).toString(16)} ` +
    `action=${b.readUInt16LE(4).toString(16)} +908=${b.readUInt32LE(8).toString(16)} +90c=${b.readUInt32LE(12).toString(16)} ` +
    `x=${p.readFloatLE(0).toFixed(1)} y=${p.readFloatLE(8).toFixed(1)}`);
  await new Promise((r) => setTimeout(r, Number(gap)));
}
