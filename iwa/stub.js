// stub.js — worker bootstrap.
//
// The engine creates its workers by pointing them straight at WebClient.js, so
// this file is only a fallback for the paths that do ask for it. Route
// roblox.com through the frame, then load the glue from /engine, which the app
// server serves instead of the CDN.
(() => {
  importScripts(location.origin + "/rbx-net.js");
  importScripts(location.origin + "/engine/WebClient.js");
})();
