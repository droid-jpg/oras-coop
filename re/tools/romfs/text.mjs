// Gen 6 text file decoder: decodeText(buf) -> array of strings.
export function decodeText(b) {
  const sections = b.readUInt16LE(0), lines = b.readUInt16LE(2);
  if (sections !== 1 || lines === 0 || lines > 5000) return null;
  const sec = b.readUInt32LE(0x0C);
  if (sec + 4 > b.length) return null;
  const out = [];
  let key = 0x7C89;
  for (let i = 0; i < lines; i++) {
    const t = sec + 4 + i * 8;
    if (t + 8 > b.length) return null;
    const off = sec + b.readUInt32LE(t), len = b.readUInt16LE(t + 4);
    if (off + len * 2 > b.length) return null;
    let k = key, s = '';
    for (let c = 0; c < len; c++) {
      const ch = b.readUInt16LE(off + c * 2) ^ k;
      k = ((k << 3) | (k >>> 13)) & 0xFFFF;
      if (ch === 0) break;
      s += ch === 0x10 ? '{var}' : ch < 0x20 ? `<${ch.toString(16)}>` : String.fromCharCode(ch);
    }
    out.push(s);
    key = (key + 0x2983) & 0xFFFF;
  }
  return out;
}
