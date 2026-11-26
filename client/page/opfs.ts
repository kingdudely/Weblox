// @ts-nocheck
// In-memory OPFS.  The engine keeps asset and audio caches here; nothing on
// the join path depends on them surviving a run.
const root = new Map(); // name -> Uint8Array

class FileHandle {
  constructor(name) { this.name = name; }
  async getFile() {
    const b = root.get(this.name) || new Uint8Array();
    return new File([b], this.name);
  }
  async createWritable() {
    const parts = [];
    return {
      write: async (c) => { parts.push(Buffer.from(c.buffer ? c : Buffer.from(c), c.byteOffset || 0, c.byteLength ?? c.length)); },
      close: async () => {
        const total = parts.reduce((n, p) => n + p.length, 0);
        const out = new Uint8Array(total);
        let at = 0;
        for (const p of parts) { out.set(p, at); at += p.length; }
        root.set(this.name, out);
      },
      abort: async () => {},
    };
  }
  async createSyncAccessHandle() {
    const self = this;
    return {
      getSize: () => (root.get(self.name) || []).length,
      read: (buf, off) => {
        const b = root.get(self.name) || new Uint8Array();
        const n = Math.min(buf.length - off, b.length);
        buf.set(b.subarray(0, n), off);
        return n;
      },
      write: (buf, off) => {
        const b = root.get(self.name) || new Uint8Array();
        const out = new Uint8Array(Math.max(b.length, off + buf.length));
        out.set(b); out.set(buf, off); root.set(self.name, out);
        return buf.length;
      },
      truncate: (n) => { root.set(self.name, (root.get(self.name) || new Uint8Array()).slice(0, n)); },
      flush: () => {},
      close: () => {},
    };
  }
}

class Dir {
  constructor(prefix = "") { this.prefix = prefix; }
  _name(n) { return this.prefix + n; }
  async getFileHandle(name, o = {}) {
    const key = this._name(name);
    if (!root.has(key) && !o.create) throw Object.assign(new Error("NotFound"), { name: "NotFoundError" });
    return new FileHandle(key);
  }
  async getDirectoryHandle(name, o = {}) {
    return new Dir(this._name(name) + "/");
  }
  async removeEntry(name, o = {}) {
    const key = this._name(name);
    if (o.recursive) for (const k of [...root.keys()]) if (k === key || k.startsWith(key + "/")) root.delete(k);
    else root.delete(key);
  }
  async *keys() { for (const k of root.keys()) if (k.startsWith(this.prefix)) yield k.slice(this.prefix.length); }
  async *values() { for await (const k of this.keys()) yield new FileHandle(this.prefix + k); }
  async *entries() { for await (const k of this.keys()) yield [k, new FileHandle(this.prefix + k)]; }
}

export function makeOPFS() { return new Dir(); }
