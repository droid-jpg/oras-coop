// Checks the server's trade commit. Usage: node tradetest.mjs   (starts its own server on 7791)
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const PORT = 7791, FRAME = 48;
const T = { HELLO: 1, WELCOME: 2, TRADE: 12 };
const OP = { OFFER: 1, ACCEPT: 2, CANCEL: 3, COMMIT: 4 };
const server = spawn(process.execPath, [fileURLToPath(new URL('../server/server.mjs', import.meta.url)), String(PORT)]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function client(name) {
  return new Promise((resolve) => {
    const c = { name, id: 0, got: [], sock: net.connect(PORT, '127.0.0.1') };
    let buf = Buffer.alloc(0);
    c.sock.on('connect', () => {
      const h = Buffer.alloc(FRAME); h[0] = T.HELLO; h.writeUInt16LE(1, 2);
      h.write('TEST', 4); h.write(name, 16); c.sock.write(h);
    });
    c.sock.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      while (buf.length >= FRAME) {
        const f = buf.subarray(0, FRAME); buf = buf.subarray(FRAME);
        if (f[0] === T.WELCOME) { c.id = f[1]; resolve(c); }
        if (f[0] === T.TRADE) c.got.push({ from: f[1], op: f[4], to: f[5], hash: f.readUInt32LE(8) });
      }
    });
    c.trade = (op, to, hash = 0) => {
      const f = Buffer.alloc(FRAME); f[0] = T.TRADE; f.writeUInt16LE(1, 2); f[4] = op; f[5] = to; f.writeUInt32LE(hash, 8);
      c.sock.write(f);
    };
  });
}

const commits = (c) => c.got.filter((g) => g.op === OP.COMMIT);
let failed = 0;
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failed++; };

await sleep(300);
// 1. Both accept the same pair: both get COMMIT.
let a = await client('A'), b = await client('B');
a.trade(OP.OFFER, b.id); await sleep(50); b.trade(OP.OFFER, a.id); await sleep(50);
b.trade(OP.ACCEPT, a.id, 1234); await sleep(100);
check(commits(a).length === 0 && commits(b).length === 0, 'no commit after one accept');
a.trade(OP.ACCEPT, b.id, 1234); await sleep(150);
check(commits(a).length === 1 && commits(b).length === 1 && commits(a)[0].from === b.id && commits(a)[0].hash === 1234,
  'both get COMMIT after both accept');

// 2. Different hashes: no commit.
a.got = []; b.got = [];
a.trade(OP.ACCEPT, b.id, 1); b.trade(OP.ACCEPT, a.id, 2); await sleep(150);
check(commits(a).length === 0 && commits(b).length === 0, 'no commit when the accepts disagree');

// 3. A new offer clears an earlier accept.
a.got = []; b.got = [];
a.trade(OP.ACCEPT, b.id, 7); b.trade(OP.OFFER, a.id); b.trade(OP.ACCEPT, a.id, 7); await sleep(150);
check(commits(a).length === 0, 'an offer after an accept needs a fresh accept');

// 4. One side accepts and disconnects: nothing commits, even for a newcomer with the same id.
a.got = []; b.got = [];
b.trade(OP.CANCEL, a.id); a.trade(OP.CANCEL, b.id);
a.trade(OP.ACCEPT, b.id, 99); await sleep(50);
const bId = b.id; b.sock.destroy(); await sleep(150);
const c = await client('C');
check(c.id === bId, 'newcomer reuses the id');
c.trade(OP.ACCEPT, a.id, 99); await sleep(150);
check(commits(a).length === 0 && commits(c).length === 0, 'no commit with a player who left');

// 5. Clients can't send COMMIT themselves.
a.got = []; c.got = [];
c.trade(OP.COMMIT, a.id, 5); await sleep(100);
check(a.got.length === 0, 'client COMMIT is not relayed');

a.sock.destroy(); c.sock.destroy(); server.kill();
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
