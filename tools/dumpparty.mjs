// Saves the running game's party (encrypted data + party stats per Pokemon, 0x104 bytes each)
// through Azahar's GDB stub, e.g. for fakefriend.mjs --party. Usage: node dumpparty.mjs <out.bin>
// Env: GDB_PORT (see ORAS-RE/tools/mem.mjs).
import { execFileSync } from 'node:child_process';
import { writeFileSync } from 'node:fs';
const MEM = 'E:/ORAS-RE/tools/mem.mjs';
function read(addr, len) {
  const out = execFileSync('node', [MEM, 'read', '0x' + addr.toString(16), '0x' + len.toString(16)], { encoding: 'latin1' });
  const buf = Buffer.alloc(len);
  for (const line of out.split('\n')) {
    const m = line.match(/^([0-9a-f]{8})\s+(.*)$/); if (!m) continue;
    const off = parseInt(m[1], 16) - addr;
    m[2].trim().split(/\s+/).forEach((h, i) => { if (off + i >= 0 && off + i < len) buf[off + i] = parseInt(h, 16); });
  }
  return buf;
}
const head = read(0x08CFB1E0, 0x1C), count = head[0x18];
const out = [];
for (let i = 0; i < count; i++) {
  const obj = head.readUInt32LE(i * 4), ptrs = read(obj + 4, 8);
  out.push(read(ptrs.readUInt32LE(0), 0xE8), read(ptrs.readUInt32LE(4), 0x1C));
}
writeFileSync(process.argv[2], Buffer.concat(out));
console.log(`${count} Pokemon -> ${process.argv[2]}`);
