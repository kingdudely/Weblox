// stub.js — worker bootstrap.
//
// The engine creates its workers by pointing them straight at WebClient.js, so
// this file is only a fallback for the paths that do ask for it. It rewrites
// roblox.com calls through our own origin (proxy-hook.js) and then loads the
// glue from /engine instead of the CDN.
(() => {
  const here = location.origin;
  importScripts(here + "/proxy-hook.js");
  importScripts(here + "/engine/WebClient.js");
})();
