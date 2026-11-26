// Optional main-loop segment timing, enabled only by WC_PERF=1.
//
// The engine's real loop is not requestAnimationFrame: WebClient.js's
// `schedule_main_loop` posts to a MessageChannel whose port1 handler awaits
// `_main_loop_iteration`, then posts the next tick. This module observes that
// turn boundary plus the host-side work around it (native present, capture,
// window input, WebTransport datagram reads). With the flag off it does nothing.

type Segment = "loop" | "raf" | "sleep" | "chan" | "present" | "capture" | "input" | "dgRead";

const SEGMENTS: Segment[] = ["loop", "raf", "sleep", "chan", "present", "capture", "input", "dgRead"];
const EDGES = [0.5, 1, 2, 4, 8, 16, 25, 50, 100, 250, 1000];
const PERIOD_MS = Number(Deno.env.get("WC_PERF_PERIOD_MS") ?? 5000);
const SLOW_MS = Number(Deno.env.get("WC_PERF_SLOW_MS") ?? 25);

type Turn = Record<Segment, number> & { n: number; engineTick: number; engineMs: number };

const fmt = (ms: number) => ms.toFixed(2);

export function maybePerf(): Perf | null {
  return Deno.env.get("WC_PERF") === "1" ? new Perf() : null;
}

export class Perf {
  private samples = new Map<Segment, number[]>(SEGMENTS.map((s) => [s, []]));
  private turns: Turn[] = [];
  private cur: Turn | null = null;
  private loopStart = 0;
  private tick = 0;
  private slowPrinted = 0;
  private period = Number.isFinite(PERIOD_MS) && PERIOD_MS > 0 ? PERIOD_MS : 5000;

  constructor() {
    console.error("[perf] WC_PERF=1", `period=${this.period}ms`, `slow>${SLOW_MS}ms`);
    setInterval(() => this.report(), this.period);
  }

  /** Time a small callback (input event, present, capture, datagram read...). */
  record(seg: Segment, ms: number): void {
    if (!Number.isFinite(ms)) return;
    this.samples.get(seg)!.push(ms);
    if (this.cur) this.cur[seg] += ms;
  }

  loopBegin(): void {
    if (this.cur) this.closeTurn();
    const t: Turn = {
      n: ++this.tick, loop: 0, raf: 0, sleep: 0, chan: 0,
      present: 0, capture: 0, input: 0, dgRead: 0, engineTick: 0, engineMs: 0,
    };
    this.cur = t;
    this.loopStart = performance.now();
  }

  loopEnd(ms: number): void {
    this.loopStart = 0;
    this.record("loop", ms);
  }

  /** The env.ts setTimeout(16) frame shim: overshoot is event-loop time spent
   *  outside the segments measured here (GC, worker scheduling, compositor). */
  raf(overshootMs: number, ms: number): void {
    this.record("sleep", overshootMs);
    this.record("raf", ms);
  }

  /** Correlate the engine's own "Tick N took X ms" with our segment breakdown. */
  engineTick(n: number, ms: number): void {
    if (!this.cur) return;
    this.cur.engineTick = n;
    this.cur.engineMs = ms;
    const soFar = this.loopStart ? performance.now() - this.loopStart : this.cur.loop;
    const t = this.cur;
    console.error(
      `[perf] engine tick #${n} took ${fmt(ms)}ms :: loopSoFar=${fmt(soFar)}` +
      ` present=${fmt(t.present)} capture=${fmt(t.capture)} input=${fmt(t.input)}` +
      ` dgWait=${fmt(t.dgRead)} raf=${fmt(t.raf)} chan=${fmt(t.chan)}`,
    );
  }

  private closeTurn(): void {
    const t = this.cur;
    if (!t) return;
    this.cur = null;
    this.turns.push(t);
    if (t.loop >= SLOW_MS && this.slowPrinted++ < 20) {
      console.error(
        `[perf] slow turn #${t.n} loop=${fmt(t.loop)}ms` +
        ` present=${fmt(t.present)} capture=${fmt(t.capture)}` +
        ` input=${fmt(t.input)} dgWait=${fmt(t.dgRead)} raf=${fmt(t.raf)}` +
        (t.engineTick ? ` engineTick=#${t.engineTick}/${fmt(t.engineMs)}ms` : ""),
      );
    }
  }

  private hist(a: number[]): string {
    const counts = new Array(EDGES.length + 1).fill(0) as number[];
    for (const v of a) {
      let i = 0;
      while (i < EDGES.length && v > EDGES[i]) i++;
      counts[i]++;
    }
    const labels = ["<0.5", ...EDGES.slice(0, -1).map((e, i) => `${e}-${EDGES[i + 1]}`), `>${EDGES[EDGES.length - 1]}`];
    return counts.map((c, i) => (c ? `${labels[i]}:${c}` : "")).filter(Boolean).join(" ");
  }

