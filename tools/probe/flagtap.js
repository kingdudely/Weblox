(() => {
  try {
    const of = window.fetch;
    window.fetch = async function (u, o) {
      const r = await of(u, o);
      const s = String(u && u.url ? u.url : u);
      if (/\/v2\/settings\/application\/WebClientApp/.test(s)) {
        try {
          const j = await r.clone().json();
          j.applicationSettings = j.applicationSettings || {};
          j.applicationSettings.FFlagUseRbxTransportClient3 = "False";
          j.applicationSettings.FFlagUseRbxTransportServer = "False";
          j.applicationSettings.DFIntConnectionTransportBranchSelectionHundredthsPercentage = "0";
          console.log("[INJ] flags: RbxTransportClient3=False");
          return new Response(JSON.stringify(j), { status: 200, headers: { "content-type": "application/json" } });
        } catch (e) { console.log("[INJ] err " + e); }
      }
      return r;
    };
    console.log("[INJ] fetch hook installed");
  } catch (e) { console.log("[INJ] fatal " + e); }
})();
