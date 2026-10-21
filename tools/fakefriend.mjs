// A pretend player for testing: joins a room and walks in a circle.
// Usage: node fakefriend.mjs [--server 127.0.0.1] [--port 7777] [--room HOENN] [--name Bot]
//                            [--map 6] [--x 1881] [--y 3141] [--radius 54] [--listen]
//                            [--speed 0.05] [--anim 0x10] [--stopgo] [--model 0xab]
// --speed radians per update (0.05 ~ 40 units/s at r=54), --anim the animation id sent while
// moving (0x10 = walk), --stopgo walks 4 s then stands 2 s to test idle/walk switching.
// --listen only prints what it receives (doesn't move). --busy sends the busy bit (as in a cutscene)
// while walking; --battle also sends a made-up battle (T_BATTLE) whose HP ticks down.
// --events sends a catch, an evolution and a badge (T_EVENT) a few seconds after joining.
// --map2 <id> --after N walks through a "door" to map <id> after N seconds (at --x2/--y2); --nowarp
// sets the STATE "no warp" bit (as if it flew) so friends don't learn that as a door.
// --party <file> answers party requests (T_PARTY, for battling us) with the party in <file>
// (1..6 Pokemon of 0x104 bytes: encrypted data + party stats, e.g. from tools/dumpparty.mjs).
// --z <height>: without it the bot takes the height of the first real player it hears on its map
// (2 until then). A wrong height puts it in the air or underground (cave floors are far below 0),
// so friends see an off-screen arrow instead of its name.
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
  radius: Number(args.radius ?? 54), listen: !!args.listen, z: Number(args.z ?? 2), autoZ: args.z === undefined,
  speed: Number(args.speed ?? 0.05), anim: Number(args.anim ?? 0x10), stopgo: !!args.stopgo,
  model: Number(args.model ?? 0xab), // player model sent to others: 0xAB May, 0xAC Brendan
  // --x2/--y2 --after N: move the circle centre to (x2,y2) after N seconds (tests moved collision)
  busy: !!args.busy, battle: !!args.battle,
  x2: args.x2 && Number(args.x2), y2: args.y2 && Number(args.y2), after: Number(args.after ?? 0),
  // --map2 <id>: with --after N, walk through a 'door' to map <id> after N seconds (at x2/y2 if given)
  map2: args.map2 && Number(args.map2),
};

const FRAME = 48;
const T = { HELLO: 1, WELCOME: 2, STATE: 3, CHAT: 4, JOIN: 5, LEAVE: 6, ERROR: 7, PING: 8, GREET: 9, TALK: 10, BATTLE: 11, TRADE: 12, EVENT: 13, PARTY: 14 };
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
        if (opt.autoZ && id !== myId && f.readUInt16LE(4) === opt.map) {
          opt.z = f.readFloatLE(12);
          opt.autoZ = false;
          console.log(`height ${opt.z.toFixed(1)} (from #${id} ${names[id] ?? ''})`);
        }
        if (opt.listen) console.log(`state #${id} ${names[id] ?? ''} map=${f.readUInt16LE(4)} facing=${f.readUInt16LE(6)} x=${f.readFloatLE(8).toFixed(2)} y=${f.readFloatLE(16).toFixed(2)} z=${f.readFloatLE(12).toFixed(2)} st=${f[37]} anim=0x${f[36].toString(16)} model=0x${f.readUInt16LE(38).toString(16)} t=${f.readUInt32LE(40)}`);
        break;
      case T.CHAT: console.log(`chat #${id} ${names[id] ?? ''}: ${cstr(f, 4, 40)}`); break;
      case T.TALK: console.log(`talk #${id} ${names[id] ?? ''} -> #${f[4]}`); break;
      case T.BATTLE: console.log(`battle #${id} side=${f[4]} count=${f[5]}` + [...Array(f[5])].map((_, i) => ` [${f.readUInt16LE(6 + i * 7)} ${f.readUInt16LE(8 + i * 7)}/${f.readUInt16LE(10 + i * 7)} L${f[12 + i * 7]}]`).join('')); break;
      case T.TRADE: console.log(`trade #${id} op=${f[4]} to=#${f[5]} chunk=${f[6]}/${f[7]}`); if (f[5] === myId) onTrade(f, id); break;
      case T.EVENT: console.log(`event #${id} ${names[id] ?? ''} kind=${f[4]} value=${f[5]} species=${f.readUInt16LE(6)} from=${f.readUInt16LE(8)}`); break;
      case T.PARTY:
        console.log(`party #${id} op=${f[4]} to=#${f[5]} piece=${f[6]} count=${f[7]}`);
        if (f[4] === 1 && f[5] === myId) sendParty(id);
        break;
      case T.ERROR: console.log(`error code ${f[4]}`); break;
    }
  }
});
sock.on('close', () => { console.log('disconnected'); process.exit(0); });
sock.on('error', (e) => { console.error(e.message); process.exit(1); });

