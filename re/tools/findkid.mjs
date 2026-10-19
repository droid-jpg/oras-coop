import { readFileSync } from 'node:fs';
const b = readFileSync(process.argv[2]); const base = 0x08000000;
const X = 1791, Y = 2925, TX = 99, TY = 162;
const fx = [], fy = [];
for (let i = 0; i + 4 <= b.length; i += 4) {
  const v = b.readFloatLE(i);
  if (v === X) fx.push(i); else if (v === Y) fy.push(i);
}
console.log(`float ${X}: ${fx.length} hits, float ${Y}: ${fy.length} hits`);
console.log('float X with float Y within 0x20 bytes after (X,?,Y or X,Y layouts):');
const ys = new Set(fy);
for (const i of fx) for (let d = 4; d <= 0x20; d += 4) if (ys.has(i + d)) console.log(`  ${(base+i).toString(16)}  Y at +0x${d.toString(16)}`);
console.log('u16 tile pairs (99,162) and u32 pairs:');
for (let i = 0; i + 8 <= b.length; i += 2) {
  if (b.readUInt16LE(i) === TX && b.readUInt16LE(i + 2) === TY) console.log(`  u16 pair @ ${(base+i).toString(16)}`);
  if (i % 4 === 0 && b.readUInt32LE(i) === TX && b.readUInt32LE(i + 4) === TY) console.log(`  u32 pair @ ${(base+i).toString(16)}`);
}
