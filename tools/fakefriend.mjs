// A pretend player for testing: joins a room and walks in a circle.
// Usage: node fakefriend.mjs [--server 127.0.0.1] [--port 7777] [--room HOENN] [--name Bot]
//                            [--map 6] [--x 1881] [--y 3141] [--radius 54] [--listen]
// --listen only prints what it receives (doesn't move).
import net from 'node:net';

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => {
    if (a.startsWith('--')) acc.push([a.slice(2), all[i + 1]?.startsWith('--') || all[i + 1] === undefined ? true : all[i + 1]]);
    return acc;
  }, []),
);
const opt = {
  server: args.server ?? '127.0.0.1', port: Number(args.port ?? 7777), room: args.room ?? 'HOENN',
  name: args.name ?? 'Bot', map: Number(args.map ?? 6), x: Number(args.x ?? 1881), y: Number(args.y ?? 3141),
  radius: Number(args.radius ?? 54), listen: !!args.listen,
};

const FRAME = 48;
const T = { HELLO: 1, WELCOME: 2, STATE: 3, CHAT: 4, JOIN: 5, LEAVE: 6, ERROR: 7, PING: 8 };
const names = {};
const frame = (type) => { const b = Buffer.alloc(FRAME); b[0] = type; b.writeUInt16LE(1, 2); return b; };
const cstr = (b, o, n) => { const s = b.subarray(o, o + n); const z = s.indexOf(0); return s.subarray(0, z < 0 ? n : z).toString('latin1'); };

const sock = net.connect(opt.port, opt.server, () => {
  const h = frame(T.HELLO);
  h.write(opt.room.slice(0, 11), 4, 'latin1');
  h.write(opt.name.slice(0, 15), 16, 'latin1');
  sock.write(h);
});

let buf = Buffer.alloc(0);
let myId = 0;
sock.on('data', (d) => {
  buf = Buffer.concat([buf, d]);
  while (buf.length >= FRAME) {
    const f = buf.subarray(0, FRAME); buf = buf.subarray(FRAME);
    const id = f[1];
    switch (f[0]) {
      case T.WELCOME: myId = id; console.log(`welcome: I am #${id}`); break;
      case T.JOIN: names[id] = cstr(f, 4, 16); console.log(`join #${id} ${names[id]}`); break;
      case T.LEAVE: console.log(`leave #${id} ${names[id] ?? ''}`); break;
      case T.STATE:
        if (opt.listen) console.log(`state #${id} ${names[id] ?? ''} map=${f.readUInt16LE(4)} facing=${f.readUInt16LE(6)} x=${f.readFloatLE(8).toFixed(2)} y=${f.readFloatLE(16).toFixed(2)}`);
        break;
      case T.CHAT: console.log(`chat #${id} ${names[id] ?? ''}: ${cstr(f, 4, 40)}`); break;
      case T.ERROR: console.log(`error code ${f[4]}`); break;
    }
  }
});
sock.on('close', () => { console.log('disconnected'); process.exit(0); });
sock.on('error', (e) => { console.error(e.message); process.exit(1); });

let t = 0, seq = 0;
setInterval(() => {
  if (!myId) return;
  if (opt.listen) { sock.write(frame(T.PING)); return; }
  t += 0.05;
  const s = frame(T.STATE);
  const x = opt.x + Math.cos(t) * opt.radius, y = opt.y + Math.sin(t) * opt.radius;
  // facing: rough guess at the game's values (0 up, 3 right seen so far)
  s.writeUInt16LE(opt.map, 4);
  s.writeUInt16LE(Math.cos(t) > 0 ? 3 : 2, 6);
  s.writeFloatLE(x, 8); s.writeFloatLE(2, 12); s.writeFloatLE(y, 16);
  s.writeUInt32LE(++seq, 24);
  // Facing = direction of travel around the circle (tangent); east when standing still.
  const dir = opt.radius > 0 ? [-Math.sin(t), Math.cos(t)] : [1, 0];
  s.writeFloatLE(dir[0], 28); s.writeFloatLE(dir[1], 32);
  sock.write(s);
}, 66);

if (!opt.listen) {
  setTimeout(() => { const c = frame(T.CHAT); c.write('hello from the bot', 4, 'latin1'); sock.write(c); }, 1500);
}