let t = 0, seq = 0;
const started = Date.now();
setInterval(() => {
  if (!myId) return;
  if (opt.listen) { sock.write(frame(T.PING)); return; }
  const moving = !opt.stopgo || (Date.now() / 1000) % 6 < 4;
  if (moving) t += opt.speed;
  const s = frame(T.STATE);
  const jumped = opt.after && Date.now() - started > opt.after * 1000;
  const cx = jumped && opt.x2 ? opt.x2 : opt.x, cy = jumped && opt.y2 ? opt.y2 : opt.y;
  const x = cx + Math.cos(t) * opt.radius, y = cy + Math.sin(t) * opt.radius;
  // facing: rough guess at the game's values (0 up, 3 right seen so far)
  s.writeUInt16LE(jumped && opt.map2 ? opt.map2 : opt.map, 4);
  s.writeUInt16LE(Math.cos(t) > 0 ? 3 : 2, 6);
  s.writeFloatLE(x, 8); s.writeFloatLE(opt.z, 12); s.writeFloatLE(y, 16);
  s.writeUInt32LE(++seq, 24);
  // Facing = direction of travel around the circle (tangent); east when standing still.
  const dir = opt.radius > 0 ? [-Math.sin(t), Math.cos(t)] : [1, 0];
  s.writeFloatLE(dir[0], 28); s.writeFloatLE(dir[1], 32);
  s.writeUInt8(moving && opt.radius > 0 ? opt.anim : 0, 36);
  s.writeUInt16LE(opt.model, 38);
  // status bit 0: busy; bit 3 (--nowarp): a map change now is Fly/Escape Rope, not a door
  s.writeUInt8((opt.busy || opt.battle ? 1 : 0) | (args.nowarp ? 8 : 0), 37);
  s.writeUInt32LE(Date.now() >>> 0, 40); // sender clock (ms), lets receivers interpolate
  sock.write(s);
}, 66);

