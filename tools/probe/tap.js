(() => {
  try {
    if (globalThis.__tapped) return; globalThis.__tapped = true;
    const P = globalThis.WebAssembly;
    const state = { mem: null, dat: 0, str: 0, opens: 0, ring: null, rx: 0, inst: 0 };
    function hex(ptr, len, max) {
      try {
        const u = new Uint8Array(state.mem.buffer, ptr, Math.min(len, max || 40));
        let s = "";
        for (let i = 0; i < u.length; i++) s += (u[i] < 16 ? "0" : "") + u[i].toString(16);
        return s;
      } catch (e) { return "ERR"; }
    }
    function tapInstance(ex) {
      try {
        if (!ex || ex.__tappedEx) return ex; ex.__tappedEx = 1; state.inst++;
        const f = ex.rbx_wasm_rna_on_receive_ring_slot;
        if (f) {
          ex.rbx_wasm_rna_on_receive_ring_slot = function (id, kind, sid, slot, len) {
            if (state.rx < 150) {
              state.rx++;
              const r = state.ring;
              const ptr = r ? r.dataPtr + ((slot >>> 0) % r.slotCount) * r.slotSize : 0;
              console.log("[TAP in] kind=" + kind + " sid=" + sid + " len=" + len + " hex=" + hex(ptr, len, 40));
            }
            return f.apply(null, arguments);
          };
        }
        const o = ex.rbx_wasm_rna_on_stream_accepted;
        if (o) ex.rbx_wasm_rna_on_stream_accepted = function (id, sid) { if (state.opens < 40) { state.opens++; console.log("[TAP acc] sid=" + sid); } return o.apply(null, arguments); };
        const cs = ex.rbx_wasm_rna_on_connect_success;
        if (cs) ex.rbx_wasm_rna_on_connect_success = function (id, mtu) { console.log("[TAP ok] mtu=" + mtu); return cs.apply(null, arguments); };
        const ce = ex.rbx_wasm_rna_on_connect_error;
        if (ce) ex.rbx_wasm_rna_on_connect_error = function (id) { console.log("[TAP connfail]"); return ce.apply(null, arguments); };
        const cc = ex.rbx_wasm_rna_on_connection_closed;
        if (cc) ex.rbx_wasm_rna_on_connection_closed = function (id, code, c2, c3) { console.log("[TAP closed] code=" + code); return cc.apply(null, arguments); };
        try {
          const tb = ex.__indirect_function_table;
          if (tb) console.log("[TAP table] len=" + tb.length + " get1=" + typeof tb.get(1) + " name=" + String(tb.get(1) && tb.get(1).name));
        } catch (e) { console.log("[TAP table] err " + e); }
        try {
          const NEEDLES = ["/etc/hosts", "/etc/resolv.conf", "getaddrinfo(...) failed", "Assigned peer id = %u", "rbxasset://", "FLog::Network"];
          const mem = state.mem;
          if (mem && !state.scanned) {
            state.scanned = 1;
            const u8 = new Uint8Array(mem.buffer);
            const lim = Math.min(u8.length, 512 * 1024 * 1024);
            for (const n of NEEDLES) {
              const nd = new TextEncoder().encode(n);
              let found = -1;
              outer: for (let i = 0; i + nd.length < lim; i++) {
                if (u8[i] !== nd[0]) continue;
                for (let k = 1; k < nd.length; k++) if (u8[i + k] !== nd[k]) continue outer;
                found = i; break;
              }
              console.log("[TAP addr] " + JSON.stringify(n) + " -> " + (found < 0 ? "not-found" : "0x" + found.toString(16)));
            }
          }
        } catch (e) { console.log("[TAP addr] err " + e); }
        console.log("[TAP] exports hooked #" + state.inst);
        return ex;
      } catch (e) { console.log("[TAP] export wrap err " + e); return ex; }
    }
    function wrap(imports) {
      try {
        const env = imports && imports.env; if (!env) return imports;
        state.mem = env.memory || state.mem;
        if (env.rbx_wasm_rna_connect_js && !env.__w_connect) {
          const f = env.rbx_wasm_rna_connect_js;
          env.rbx_wasm_rna_connect_js = function (id, url, hp, hc, dataPtr, headPtr, tailPtr, slotSize, slotCount) {
            try {
              state.ring = { dataPtr: dataPtr >>> 0, headPtr: headPtr >>> 0, tailPtr: tailPtr >>> 0, slotSize: slotSize >>> 0, slotCount: slotCount >>> 0 };
              console.log("[TAP conn] ring data=" + dataPtr + " slotSize=" + slotSize + " slots=" + slotCount);
              const ADDRS = [0x2b14c4, 0x2b2373, 0x3366, 0xe913, 0x633c4];
              for (const a of ADDRS) {
                try {
                  const u = new Uint8Array(state.mem.buffer, a, 40);
                  let t = ""; for (let i = 0; i < u.length; i++) { const c = u[i]; t += (c >= 32 && c < 127) ? String.fromCharCode(c) : "."; }
                  console.log("[TAP mem] 0x" + a.toString(16) + " | " + t);
                } catch (e) { console.log("[TAP mem] 0x" + a.toString(16) + " ERR"); }
              }
            } catch (e) {}
            return f.apply(null, arguments);
          };
          env.__w_connect = 1;
        }
        if (env.rbx_wasm_rna_send_datagram_js && !env.__w_dat) {
          const f = env.rbx_wasm_rna_send_datagram_js;
          env.rbx_wasm_rna_send_datagram_js = function (id, ptr, len, hi, lo) {
            if (state.dat < 12) { state.dat++; console.log("[TAP tx-dat] len=" + len + " hi=" + hi + " lo=" + lo + " hex=" + hex(ptr, len, 40)); }
            return f.apply(null, arguments);
          };
          env.__w_dat = 1;
        }
        if (env.rbx_wasm_rna_write_stream_js && !env.__w_str) {
          const f = env.rbx_wasm_rna_write_stream_js;
          env.rbx_wasm_rna_write_stream_js = function (id, sid, ptr, len) {
            if (state.str < 25) { state.str++; console.log("[TAP tx-str] sid=" + sid + " len=" + len + " hex=" + hex(ptr, len, 32)); }
            return f.apply(null, arguments);
          };
          env.__w_str = 1;
        }
        if (env.rbx_wasm_rna_open_stream_js && !env.__w_open) {
          const f = env.rbx_wasm_rna_open_stream_js;
          env.rbx_wasm_rna_open_stream_js = function (id, sid) { if (state.opens < 40) { state.opens++; console.log("[TAP open] sid=" + sid); } return f.apply(null, arguments); };
          env.__w_open = 1;
        }
      } catch (e) { console.log("[TAP] wrap error " + e); }
      return imports;
    }
    const origStore = Atomics.store, origAdd = Atomics.add;
    function bump(name, idx, nv) {
      const r = state.ring;
      if (!r || !state.mem) return;
      if (((idx << 2) >>> 0) !== r.headPtr) return;
      if (state.rx >= 400) return;
      const head = nv >>> 0;
      const slot = (head - 1 + r.slotCount) % r.slotCount;
      const ptr = r.dataPtr + slot * r.slotSize;
      let v = 0, n = 1, ok = false;
      try {
        const b = new Uint8Array(state.mem.buffer, ptr, 8);
        n = 1 << (b[0] >> 6);
        if (n <= 8) { v = b[0] & 0x3f; for (let i = 1; i < n; i++) v = (v << 8) | b[i]; ok = true; }
        var t = (v < r.slotSize) ? b[n] : -1;
      } catch (e) { return; }
      const interesting = ok && (t === 0x83 || t === 0x97 || t === 0x9b || t === 0x81 || t === 0x84 || t === 0x8d || t === 0x93 || v > 900);
      if (!interesting) return;
      state.rx++;
      console.log("[TAP in] head=" + head + " len=" + v + " type=" + (t < 0 ? "?" : t.toString(16)) + " hex=" + hex(ptr, 40, 40));
    }
    Atomics.store = function (a, i, v) { const r = origStore.apply(Atomics, arguments); bump("store", i, v); return r; };
    Atomics.add = function (a, i, v) { const r = origAdd.apply(Atomics, arguments); bump("add", i, r + v); return r; };
    const origS = P.instantiateStreaming, origI = P.instantiate, origInst = P.Instance;
    P.instantiateStreaming = function (m, i) { return origS.call(P, m, wrap(i)).then((r) => (tapInstance(r.instance), r)); };
    P.instantiate = function (m, i) { return origI.call(P, m, wrap(i)).then((r) => (tapInstance(r.instance), r)); };
    P.Instance = function (m, i) { return tapInstance(new origInst(m, wrap(i))); };
    console.log("[TAP] installed");
  } catch (e) { console.log("[TAP] fatal " + e); }
})();
