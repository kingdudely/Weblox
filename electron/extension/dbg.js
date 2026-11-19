// Debug tap: capture the instance, the memory, and every fibre create.
// Main world, document_start.
(() => {
  const ORIG = new WeakMap();
  const OS = WebAssembly.Suspending;
  if (OS) {
    class TapSusp extends OS {
      constructor(f) {
        let g = f;
        if (typeof f === "function" && f.length === 5) {
          g = (a, b, c, d, e) => {
            window.__creates = (window.__creates || 0) + 1;
            if (window.__mem) {
              const dv = new DataView(window.__mem.buffer, e, 40);
              const w = [];
              for (let i = 0; i < 10; i++) w.push(dv.getUint32(i * 4, true).toString(16));
              console.log("[tap] create", a.toString(16), b.toString(16), c.toString(16), d, e.toString(16), "s=", w.join(","));
            }
            return f(a, b, c, d, e);
          };
        }
        super(g);
        ORIG.set(this, g);
      }
    }
    WebAssembly.Suspending = TapSusp;
  }

  const wrap = (imports) => {
    try {
      const env = imports && imports.env;
      if (!env) return console.log("[tap] no env");
      if (env.memory) window.__mem = env.memory;
      const log = env.emscripten_console_log;
      if (log) {
        const o = ORIG.get(log) || log;
        const f = (p2) => { console.log("[tap] log fired"); return o(p2); };
        env.emscripten_console_log = OS ? new OS(f) : f;
      }
      const cur = env.__asyncjs__create_suspended_fiber_js;
      if (!cur) return console.log("[tap] no create import");
      const orig = ORIG.get(cur) || cur;
      const fn = (id, base, limit, entry, arg) => {
        if (window.__mem) {
          const d = new DataView(window.__mem.buffer, arg, 40);
          const words = [];
          for (let i = 0; i < 10; i++) words.push(d.getUint32(i * 4, true).toString(16));
          console.log("[tap] create", id.toString(16), base.toString(16), limit.toString(16),
            entry, arg.toString(16), "struct=", words.join(","));
        } else {
          console.log("[tap] create (no mem)");
        }
        return orig(id, base, limit, entry, arg);
      };
      env.__asyncjs__create_suspended_fiber_js = OS ? new OS(fn) : fn;
      console.log("[tap] create wrapped, susp=", !!OS);
    } catch (e) {
      console.log("[tap] wrap failed", e);
    }
  };

  const keep = (inst) => {
    try {
      const ex = inst.exports || inst;
      const out = { exports: ex, table: null };
      for (const k of Object.keys(ex)) if (ex[k] instanceof WebAssembly.Table) out.table = ex[k];
      window.__wc = out;
    } catch (e) {
      console.log("[tap] keep failed", e);
    }
  };

  const OI = WebAssembly.Instance;
  class TapInstance extends OI {
    constructor(m, i) {
      wrap(i);
      super(m, i);
      keep(this);
    }
  }
  WebAssembly.Instance = TapInstance;

  const oi = WebAssembly.instantiate;
  WebAssembly.instantiate = async function (buf, imp) {
    wrap(imp);
    const r = await oi.call(this, buf, imp);
    keep(r.instance || r);
    return r;
  };
})();