// --trader: answers a trade like a player who picks one back and accepts. Offers --offer <file>
// (a 0x104-byte Pokemon saved by an earlier run), else the same Pokemon back. Saves what it got
// to --save <file>. --offer-first <id>: offers to player <id> right away.
const PK = 0x104, CHUNK = 40, CHUNKS = Math.ceil(PK / CHUNK);
const trade = { from: 0, got: Buffer.alloc(PK), mask: 0, mine: null };
function fnv(...bufs) { let h = 2166136261; for (const b of bufs) for (const x of b) h = Math.imul(h ^ x, 16777619) >>> 0; return h; }
function sendOffer(to, pk) {
  for (let c = 0; c < CHUNKS; c++) {
    const f = frame(T.TRADE); f[4] = 1; f[5] = to; f[6] = c; f[7] = CHUNKS;
    pk.copy(f, 8, c * CHUNK, Math.min(PK, c * CHUNK + CHUNK)); sock.write(f);
  }
}
function sendAccept(to) {
  const h = myId < to ? fnv(trade.mine, trade.got) : fnv(trade.got, trade.mine);
  const f = frame(T.TRADE); f[4] = 2; f[5] = to; f.writeUInt32LE(h, 8); sock.write(f);
  console.log(`accepted, hash ${h.toString(16)}`);
}
async function onTrade(f, id) {
  if (!args.trader) return;
  const op = f[4];
  if (op === 1) {
    if (f[6] === 0) { trade.from = id; trade.mask = 0; }
    f.copy(trade.got, f[6] * CHUNK, 8, 8 + Math.min(CHUNK, PK - f[6] * CHUNK));
    trade.mask |= 1 << f[6];
    if (trade.mask !== (1 << CHUNKS) - 1) return;
    console.log(`got a complete offer from #${id}`);
    if (args.save) (await import('node:fs')).writeFileSync(args.save, trade.got);
    if (!trade.mine) { trade.mine = args.offer ? (await import('node:fs')).readFileSync(args.offer) : Buffer.from(trade.got); sendOffer(id, trade.mine); }
    sendAccept(id);
  } else if (op === 3) { console.log('trade cancelled'); trade.mine = null; }
  else if (op === 4) { console.log(`COMMIT from #${id}, hash ${f.readUInt32LE(8).toString(16)}`); trade.mine = null; }
}
if (args['offer-first'] && args.offer) {
  setTimeout(async () => { trade.mine = (await import('node:fs')).readFileSync(args.offer); sendOffer(Number(args['offer-first']), trade.mine); console.log('offered first'); }, 2000);
}

if (opt.battle) {
  // Our team: species 258 Lv12 and 255 Lv10; the opponent: 261 Lv8. Opponent HP ticks down.
  let foeHp = 26;
  setInterval(() => {
    if (!myId) return;
    foeHp = foeHp > 2 ? foeHp - 2 : 26;
    const sides = [[[258, 31, 35, 12], [255, 30, 30, 10]], [[261, foeHp, 26, 8]]];
    sides.forEach((mons, side) => {
      const b = frame(T.BATTLE); b[4] = side; b[5] = mons.length;
      mons.forEach(([sp, hp, max, lv], i) => { const o = 6 + i * 7; b.writeUInt16LE(sp, o); b.writeUInt16LE(hp, o + 2); b.writeUInt16LE(max, o + 4); b[o + 6] = lv; });
      sock.write(b);
    });
  }, 1000);
}

if (!opt.listen) {
  setTimeout(() => { const c = frame(T.CHAT); c.write('hello from the bot', 4, 'latin1'); sock.write(c); }, 1500);
}

if (args.events) {
  // Caught a Wingull (278), Torchic (255) evolved into Combusken (256), earned the Knuckle Badge (2).
  [[1, 0, 278, 0], [2, 0, 256, 255], [3, 2, 0, 0]].forEach(([kind, value, species, from], i) => {
    setTimeout(() => {
      const e = frame(T.EVENT); e[4] = kind; e[5] = value; e.writeUInt16LE(species, 6); e.writeUInt16LE(from, 8);
      sock.write(e);
    }, 4000 + i * 1000);
  });
}

function sendParty(to) {
  const data = args.party ? (await_party ??= (globalThis.fsMod).readFileSync(args.party)) : Buffer.alloc(0);
  const count = Math.min(6, Math.floor(data.length / 0x104));
  const frames = [];
  for (let piece = 0; piece < Math.max(1, count * 7); piece++) {
    const p = frame(T.PARTY); p[4] = 2; p[5] = to; p[6] = count ? piece : 0; p[7] = count;
    if (count) { const slot = Math.floor(piece / 7), off = (piece % 7) * 40; data.copy(p, 8, slot * 0x104 + off, slot * 0x104 + Math.min(off + 40, 0x104)); }
    frames.push(p);
  }
  // 20 per 0.5 s: under the server's 60 frames/s with our state frames
  frames.forEach((p, i) => setTimeout(() => sock.write(p), Math.floor(i / 20) * 500));
  console.log(`sent party (${count}) to #${to}`);
}
let await_party = null;
globalThis.fsMod = await import('node:fs');
