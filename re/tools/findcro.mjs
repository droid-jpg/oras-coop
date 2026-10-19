import { readFileSync } from 'node:fs';
const b = readFileSync(process.argv[2]); const base = 0x00630000;
// CRO header: "CRO0" at +0x80; name offset @+0x84? (we print nearby strings)
let mapped = 0; for (let i = 0; i < b.length; i += 0x1000) if (b.readUInt32LE(i) || b.readUInt32LE(i + 0x800)) mapped++;
console.log(`pages with data: ${mapped}`);
for (let i = 0; i + 0x100 <= b.length; i += 4) {
  if (b.toString('latin1', i, i + 4) === 'CRO0') {
    const start = i - 0x80;
    const nameOff = b.readUInt32LE(i + 0x04 + 0x80 - 0x80 + 0x4); // placeholder; dump raw header instead
    const hdr = b.subarray(start, start + 0x140);
    // name: CRO header field "name offset" is at 0x84 (u32) relative to module start, size at 0x88
    const no = b.readUInt32LE(start + 0x84), ns = b.readUInt32LE(start + 0x88);
    const codeOff = b.readUInt32LE(start + 0xB0), codeSize = b.readUInt32LE(start + 0xB4);
    let name = '?';
    if (no >= base && no < base + b.length) name = b.toString('latin1', no - base, no - base + Math.min(ns, 40)).replace(/\0.*$/, '');
    console.log(`CRO0 module at 0x${(base + start).toString(16)}  name=${name}  nameOff=0x${no.toString(16)}  code=0x${codeOff.toString(16)}+0x${codeSize.toString(16)}`);
  }
}
