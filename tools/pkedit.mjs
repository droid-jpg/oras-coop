// Edits one Pokemon from a party file (dumpparty.mjs: 0x104 bytes each, encrypted) for tests, e.g.
// a Haunter for fakefriend.mjs --trader --offer. Fixes the checksum and keeps the encryption.
// Usage: node pkedit.mjs <in.bin> <out.bin> [--index 0] [--species 93] [--item 233]
import { readFileSync, writeFileSync } from 'node:fs';

const PK = 0x104, DATA = 0xE8;
const ORDER = ['ABCD', 'ABDC', 'ACBD', 'ACDB', 'ADBC', 'ADCB', 'BACD', 'BADC', 'BCAD', 'BCDA', 'BDAC', 'BDCA',
  'CABD', 'CADB', 'CBAD', 'CBDA', 'CDAB', 'CDBA', 'DABC', 'DACB', 'DBAC', 'DBCA', 'DCAB', 'DCBA'];

const [inFile, outFile, ...rest] = process.argv.slice(2);
const args = {};
for (let i = 0; i < rest.length; i += 2) args[rest[i].replace(/^--/, '')] = Number(rest[i + 1]);
if (!inFile || !outFile) { console.log('usage: node pkedit.mjs <in.bin> <out.bin> [--index n] [--species n] [--item n]'); process.exit(1); }

function crypt(b, seed, start, end) {
  for (let i = start; i + 1 < end; i += 2) {
    seed = (Math.imul(seed, 0x41C64E6D) + 0x6073) >>> 0;
    b.writeUInt16LE(b.readUInt16LE(i) ^ (seed >>> 16), i);
  }
}

const all = readFileSync(inFile);
const i = args.index ?? 0;
const pk = Buffer.from(all.subarray(i * PK, (i + 1) * PK));
const ec = pk.readUInt32LE(0);
crypt(pk, ec, 8, DATA);
const a = 8 + ORDER[((ec >>> 13) & 31) % 24].indexOf('A') * 56; // block A: species u16 @0, item u16 @2
console.log(`was species ${pk.readUInt16LE(a)} item ${pk.readUInt16LE(a + 2)}`);
if (args.species !== undefined) pk.writeUInt16LE(args.species, a);
if (args.item !== undefined) pk.writeUInt16LE(args.item, a + 2);
let sum = 0;
for (let o = 8; o < DATA; o += 2) sum = (sum + pk.readUInt16LE(o)) & 0xFFFF;
pk.writeUInt16LE(sum, 6);
console.log(`now species ${pk.readUInt16LE(a)} item ${pk.readUInt16LE(a + 2)}`);
crypt(pk, ec, 8, DATA);
writeFileSync(outFile, pk);
