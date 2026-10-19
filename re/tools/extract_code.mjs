// Extract (and BLZ-decompress) .code from the first NCCH of a decrypted CIA or CCI.
// Usage: node extract_code.mjs <input.cia|.cci> <outDir>
import { openSync, readSync, writeFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const rd = (fd, off, len) => { const b = Buffer.alloc(len); readSync(fd, b, 0, len, off); return b; };
const align = (n, a) => Math.ceil(n / a) * a;

const [input, outDir] = process.argv.slice(2);
const fd = openSync(input, 'r');
const head = rd(fd, 0, 0x200);

let base;
if (head.toString('ascii', 0x100, 0x104) === 'NCSD') {
  base = head.readUInt32LE(0x120) * 0x200;
} else {
  const hdrSize = head.readUInt32LE(0), certSize = head.readUInt32LE(8), tikSize = head.readUInt32LE(12), tmdSize = head.readUInt32LE(16);
  base = align(hdrSize, 64) + align(certSize, 64) + align(tikSize, 64) + align(tmdSize, 64);
}

const ncch = rd(fd, base, 0x200);
if (ncch.toString('ascii', 0x100, 0x104) !== 'NCCH') throw new Error('NCCH not found');
if (!(ncch[0x18F] & 0x04)) throw new Error('NCCH is encrypted');

// Extended header: code set info and compression flag.
const exh = rd(fd, base + 0x200, 0x400);
const appTitle = exh.toString('ascii', 0, 8).replace(/\0+$/, '');
const compressed = (exh[0xD] & 1) !== 0;
const seg = (o) => ({ addr: exh.readUInt32LE(o), pages: exh.readUInt32LE(o + 4), size: exh.readUInt32LE(o + 8) });
const text = seg(0x10), ro = seg(0x20), data = seg(0x30);
const bss = exh.readUInt32LE(0x3C);

const exefsOff = base + ncch.readUInt32LE(0x1A0) * 0x200;
const exefsHdr = rd(fd, exefsOff, 0x200);
let code;
for (let i = 0; i < 10; i++) {
  if (exefsHdr.toString('ascii', i * 16, i * 16 + 8).replace(/\0+$/, '') === '.code') {
    code = rd(fd, exefsOff + 0x200 + exefsHdr.readUInt32LE(i * 16 + 8), exefsHdr.readUInt32LE(i * 16 + 12));
  }
}
if (!code) throw new Error('.code not found');

// Nintendo bottom-up LZ77 ("BLZ") decompression.
function blzDecompress(src) {
  const footer = src.readUInt32LE(src.length - 8);
  const extra = src.readUInt32LE(src.length - 4);
  const hdrLen = footer >>> 24;
  const compLen = footer & 0xFFFFFF;
  const out = Buffer.alloc(src.length + extra);
  src.copy(out, 0, 0, src.length);
  let inPos = src.length - hdrLen;
  let outPos = out.length;
  const stop = src.length - compLen;
  while (inPos > stop) {
    const flags = out[--inPos];
    for (let bit = 0; bit < 8 && inPos > stop; bit++) {
      if (flags & (0x80 >> bit)) {
        const hi = out[--inPos], lo = out[--inPos];
        const pair = (hi << 8) | lo;
        const len = (pair >>> 12) + 3;
        const disp = (pair & 0xFFF) + 3;
        for (let j = 0; j < len; j++) { outPos--; out[outPos] = out[outPos + disp]; }
      } else {
        out[--outPos] = out[--inPos];
      }
    }
  }
  return out;
}

const plain = compressed ? blzDecompress(code) : code;
mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, 'code.bin'), plain);
writeFileSync(join(outDir, 'exheader.bin'), exh);
const info = {
  source: input,
  titleId: ncch.readBigUInt64LE(0x108).toString(16).padStart(16, '0'),
  appTitle, compressed, size: plain.length,
  text: { ...text, addrHex: '0x' + text.addr.toString(16) },
  rodata: { ...ro, addrHex: '0x' + ro.addr.toString(16) },
  data: { ...data, addrHex: '0x' + data.addr.toString(16) },
  bssSize: bss,
};
writeFileSync(join(outDir, 'layout.json'), JSON.stringify(info, null, 2));
console.log(JSON.stringify(info, null, 2));
