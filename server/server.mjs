// oras-coop relay server. No dependencies; Node 18+.
// Usage: node server.mjs [port]      (default 7777, or PORT env)
// See ../PROTOCOL.md for the frame format.
import net from 'node:net';
import os from 'node:os';

const PORT = Number(process.argv[2] ?? process.env.PORT ?? 7777);
const FRAME = 48;
const PROTO = 1;
const MAX_PER_ROOM = 8;
const HELLO_TIMEOUT_MS = 5000;
const IDLE_TIMEOUT_MS = 10000;
const MAX_FRAMES_PER_SEC = 60;

const T = { HELLO: 1, WELCOME: 2, STATE: 3, CHAT: 4, JOIN: 5, LEAVE: 6, ERROR: 7, PING: 8, GREET: 9, TALK: 10 };
const ERR = { ROOM_FULL: 1, BAD_HELLO: 2, VERSION: 3 };

/** @type {Map<string, Map<number, Client>>} */
const rooms = new Map();

const cstr = (buf, start, len) => {
  const s = buf.subarray(start, start + len);
  const end = s.indexOf(0);
  return s.subarray(0, end < 0 ? len : end).toString('latin1').replace(/[^\x20-\x7e]/g, '');
};

function frame(type, player = 0) {
  const b = Buffer.alloc(FRAME);
  b[0] = type;
  b[1] = player;
  b.writeUInt16LE(PROTO, 2);
  return b;
}

function log(...a) {
  console.log(new Date().toISOString().slice(11, 19), ...a);
}

class Client {
  constructor(sock) {
    this.sock = sock;
    this.buf = Buffer.alloc(0);
    this.room = null;
    this.id = 0;
    this.name = '';
    this.lastSeen = Date.now();
    this.windowStart = Date.now();
    this.framesInWindow = 0;
    this.helloTimer = setTimeout(() => this.drop('no hello'), HELLO_TIMEOUT_MS);

    sock.setNoDelay(true);
    sock.on('data', (d) => this.onData(d));
    sock.on('close', () => this.leave());
    sock.on('error', () => {});
  }

  send(b) {
    if (!this.sock.destroyed) this.sock.write(b);
  }

  drop(reason) {
    log(`drop ${this.sock.remoteAddress} (${this.name || '?'}): ${reason}`);
    this.sock.destroy();
  }

  onData(d) {
    this.buf = Buffer.concat([this.buf, d]);
    while (this.buf.length >= FRAME) {
      const f = Buffer.from(this.buf.subarray(0, FRAME));
      this.buf = this.buf.subarray(FRAME);
      this.onFrame(f);
      if (this.sock.destroyed) return;
    }
  }

  onFrame(f) {
    this.lastSeen = Date.now();

    const now = Date.now();
    if (now - this.windowStart >= 1000) { this.windowStart = now; this.framesInWindow = 0; }
    if (++this.framesInWindow > MAX_FRAMES_PER_SEC) return; // drop excess frames silently

    const type = f[0];
    if (!this.room) {
      if (type !== T.HELLO) return this.drop('expected hello');
      return this.hello(f);
    }

    switch (type) {
      case T.STATE:
      case T.CHAT:
      case T.GREET:
      case T.TALK:
        f[1] = this.id; // stamp the real sender
        this.broadcast(f);
        break;
      case T.PING:
        break;
      default:
        break; // ignore unknown types for forward compatibility
    }
  }

  hello(f) {
    clearTimeout(this.helloTimer);
    if (f.readUInt16LE(2) !== PROTO) {
      const e = frame(T.ERROR); e[4] = ERR.VERSION; this.send(e);
      return this.drop(`protocol ${f.readUInt16LE(2)} != ${PROTO}`);
    }
    const roomCode = cstr(f, 4, 12).trim().toUpperCase();
    const name = cstr(f, 16, 16).trim() || 'Trainer';
    if (!roomCode) {
      const e = frame(T.ERROR); e[4] = ERR.BAD_HELLO; this.send(e);
      return this.drop('empty room code');
    }

    let room = rooms.get(roomCode);
    if (!room) { room = new Map(); rooms.set(roomCode, room); }
    let id = 0;
    for (let i = 1; i <= MAX_PER_ROOM; i++) if (!room.has(i)) { id = i; break; }
    if (!id) {
      const e = frame(T.ERROR); e[4] = ERR.ROOM_FULL; this.send(e);
      return this.drop(`room ${roomCode} full`);
    }

    this.room = roomCode;
    this.id = id;
    this.name = name;
    room.set(id, this);

    const w = frame(T.WELCOME, id);
    w[4] = MAX_PER_ROOM;
    this.send(w);

    for (const other of room.values()) {
      if (other === this) continue;
      this.send(joinFrame(other));
    }
    this.broadcast(joinFrame(this));
    log(`join  room=${roomCode} id=${id} name=${name} from ${this.sock.remoteAddress} (${room.size} in room)`);
  }

  broadcast(f) {
    const room = rooms.get(this.room);
    if (!room) return;
    for (const other of room.values()) if (other !== this) other.send(f);
  }

  leave() {
    clearTimeout(this.helloTimer);
    if (!this.room) return;
    const room = rooms.get(this.room);
    if (room && room.get(this.id) === this) {
      room.delete(this.id);
      this.broadcast(frame(T.LEAVE, this.id));
      log(`leave room=${this.room} id=${this.id} name=${this.name} (${room.size} left)`);
      if (room.size === 0) rooms.delete(this.room);
    }
    this.room = null;
  }
}

function joinFrame(c) {
  const j = frame(T.JOIN, c.id);
  j.write(c.name.slice(0, 15), 4, 'latin1');
  return j;
}

setInterval(() => {
  const now = Date.now();
  for (const room of rooms.values()) {
    for (const c of room.values()) if (now - c.lastSeen > IDLE_TIMEOUT_MS) c.drop('timeout');
  }
}, 2000);

const server = net.createServer((sock) => new Client(sock));
server.on('error', (e) => { console.error(`server error: ${e.message}`); process.exit(1); });
server.listen(PORT, '0.0.0.0', () => {
  console.log(`oras-coop server listening on port ${PORT}`);
  console.log('Give friends one of these addresses:');
  for (const [ifname, addrs] of Object.entries(os.networkInterfaces())) {
    for (const a of addrs ?? []) {
      if (a.family === 'IPv4' && !a.internal) console.log(`  ${a.address}  (${ifname})`);
    }
  }
  console.log('  127.0.0.1  (this PC only)');
});
