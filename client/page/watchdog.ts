// A separate thread, so it keeps running when the engine blocks the main
// thread. If no frame arrives for a while, start a fresh client and exit this
// one. This is the "reload the join when it fails" safety net.
const msg = await new Promise<any>((r) => { (globalThis as any).onmessage = (e: any) => r(e.data); });
const { exe, args, timeout = 45000 } = msg;
const tries = Number(Deno.env.get("WC_RESTARTS") ?? 0);
let timer: ReturnType<typeof setTimeout> | undefined;
let spawned = false;

const restart = () => {
  if (spawned) return;
  spawned = true;
  console.error("[watchdog] no frame for", timeout, "ms - restarting");
  if (tries >= 3) {
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
    return;
  }
  Deno.kill(Deno.pid, "SIGKILL");
};

const arm = () => {
  clearTimeout(timer);
  timer = setTimeout(restart, timeout);
};
arm();
(globalThis as any).onmessage = () => arm();
setInterval(() => {}, 1 << 30);
