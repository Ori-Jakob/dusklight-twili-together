// GameCube disc (GCM), Yaz0 and RARC readers. Read-only.

const fs = require("node:fs");

class Disc {
    constructor(file) {
        this.fd = fs.openSync(file, "r");
        const head = this.read(0, 0x440);
        this.gameId = head.toString("latin1", 0, 6);
        const fstOff = head.readUInt32BE(0x424);
        const fstSize = head.readUInt32BE(0x428);
        const fst = this.read(fstOff, fstSize);
        const count = fst.readUInt32BE(8);
        const strings = count * 12;
        const name = (i) => {
            const off = strings + (fst.readUInt32BE(i * 12) & 0xffffff);
            return fst.toString("latin1", off, fst.indexOf(0, off));
        };
        this.files = new Map();
        // Directory entries: next = index after the last child.
        const walk = (start, end, prefix) => {
            for (let i = start; i < end;) {
                const isDir = fst[i * 12] !== 0;
                const n = name(i);
                if (isDir) {
                    const next = fst.readUInt32BE(i * 12 + 8);
                    walk(i + 1, next, `${prefix}${n}/`);
                    i = next;
                } else {
                    this.files.set(`${prefix}${n}`, { off: fst.readUInt32BE(i * 12 + 4), size: fst.readUInt32BE(i * 12 + 8) });
                    i++;
                }
            }
        };
        walk(1, count, "");
    }

    read(off, size) {
        const buf = Buffer.alloc(size);
        fs.readSync(this.fd, buf, 0, size, off);
        return buf;
    }

    list(prefix) {
        return [...this.files.keys()].filter((k) => k.startsWith(prefix));
    }

    file(path) {
        const f = this.files.get(path);
        return f ? this.read(f.off, f.size) : null;
    }
}

function yaz0(src) {
    if (src.toString("latin1", 0, 4) !== "Yaz0") return src;
    const size = src.readUInt32BE(4);
    const dst = Buffer.alloc(size);
    let s = 16;
    let d = 0;
    while (d < size) {
        const code = src[s++];
        for (let bit = 7; bit >= 0 && d < size; bit--) {
            if (code & (1 << bit)) {
                dst[d++] = src[s++];
            } else {
                const b1 = src[s++];
                const b2 = src[s++];
                const back = ((b1 & 0x0f) << 8 | b2) + 1;
                let len = b1 >> 4;
                len = len === 0 ? src[s++] + 0x12 : len + 2;
                for (let k = 0; k < len; k++, d++) dst[d] = dst[d - back];
            }
        }
    }
    return dst;
}

// RARC: returns Map of lower-case "dir/file" path -> Buffer (Yaz0 entries decompressed).
function rarc(buf) {
    buf = yaz0(buf);
    if (buf.toString("latin1", 0, 4) !== "RARC") throw new Error("not a RARC");
    const dataOff = 0x20 + buf.readUInt32BE(0x0c);
    const info = 0x20;
    const nodeCount = buf.readUInt32BE(info);
    const nodeOff = info + buf.readUInt32BE(info + 4);
    const entryOff = info + buf.readUInt32BE(info + 0x0c);
    const strOff = info + buf.readUInt32BE(info + 0x14);
    const str = (o) => buf.toString("latin1", strOff + o, buf.indexOf(0, strOff + o));
    const out = new Map();
    const visit = (n, prefix) => {
        const node = nodeOff + n * 0x10;
        const count = buf.readUInt16BE(node + 0x0a);
        const first = buf.readUInt32BE(node + 0x0c);
        for (let i = 0; i < count; i++) {
            const e = entryOff + (first + i) * 0x14;
            const typeName = buf.readUInt32BE(e + 4);
            const flags = typeName >>> 24;
            const name = str(typeName & 0xffffff);
            const dataOrNode = buf.readUInt32BE(e + 8);
            const size = buf.readUInt32BE(e + 0x0c);
            if (flags & 0x02) {
                if (name !== "." && name !== "..") visit(dataOrNode, `${prefix}${name.toLowerCase()}/`);
            } else {
                out.set(`${prefix}${name.toLowerCase()}`, yaz0(buf.subarray(dataOff + dataOrNode, dataOff + dataOrNode + size)));
            }
        }
    };
    if (nodeCount > 0) visit(0, "");
    return out;
}

// The first file in an archive whose path ends with `name`.
function arcFile(arc, name) {
    for (const [k, v] of arc) if (k === name || k.endsWith(`/${name}`)) return v;
    return null;
}

module.exports = { Disc, yaz0, rarc, arcFile };
