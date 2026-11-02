// worker-prelude.js — prepended to WebClient.js at stage time.
//
// The engine creates its workers by pointing them straight at WebClient.js
// ("Redirecting WebWorker to primary script"), so the worker picks up this file
// before any engine code. That is the only place rbx-net.js can be installed
// inside a worker: workers cannot reach the controlled frame, so they route
// their roblox.com calls through a BroadcastChannel to the page.
//
// A Trusted Types policy does not cross realms, and CSP ships
// require-trusted-types-for 'script' into the worker, so this realm needs its
// own before importScripts will accept a string.
;
(function () {
  if (typeof importScripts !== "function") return;
  if (typeof trustedTypes !== "undefined") {
    try {
      trustedTypes.createPolicy("default", { createScriptURL: (url) => url });
    } catch (e) {
      // A default policy already exists in this realm.
    }
  }
  try {
    importScripts(location.origin + "/rbx-net.js");
  } catch (e) {
    console.error("weblox prelude: " + e);
  }
})();
