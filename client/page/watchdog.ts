// Runs on its own thread: the engine can block the main one. Restart when a
// GPU device is lost (the renderer can no longer draw) or when no frame has
// arrived for too long. The deadline widens once gamejoin is posted, so a
// pre-join stall is recovered in ~18s instead of waiting 30s every time.
const msg = await new Promise<any>((r) => { (globalThis as any).onmessage = (e: any) => r(e.data); });
const { exe, args, timeout = 18000 } = msg;
const runTimeout = Math.max(timeout, 45000);
let tries = Number(Deno.env.get("WC_RESTARTS") ?? 0);
let timer: ReturnType<typeof setTimeout> | undefined;
let spawned = false;
let progressed = false;

const restart = (why: string) => {
  if (spawned) return;
  spawned = true;
  clearTimeout(timer);
  console.error("[watchdog]", why, "- restarting");
  if (tries >= 8) {
    console.error("[watchdog] restart limit reached");
    return;
  }
  try {
    new Deno.Command(exe, {
      args,
      env: { WC_RESTARTS: String(tries + 1) },
      stdout: Deno.env.get("WC_WATCHDOG_LOG") === "1" ? "inherit" : "null",
      stderr: Deno.env.get("WC_WATCHDOG_LOG") === "1" ? "inherit" : "null",
      detached: true,
    }).spawn();
  } catch (e) {
    console.error("[watchdog] spawn failed", e);
    spawned = false;
    return;
  }
  Deno.kill(Deno.pid, "SIGKILL");
};

const arm = (ms = progressed ? runTimeout : timeout) => {
  clearTimeout(timer);
  if (timeout <= 0) return; // WC_WATCHDOG_MS=0 disables all restarts
  timer = setTimeout(() => restart(`no frame for ${ms} ms`), ms);
};
arm();

(globalThis as any).onmessage = (e: any) => {
  const d = e.data;
  if (d?.lost) return timeout > 0 ? restart("GPU device lost") : undefined;
  if (d?.progress) {
    progressed = true;
    tries = 0;
    arm();
    return;
  }
  if (d?.frame) {
    progressed = true;
    tries = 0;
    arm();
  }
};
setInterval(() => {}, 1 << 30);
