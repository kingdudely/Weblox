// Isolated-world shim: publish the module URL and inject the page-world hook.
(() => {
  const url = chrome.runtime.getURL("WebClient.patched.wasm");
  document.documentElement.dataset.rbxWasmUrl = url;
  const s = document.createElement("script");
  s.src = chrome.runtime.getURL("inject.js");
  s.onload = () => s.remove();
  (document.head || document.documentElement).appendChild(s);
})();
