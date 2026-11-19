// Play a Roblox place in a Chromium window.
//
// Reference/oracle host: it loads the official Web Play page and only patches
// the two things that page gets wrong for a public join (see extension/).
//
//   WC_COOKIE='_|WARNING...' ./run.sh <placeId>
//   WC_COOKIE_FILE=/path/to/cookie ./run.sh <placeId>
const { app, BrowserWindow, session } = require("electron");
const fs = require("node:fs");
const path = require("node:path");

const EXT = path.join(__dirname, "extension");
const DEBUG = process.env.WC_DEBUG === "1";

const place = (process.env.WC_PLACE || process.argv.slice(2).find((a) => /^\d+$/.test(a)) || "").trim();
if (!/^\d+$/.test(place)) {
  console.error("usage: WC_COOKIE='...' ./run.sh <placeId>");
  process.exit(1);
}

const cookie = (() => {
  if (process.env.WC_COOKIE) return process.env.WC_COOKIE.trim();
  const file = process.env.WC_COOKIE_FILE || "/home/john/king.txt";
  try {
    return fs.readFileSync(file, "utf8").trim();
  } catch {
    console.error(`no cookie: set WC_COOKIE or point WC_COOKIE_FILE at one (tried ${file})`);
    process.exit(1);
  }
})();
if (!cookie) {
  console.error("empty .ROBLOSECURITY value");
  process.exit(1);
}

app.commandLine.appendSwitch("enable-unsafe-webgpu");
app.commandLine.appendSwitch("enable-features", "Vulkan");
app.commandLine.appendSwitch("use-angle", "vulkan");
app.commandLine.appendSwitch("ignore-gpu-blocklist");
app.commandLine.appendSwitch("disable-features", "CalculateNativeWinOcclusion");
if (DEBUG) app.commandLine.appendSwitch("remote-debugging-port", "9222");

let win = null;
let onPlay = false;

app.whenReady().then(async () => {
  try {
  const sess = session.defaultSession;
  await sess.cookies.set({
    url: "https://www.roblox.com/",
    name: ".ROBLOSECURITY",
    value: cookie,
    domain: ".roblox.com",
    path: "/",
    secure: true,
    httpOnly: true,
    sameSite: "no_restriction",
  });
  const loaded = await sess.loadExtension(EXT, { allowFileAccess: true });
  console.log("extension", loaded && loaded.name);

  win = new BrowserWindow({
    width: 1280,
    height: 720,
    backgroundColor: "#0d0d14",
    title: "alcohol",
    autoHideMenuBar: true,
    webPreferences: { backgroundThrottling: false, spellcheck: false },
  });

  // the page's Leave navigates back to www.roblox.com: that ends the run
  win.webContents.on("did-start-navigation", (_e, url, _ip, isMain) => {
    if (!isMain) return;
    const host = new URL(url).hostname;
    if (host === "play.roblox.com") {
      onPlay = true;
      return;
    }
    if (onPlay && host.endsWith("roblox.com")) setTimeout(() => app.quit(), 120);
  });

  win.webContents.setWindowOpenHandler(({ url }) => {
    win.loadURL(url);
    return { action: "deny" };
  });

  win.on("closed", () => {
    win = null;
    app.quit();
  });

  const returnUrl = `https://www.roblox.com/games/${place}`;
  win.loadURL(
    `https://play.roblox.com/games/start?placeId=${place}` +
      `&returnUrl=${encodeURIComponent(returnUrl)}`,
  );
  } catch (e) {
    console.error("startup failed:", e);
    app.quit();
  }
});

app.on("window-all-closed", () => app.quit());
