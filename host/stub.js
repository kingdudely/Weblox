// stub.js — engine worker bootstrap for the self-hosted client.
//
// Rewrites roblox.com API calls to our own origin (see proxy-hook.js), then
// loads the engine glue from our own /engine directory instead of the CDN.
(() => {
  // Same clamp as ring.js; the engine's glue is loaded here too.
  try {
    if (typeof AudioDestinationNode !== "undefined") {
      const dst = AudioDestinationNode.prototype;
      const node = typeof AudioNode !== "undefined" ? AudioNode.prototype : null;
      const count = node && Object.getOwnPropertyDescriptor(node, "channelCount");
      Object.defineProperty(dst, "maxChannelCount", { configurable: true, get: () => 2 });
      Object.defineProperty(dst, "channelCount", {
        configurable: true,
        get() { return count && count.get ? count.get.call(this) : 2; },
        set() {},
      });
    }
  } catch (e) {}

  const here = location.origin;
  importScripts(here + "/proxy-hook.js");
  importScripts(here + "/engine/WebClient.js");
})();