  private report(): void {
    this.closeTurn();
    console.error(`[perf] === ${this.period}ms window, turns=${this.tick} ===`);
    for (const seg of SEGMENTS) {
      const a = this.samples.get(seg)!;
      if (!a.length) {
        console.error(`[perf] ${seg.padEnd(7)} n=0`);
        continue;
      }
      const sorted = [...a].sort((x, y) => x - y);
      const q = (p: number) => fmt(sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))]);
      const sum = a.reduce((s, v) => s + v, 0);
      console.error(
        `[perf] ${seg.padEnd(7)} n=${a.length} avg=${fmt(sum / a.length)}` +
        ` p50=${q(0.5)} p95=${q(0.95)} p99=${q(0.99)} max=${fmt(sorted[sorted.length - 1])}` +
        ` [${this.hist(a)}]`,
      );
      a.length = 0;
    }
    const worst = this.turns.slice().sort((x, y) => y.loop - x.loop).slice(0, 3);
    for (const t of worst) {
      console.error(
        `[perf] worst #${t.n} loop=${fmt(t.loop)}ms present=${fmt(t.present)}` +
        ` capture=${fmt(t.capture)} input=${fmt(t.input)} dgWait=${fmt(t.dgRead)}` +
        ` raf=${fmt(t.raf)}` + (t.engineTick ? ` engineTick=#${t.engineTick}/${fmt(t.engineMs)}ms` : ""),
      );
    }
    this.turns.length = 0;
    this.slowPrinted = 0;
  }

  /** Wrap boundaries visible from main.ts. Call after env.install() and before
   *  the engine script runs. */
  wrapRuntime(host: any, surface: any, wtClass: any): void {
    const g = globalThis as any;

    // WebClient.js's schedule_main_loop uses a MessageChannel, not rAF.
    this.wrapMessageChannels();

    const origRAF = g.requestAnimationFrame;
    if (typeof origRAF === "function") {
      g.requestAnimationFrame = (f: any) => {
        const at = performance.now();
        const cb = (t: number) => {
          const overshoot = performance.now() - at - 16;
          const t0 = performance.now();
          try {
            return f(t);
          } finally {
            this.raf(Math.max(0, overshoot), performance.now() - t0);
          }
        };
        return origRAF(cb);
      };
    }

    if (surface && typeof surface.present === "function") {
      const present = surface.present.bind(surface);
      try {
        surface.present = () => {
          const t0 = performance.now();
          try {
            return present();
          } finally {
            this.record("present", performance.now() - t0);
          }
        };
      } catch (e) {
        console.error("[perf] present wrap failed", String(e));
      }
    }

    let captureFn = host?.capture ?? null;
    try {
      Object.defineProperty(host, "capture", {
        configurable: true,
        get: () => captureFn,
        set: (v: any) => {
          if (typeof v !== "function") {
            captureFn = v;
            return;
          }
          captureFn = (...args: any[]) => {
            const t0 = performance.now();
            try {
              return v(...args);
            } finally {
              this.record("capture", performance.now() - t0);
            }
          };
        },
      });
    } catch (e) {
      console.error("[perf] capture wrap failed", String(e));
    }

    // draftwt.ts exposes datagrams through a fresh object per getter access;
    // wrap the reader's read() without changing the transport module itself.
    const self = this;
    const desc = wtClass?.prototype && Object.getOwnPropertyDescriptor(wtClass.prototype, "datagrams");
    if (desc?.get) {
      try {
        Object.defineProperty(wtClass.prototype, "datagrams", {
          configurable: true,
          get(this: any) {
            const dg = desc.get!.call(this);
            const readable = dg?.readable;
            const origReader = readable?.getReader;
            if (typeof origReader === "function") {
              readable.getReader = () => {
                const r = origReader.call(readable);
                const origRead = r?.read;
                if (typeof origRead === "function") {
                  r.read = async () => {
                    const t0 = performance.now();
                    try {
                      return await origRead.call(r);
                    } finally {
                      self.record("dgRead", performance.now() - t0);
                    }
                  };
                }
                return r;
              };
            }
            return dg;
          },
        });
      } catch (e) {
        console.error("[perf] datagram wrap failed", String(e));
      }
    }

    // The engine logs slow ticks through console.log with a single string.
    for (const method of ["log", "info"] as const) {
      const orig = console[method].bind(console);
      (console as any)[method] = (...args: any[]) => {
        if (args.length === 1 && typeof args[0] === "string") {
          const m = /^Tick (\d+) took ([\d.]+) ms/.exec(args[0]);
          if (m) self.engineTick(Number(m[1]), Number(m[2]));
        }
        return orig(...args);
      };
    }
  }

  private wrapMessageChannels(): void {
    const Native = (globalThis as any).MessageChannel;
    if (typeof Native !== "function") return;
    const self = this;
    let count = 0;
    function PerfMessageChannel(this: any, ...args: any[]): any {
      const mc = new Native(...args);
      const port: any = mc.port1;
      const proto = Object.getPrototypeOf(port);
      const desc = Object.getOwnPropertyDescriptor(proto, "onmessage");
      const id = ++count;
      if (!desc?.set) return mc;
      const setter = desc.set as (this: any, v: any) => void;
      let wrapped: any = null;
      try {
        Object.defineProperty(port, "onmessage", {
          configurable: true,
          get: () => wrapped,
          set: (fn: any) => {
            wrapped = fn;
            if (typeof fn !== "function") {
              setter.call(port, fn);
              return;
            }
            const isMain = /_main_loop_iteration/.test(String(fn));
            const cb = async (...a: any[]) => {
              if (isMain) self.loopBegin();
              const t0 = performance.now();
              try {
                return await fn(...a);
              } finally {
                const ms = performance.now() - t0;
                if (isMain) self.loopEnd(ms);
                else self.record("chan", ms);
              }
            };
            setter.call(port, cb);
            if (id === 1 && !isMain) console.error("[perf] first MessageChannel is not the engine loop");
          },
        });
      } catch (e) {
        console.error("[perf] MessageChannel wrap failed", String(e));
      }
      return mc;
    }
    PerfMessageChannel.prototype = Native.prototype;
    Object.setPrototypeOf(PerfMessageChannel, Native);
    (globalThis as any).MessageChannel = PerfMessageChannel;
  }
}
