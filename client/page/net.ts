// Network policy: engine files come from the CDN once, then from a mirror.
//
// The page chain is the engine's own bootstrap, so a new Roblox build only
// needs a new mirror. Everything build-specific (file names, fsindex, wasm,
// audio processor) is fetched by name from the resolved engine base; nothing
// about it is written down here.
import { UA } from "./env.ts";

const SETTINGS = "https://clientsettings.roblox.com/v2/client-version/WebClientApp";
const SETUP = "https://setup.rbxcdn.com/webclient/";
const PLAYER = "https://play.roblox.com";
const JOIN = "https://gamejoin.roblox.com";
const START = PLAYER + "/games/start";
// Where a build's page scripts (WasmLoader/WasmPlayer/stub) live. The hash is
// part of the deployment, so it is read off the start page and cached.
export const pageAssets = () => Deno.env.get("WC_PAGE_ASSETS") ?? "";
const JOIN_PATHS = new Set(["/v1/join-game", "/v1/join-game-instance", "/v1/join-reserved-game", "/v1/join-private-game"]);

export const mirrorDir = () => Deno.env.get("WC_MIRROR") ?? "mirror";
const refresh = () => Deno.env.get("WC_REFRESH") === "1";

/** Mirror file for a URL. Named by basename: engine names are unique. */
export function mirrorFile(url: string) {
  const name = (url.split("?")[0].split("/").pop() || "file").replace(/[^A-Za-z0-9._-]/g, "_");
  return `${mirrorDir()}/files/${name}`;
}

async function cached(url: string, load: () => Promise<Uint8Array>) {
  const file = mirrorFile(url);
  if (!refresh()) {
    try { return await Deno.readFile(file); } catch { /* miss */ }
  }
  const body = await load();
  await Deno.mkdir(`${mirrorDir()}/files`, { recursive: true });
  await Deno.writeFile(file, body);
  return body;
}

/**
 * The engine base for this build: `WC_ENGINE` if pinned, else the last one
 * resolved, else ask the version endpoint. `WC_REFRESH=1` always asks.
 */
export async function resolveEngine(): Promise<string> {
  const pinned = Deno.env.get("WC_ENGINE");
  if (pinned) return pinned.replace(/\/$/, "");
  const file = `${mirrorDir()}/engine.json`;
  if (!refresh()) {
    try {
      const { base, upload } = JSON.parse(await Deno.readTextFile(file));
      console.error("[net ] engine", upload);
      return base;
    } catch { /* resolve */ }
  }
  const r = await fetch(SETTINGS, { headers: { "user-agent": UA } });
  if (!r.ok) throw new Error(`client-version: HTTP ${r.status}`);
  const { clientVersionUpload } = await r.json();
  if (!clientVersionUpload) throw new Error("client-version: no upload");
  const base = `${SETUP}${clientVersionUpload}-RobloxWebClient`;
  await Deno.mkdir(mirrorDir(), { recursive: true });
  await Deno.writeTextFile(file, JSON.stringify({ base, upload: clientVersionUpload }));
  console.error("[net ] engine", clientVersionUpload);
  return base;
}

/** The deployment's page asset prefix, from the start page (or the cache). */
export async function resolvePage(): Promise<string> {
  const pinned = Deno.env.get("WC_PAGE_ASSETS");
  if (pinned) return pinned.replace(/\/$/, "");
  const file = `${mirrorDir()}/page.json`;
  if (!refresh()) {
    try { return JSON.parse(await Deno.readTextFile(file)).assets; } catch { /* resolve */ }
  }
  const r = await fetch(START, { headers: { "user-agent": UA } });
  const html = await r.text();
  const hash = /\/([0-9a-f]{20,})\/assets\//.exec(html)?.[1];
  if (!hash) throw new Error("start page: no asset hash");
  const assets = `${PLAYER}/${hash}/assets/webclient`;
  await Deno.mkdir(mirrorDir(), { recursive: true });
  await Deno.writeTextFile(file, JSON.stringify({ hash, assets }));
  console.error("[net ] page", hash);
  return assets;
}

/** The page's own files (WasmLoader, WasmPlayer, stub) live under the asset
 *  prefix; the engine files live under the engine base. */
const PAGE_FILES = /^\/(WasmLoader\.js|WasmPlayer\.js|stub\.js)$/;

export function canonical(url: string): string {
  const base = Deno.env.get("WC_ENGINE") ?? "";
  try {
    const u = new URL(url, PLAYER);
    if (u.origin === PLAYER) {
      const name = u.pathname.split("/").pop() ?? "";
      if (PAGE_FILES.test("/" + name) || /\/assets\/webclient\//.test(u.pathname)) return `${pageAssets()}/${name}`;
      return base + u.pathname;
    }
    return u.href;
  } catch {
    return url;
  }
}

const TEXT = /\.(js|json|txt|html|xml|css)$/i;

export function installFetch() {
  const real = globalThis.fetch;
  const pin = Deno.env.get("WC_BUILD_VERSION") ?? "0.748.0.7481526";
  const upload = () => (Deno.env.get("WC_ENGINE") ?? "").split("/webclient/")[1]?.replace(/-RobloxWebClient$/, "") ?? "";
  (globalThis as any).fetch = async (input: any, init: any = {}) => {
    let url = canonical(typeof input === "string" ? input : input.url);
    // The CDN rejects Deno's UA outright.
    if (/^https:\/\/([a-z0-9-]+\.)*(roblox\.com|rbxcdn\.com|robloxlabs\.com)\//.test(url)) {
      const h = new Headers(init.headers ?? {});
      if (!h.has("user-agent")) h.set("user-agent", UA);
      init = { ...init, headers: h };
    }
    // The launcher asks the settings service which build to run; answer with
    // the mirrored one so both halves agree.
    if (/clientsettings\.roblox\.com\/v2\/client-version\/WebClientApp/.test(url)) {
      return Response.json({ version: pin, clientVersionUpload: upload(), bootstrapperVersion: "" });
    }
    if (/\/v2\/settings\/application\//.test(url) && Deno.env.get("WC_GLES") === "1") {
      const r = await real(url, init);
      const j = await r.json();
      (j.applicationSettings ??= {}).FFlagWasmPreferWebGPU = "False";
      return Response.json(j);
    }
    if (/^https:\/\/(setup\.rbxcdn\.com|play\.roblox\.com)\//.test(url)) {
      const body = await cached(url, async () => {
        const r = await real(url, init);
        if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
        return new Uint8Array(await r.arrayBuffer());
      });
      return new Response(body as any, { headers: { "content-type": TEXT.test(url) ? "text/javascript" : "application/octet-stream" } });
    }
    return real(url, init);
  };
}

/** Load a page script the way the start page does. */
export async function loadScript(src: string) {
  const r = await fetch(src);
  if (!r.ok) throw new Error(`${src}: HTTP ${r.status}`);
  const body = await r.text();
  (0, eval)(body + `\n//# sourceURL=${src}`);
}

export { JOIN, JOIN_PATHS };
