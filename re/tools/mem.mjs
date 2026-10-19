// Minimal GDB remote-protocol client for Azahar's GDB stub. Read-only except write-float.
// Usage:
//   node mem.mjs watch                     -> print player X/Y/Z, facing, map once
//   node mem.mjs read <addr> <len>         -> hex dump
//   node mem.mjs dump <start> <end> <file> -> raw dump of a range
//   node mem.mjs find-float <start> <end> <value> [tolerance]
//   node mem.mjs find-u32 <start> <end> <value>
//   node mem.mjs find-pos <start> <end> <xmin> <xmax> <ymin> <ymax> [zmin] [zmax]
//   node mem.mjs catch-write <addr> [hits] [timeoutMs]   (pauses game on write, prints PC/LR)
// Env: GDB_PORT (default 24689)
import net from 'node:net';
import { writeFileSync } from 'node:fs';

const PORT = Number(process.env.GDB_PORT ?? 24689);
const CHUNK = 0x400;

function checksum(s) {
  let c = 0;
  for (const ch of Buffer.from(s, 'latin1')) c = (c + ch) & 0xff;
  return c.toString(16).padStart(2, '0');
}

class Gdb {
  constructor(sock) {
    this.sock = sock;
    this.buf = '';
    this.waiters = [];
    sock.setEncoding('latin1');
    sock.on('data', (d) => { this.buf += d; this.pump(); });
  }
  pump() {
    for (;;) {
      this.buf = this.buf.replace(/^\+/, '');
      const start = this.buf.indexOf('$');
      const hash = this.buf.indexOf('#', start);
      if (start < 0 || hash < 0 || this.buf.length < hash + 3) return;
      const body = this.buf.slice(start + 1, hash);
      this.buf = this.buf.slice(hash + 3);
      this.sock.write('+');
      const w = this.waiters.shift();
      if (w) w(body);
    }
  }
  cmd(s) {
    return new Promise((resolve) => {
      this.waiters.push(resolve);
      this.sock.write(`$${s}#${checksum(s)}`);
    });
  }
  async read(addr, len) {
    const out = Buffer.alloc(len);
    for (let off = 0; off < len; off += CHUNK) {
      const n = Math.min(CHUNK, len - off);
      const r = await this.cmd(`m${(addr + off).toString(16)},${n.toString(16)}`);
      if (r.startsWith('E') || r.length !== n * 2) {
        out.fill(0, off, off + n); // unmapped
      } else {
        Buffer.from(r, 'hex').copy(out, off);
      }
    }
    return out;
  }
  // Detach so the emulator keeps running.
  detach() { this.sock.write(`$D#${checksum('D')}`); setTimeout(() => this.sock.destroy(), 100); }
}

function connect() {
  return new Promise((resolve, reject) => {
    const s = net.connect(PORT, '127.0.0.1', () => resolve(new Gdb(s)));
    s.on('error', reject);
  });
}

const num = (s) => Number.parseInt(s, s.startsWith('0x') ? 16 : 10);
const [mode, ...args] = process.argv.slice(2);
const g = await connect();
await g.cmd('?'); // stub halts the target on connect; reads happen while halted

