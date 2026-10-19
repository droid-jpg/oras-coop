// Read-only header inspection for a CCI (NCSD) and a CIA.
import { openSync, readSync } from 'node:fs';

const rd = (fd, off, len) => { const b = Buffer.alloc(len); readSync(fd, b, 0, len, off); return b; };

function ncch(fd, base, label) {
  const h = rd(fd, base, 0x200);
  const magic = h.toString('ascii', 0x100, 0x104);
  if (magic !== 'NCCH') { console.log(`${label}: no NCCH at 0x${base.toString(16)} (got ${JSON.stringify(magic)})`); return; }
  const tid = h.readBigUInt64LE(0x108).toString(16).padStart(16, '0');
  const pcode = h.toString('ascii', 0x150, 0x160).replace(/\0+$/, '');
  const flags = h.subarray(0x188, 0x190);
  const noCrypto = (flags[7] & 0x04) !== 0;
  const fixedKey = (flags[7] & 0x01) !== 0;
  const exefsOff = h.readUInt32LE(0x1A0) * 0x200;
  const exefsSize = h.readUInt32LE(0x1A4) * 0x200;
  console.log(`${label}: titleID=${tid} product=${pcode} noCrypto=${noCrypto} fixedKey=${fixedKey} cryptoMethod=0x${flags[3].toString(16)} exefs@+0x${exefsOff.toString(16)} size=0x${exefsSize.toString(16)}`);
  if (noCrypto) {
    const ex = rd(fd, base + exefsOff, 0xA0);
    for (let i = 0; i < 10; i++) {
      const name = ex.toString('ascii', i * 16, i * 16 + 8).replace(/\0+$/, '');
      if (name) console.log(`   exefs file: ${name} off=0x${ex.readUInt32LE(i * 16 + 8).toString(16)} size=0x${ex.readUInt32LE(i * 16 + 12).toString(16)}`);
    }
  }
}

const [cci, cia] = process.argv.slice(2);

{
  const fd = openSync(cci, 'r');
  const h = rd(fd, 0, 0x200);
  console.log(`CCI magic: ${h.toString('ascii', 0x100, 0x104)}`);
  for (let i = 0; i < 8; i++) {
    const off = h.readUInt32LE(0x120 + i * 8) * 0x200, sz = h.readUInt32LE(0x124 + i * 8) * 0x200;
    if (sz) ncch(fd, off, `CCI partition ${i}`);
  }
}

if (cia) {
  const fd = openSync(cia, 'r');
  const h = rd(fd, 0, 0x20);
  const align = (n) => Math.ceil(n / 64) * 64;
  const hdrSize = h.readUInt32LE(0), certSize = h.readUInt32LE(8), tikSize = h.readUInt32LE(12), tmdSize = h.readUInt32LE(16);
  const contentOff = align(hdrSize) + align(certSize) + align(tikSize) + align(tmdSize);
  console.log(`CIA: header=0x${hdrSize.toString(16)} contentOffset=0x${contentOff.toString(16)}`);
  // If the CIA content layer is decrypted, an NCCH magic is visible here; if not, it's title-key encrypted.
  ncch(fd, contentOff, 'CIA content 0');
}
