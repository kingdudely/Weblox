// stub.js — engine worker bootstrap for the self-hosted client.
//
// Rewrites roblox.com API calls to our own origin (see proxy-hook.js), then
// loads the engine glue from our own /engine directory instead of the CDN.
(() => {
  const here = location.origin;
  importScripts(here + "/proxy-hook.js");
  importScripts(here + "/engine/WebClient.js");
})();
