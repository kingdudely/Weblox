// proxy-hook.js — route every roblox.com API call through our own origin.
//
// Loaded in the page and in the engine worker before any engine code runs. The
// page is served from 127.0.0.1, so every Roblox API host is cross-origin and
// either CORS-blocked (gamejoin is origin-locked to Roblox) or credentialed.
// Rewriting to /proxy/<host>/<path> makes all of it same-origin; the local
// server injects the cookie and the web-client identity on the way out.
(() => {
  if (self.__rbxProxyHook) return;
  self.__rbxProxyHook = 1;

  const HOSTS = new Set([
    "gamejoin.roblox.com", "assetgame.roblox.com", "www.roblox.com",
    "apis.roblox.com", "clientsettings.roblox.com", "clientsettingscdn.roblox.com",
    "assetdelivery.roblox.com", "users.roblox.com", "economy.roblox.com",
    "games.roblox.com", "thumbnails.roblox.com", "locale.roblox.com",
    "avatar.roblox.com", "accountsettings.roblox.com", "auth.roblox.com",
    "tracing.roblox.com", "ecsv2.roblox.com", "ephemeralcounters.api.roblox.com",
    "gameinternationalization.roblox.com", "develop.roblox.com", "groups.roblox.com",
    "presence.roblox.com", "voice.roblox.com", "chat.roblox.com",
    "friends.roblox.com", "badges.roblox.com", "inventory.roblox.com",
    "catalog.roblox.com", "accountinformation.roblox.com", "trades.roblox.com",
  ]);

  const origin = (typeof location !== "undefined" && location.origin) || "";

  function rewrite(u) {
    try {
      const abs = new URL(u, origin);
      if (HOSTS.has(abs.hostname)) {
        return origin + "/proxy/" + abs.hostname + abs.pathname + abs.search;
      }
    } catch (e) {}
    return null;
  }

  self.__rbxRewrites = 0;
  const origFetch = self.fetch;
  self.fetch = function (input, init) {
    const url = typeof input === "string" ? input : (input && input.url);
    const r = url && rewrite(url);
    if (r) self.__rbxRewrites++;
    return origFetch.call(self, r || input, init);
  };

  if (self.XMLHttpRequest) {
    const origOpen = self.XMLHttpRequest.prototype.open;
    self.XMLHttpRequest.prototype.open = function (method, url) {
      const r = rewrite(url);
      const args = Array.prototype.slice.call(arguments);
      if (r) args[1] = r;
      return origOpen.apply(this, args);
    };
  }

  self.__rbxProxyOrigin = origin;
})();