try {
  if (mode === 'watch') {
    // Addresses used by SamuTecYT/ORAS-MMO; we're checking whether they hold on AS 1.4.
    const b = await g.read(0x08C6E884, 0x1C);
    console.log(JSON.stringify({
      map: b.readUInt16LE(0x00),
      facing: b.readUInt16LE(0x02),
      x: b.readFloatLE(0x10),
      z: b.readFloatLE(0x14),
      y: b.readFloatLE(0x18),
      raw: b.toString('hex'),
    }));
  } else if (mode === 'read') {
    const addr = num(args[0]), len = num(args[1]);
    const b = await g.read(addr, len);
    for (let i = 0; i < len; i += 16) {
      console.log((addr + i).toString(16).padStart(8, '0') + '  ' + b.subarray(i, i + 16).toString('hex').match(/../g).join(' '));
    }
  } else if (mode === 'dump') {
    const start = num(args[0]), end = num(args[1]);
    writeFileSync(args[2], await g.read(start, end - start));
    console.log(`wrote ${end - start} bytes to ${args[2]}`);
  } else if (mode === 'find-float') {
    const start = num(args[0]), end = num(args[1]), v = Number(args[2]), tol = Number(args[3] ?? 0.01);
    const b = await g.read(start, end - start);
    for (let i = 0; i + 4 <= b.length; i += 4) {
      if (Math.abs(b.readFloatLE(i) - v) <= tol) console.log((start + i).toString(16));
    }
  } else if (mode === 'find-u32') {
    // Aligned 32-bit search, e.g. for the player vtable 0x005DEDC0.
    const start = num(args[0]), end = num(args[1]), v = num(args[2]) >>> 0;
    const STEP = 0x10000;
    for (let a = start; a < end; a += STEP) {
      const b = await g.read(a, Math.min(STEP, end - a));
      for (let i = 0; i + 4 <= b.length; i += 4) {
        if (b.readUInt32LE(i) === v) console.log((a + i).toString(16));
      }
    }
  } else if (mode === 'find-pos') {
    // Find X/Z/Y float triplets (as in the player object at +0x14) inside a box.
    // find-pos <start> <end> <xmin> <xmax> <ymin> <ymax> [zmin] [zmax]
    const start = num(args[0]), end = num(args[1]);
    const [xmin, xmax, ymin, ymax] = args.slice(2, 6).map(Number);
    const zmin = Number(args[6] ?? -50), zmax = Number(args[7] ?? 50);
    const STEP = 0x10000;
    for (let a = start; a < end; a += STEP) {
      const b = await g.read(a, Math.min(STEP, end - a) + 8);
      for (let i = 0; i + 12 <= b.length && i < STEP; i += 4) {
        const x = b.readFloatLE(i), z = b.readFloatLE(i + 4), y = b.readFloatLE(i + 8);
        if (x >= xmin && x <= xmax && y >= ymin && y <= ymax && z >= zmin && z <= zmax) {
          console.log(`${(a + i).toString(16)}  x=${x.toFixed(2)} z=${z.toFixed(2)} y=${y.toFixed(2)}`);
        }
      }
    }
  } else if (mode === 'write-float') {
    // WRITES game memory. write-float <addr> <value> [<addr> <value> ...]
    for (let i = 0; i + 1 < args.length; i += 2) {
      const addr = num(args[i]);
      const b = Buffer.alloc(4);
      b.writeFloatLE(Number(args[i + 1]));
      const r = await g.cmd(`M${addr.toString(16)},4:${b.toString('hex')}`);
      console.log(`${addr.toString(16)} <- ${args[i + 1]}: ${r || '(empty reply)'}`);
    }
  } else if (mode === 'write-u32') {
    // WRITES game memory. write-u32 <addr> <valueHex> [<addr> <valueHex> ...]
    for (let i = 0; i + 1 < args.length; i += 2) {
      const addr = num(args[i]);
      const b = Buffer.alloc(4);
      b.writeUInt32LE(num(args[i + 1]) >>> 0);
      const r = await g.cmd(`M${addr.toString(16)},4:${b.toString('hex')}`);
      console.log(`${addr.toString(16)} <- ${args[i + 1]}: ${r || '(empty reply)'}`);
    }
  } else if (mode === 'catch-break') {
    // Break at <addr>; print r0-r3, lr and <dumpLen> bytes at r1. Repeats <hits> times.
    // catch-break <addr> [hits=3] [timeoutMs=30000] [dumpLen=0x30]
    const addr = num(args[0]), hits = Number(args[1] ?? 3), timeout = Number(args[2] ?? 30000);
    const dumpLen = num(args[3] ?? '0x30');
    const z = await g.cmd(`Z0,${addr.toString(16)},4`);
    if (z !== 'OK') console.log(`breakpoint not accepted: ${JSON.stringify(z)}`);
    else {
      for (let h = 0; h < hits; h++) {
        const cont = g.cmd('c');
        const stop = await Promise.race([cont, new Promise((r) => setTimeout(() => r(null), timeout))]);
        if (stop === null) { g.sock.write('\x03'); await cont; console.log(`no hit within ${timeout} ms`); break; }
        const regs = await g.cmd('g');
        const reg = (n) => Buffer.from(regs.slice(n * 8, n * 8 + 8), 'hex').readUInt32LE(0);
        const r = [0, 1, 2, 3].map(reg);
        console.log(`hit ${h + 1}: pc=${reg(15).toString(16)} lr=${reg(14).toString(16)} r0=${r[0].toString(16)} r1=${r[1].toString(16)} r2=${r[2].toString(16)} r3=${r[3].toString(16)}`);
        const mem = await g.read(r[1], dumpLen);
        for (let i = 0; i < dumpLen; i += 16) console.log(`   ${(r[1] + i).toString(16)}  ${mem.subarray(i, i + 16).toString('hex').match(/../g).join(' ')}`);
        if (r[3] >= 0x08000000 && r[3] < 0x20000000) {
          const ex = await g.read(r[3], 0x20);
          console.log(`   r3 -> ${ex.toString('hex').match(/../g).join(' ')}`);
        }
        // Step past the breakpoint: remove, single-step, re-insert.
        await g.cmd(`z0,${addr.toString(16)},4`);
        await g.cmd('s');
        await g.cmd(`Z0,${addr.toString(16)},4`);
      }
      await g.cmd(`z0,${addr.toString(16)},4`);
    }
  } else if (mode === 'catch-write') {
    // Pause the game when <addr> is written; report PC/LR of the writer. Repeats <hits> times.
    // catch-write <addr> [hits=3] [timeoutMs=4000]
    const addr = num(args[0]), hits = Number(args[1] ?? 3), timeout = Number(args[2] ?? 4000);
    const z = await g.cmd(`Z2,${addr.toString(16)},4`);
    if (z !== 'OK') { console.log(`watchpoint not supported/accepted: ${JSON.stringify(z)}`); }
    else {
      const seen = new Map();
      let prev = (await g.read(addr, 4)).toString('hex');
      for (let h = 0; h < hits; h++) {
        const cont = g.cmd('c');
        const stop = await Promise.race([cont, new Promise((r) => setTimeout(() => r(null), timeout))]);
        if (stop === null) {
          g.sock.write('\x03'); // interrupt; the stop reply resolves the pending 'c'
          await cont;
          console.log(`no write within ${timeout} ms`);
          break;
        }
        const regs = await g.cmd('g');
        const reg = (n) => Buffer.from(regs.slice(n * 8, n * 8 + 8), 'hex').readUInt32LE(0);
        const pc = reg(15), lr = reg(14);
        const now = (await g.read(addr, 4)).toString('hex');
        const changed = now !== prev;
        const fval = Buffer.from(now, 'hex').readFloatLE(0);
        prev = now;
        const key = `pc=${pc.toString(16)} lr=${lr.toString(16)}${changed ? ' WRITE' : ''}`;
        seen.set(key, (seen.get(key) ?? 0) + 1);
        console.log(`hit ${h + 1}: ${key} val=${fval.toFixed(3)} r0=${reg(0).toString(16)} r1=${reg(1).toString(16)} r4=${reg(4).toString(16)} r5=${reg(5).toString(16)}`);
      }
      await g.cmd(`z2,${addr.toString(16)},4`);
      console.log('distinct writers:', [...seen.entries()].map(([k, n]) => `${k} x${n}`).join(' | '));
    }
  } else {
    console.log('modes: watch | read | dump | find-float | find-u32 | find-pos | write-float | catch-write');
  }
} finally {
  g.detach();
}
