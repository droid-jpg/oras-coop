// Reads files from a decrypted .cci (NCSD) RomFS without extracting everything.
// node romfs.mjs <rom.cci> list [filter]        -> paths and sizes
// node romfs.mjs <rom.cci> get <path> <outFile> -> one file
import { openSync, readSync, writeFileSync } from 'node:fs';

const [romPath, cmd, a1, a2] = process.argv.slice(2);
const fd = openSync(romPath, 'r');
const rd = (off, len) => { const b = Buffer.alloc(len); readSync(fd, b, 0, len, off); return b; };

const ncsd = rd(0, 0x200);
if (ncsd.toString('latin1', 0x100, 0x104) !== 'NCSD') throw new Error('not an NCSD image');
const ncch = ncsd.readUInt32LE(0x120) * 0x200;
const nh = rd(ncch, 0x200);
if (nh.toString('latin1', 0x100, 0x104) !== 'NCCH') throw new Error('no NCCH');
if (!(nh[0x188 + 7] & 4)) throw new Error('NCCH is encrypted (NoCrypto flag not set)');
const romfs = ncch + nh.readUInt32LE(0x1B0) * 0x200;
const ivfc = rd(romfs, 0x60);
if (ivfc.toString('latin1', 0, 4) !== 'IVFC') throw new Error('no IVFC');
const masterHash = ivfc.readUInt32LE(0x08);
const l3BlockLog = ivfc.readUInt32LE(0x4C);
const l3Block = 1 << l3BlockLog;
const l3 = romfs + Math.ceil((0x60 + masterHash) / l3Block) * l3Block;
const h = rd(l3, 0x28);
const dirMeta = rd(l3 + h.readUInt32LE(0x0C), h.readUInt32LE(0x10));
const fileMeta = rd(l3 + h.readUInt32LE(0x1C), h.readUInt32LE(0x20));
const dataOff = l3 + h.readUInt32LE(0x24);
const name = (b, o, len) => b.toString('utf16le', o, o + len);

const files = [];
function walkDir(off, path) {
  // dir: parent, sibling, child, file, hash, nameLen, name
  const child = dirMeta.readUInt32LE(off + 8), file = dirMeta.readUInt32LE(off + 12);
  for (let f = file; f !== 0xFFFFFFFF; f = fileMeta.readUInt32LE(f + 4)) {
    // file: parent, sibling, u64 offset, u64 size, hash, nameLen, name
    const fo = Number(fileMeta.readBigUInt64LE(f + 8)), fs = Number(fileMeta.readBigUInt64LE(f + 16));
    files.push({ path: path + name(fileMeta, f + 0x20, fileMeta.readUInt32LE(f + 0x1C)), off: dataOff + fo, size: fs });
  }
  for (let c = child; c !== 0xFFFFFFFF; c = dirMeta.readUInt32LE(c + 4)) {
    walkDir(c, path + name(dirMeta, c + 0x18, dirMeta.readUInt32LE(c + 0x14)) + '/');
  }
}
walkDir(0, '');

if (cmd === 'list') {
  for (const f of files) if (!a1 || f.path.includes(a1)) console.log(f.path, f.size);
} else if (cmd === 'get') {
  const f = files.find((x) => x.path === a1);
  if (!f) throw new Error('no such file: ' + a1);
  writeFileSync(a2, rd(f.off, f.size));
  console.log(`wrote ${f.size} bytes to ${a2}`);
}
