window.__ModuleLoader__.load({
  id: "@openin/dsh-session-link",
  factory: (require) => {
    var module = { exports: {} };
    var exports = module.exports;

    const QUERY_KEY = "openinSession";
    const WAIT_TIMEOUT_MS = 10000;

    function clearDeepLink() {
      const url = new URL(window.location.href);
      url.searchParams.delete(QUERY_KEY);
      const suffix = `${url.pathname}${url.search}${url.hash}`;
      window.history.replaceState(null, "", suffix);
    }

    function apply(ctx) {
      ctx.effect(() => {
        const targetId = new URL(window.location.href).searchParams.get(QUERY_KEY);
        if (!targetId) return;

        let settled = false;
        let selecting = false;
        let unsubscribe = () => {};
        let timeout = 0;

        const finish = (clearUrl) => {
          if (settled) return;
          settled = true;
          unsubscribe();
          if (timeout) window.clearTimeout(timeout);
          if (clearUrl) clearDeepLink();
        };

        const reconcile = () => {
          if (settled) return;
          const snapshot = ctx.sessions.list.getSnapshot();
          if (snapshot.phase !== "ready") return;
          if (snapshot.byId[targetId] === void 0) return;
          if (selecting) return;
          selecting = true;
          try {
            ctx.sessions.open(targetId);
            finish(true);
          } catch (error) {
            console.warn("openin dsh session link failed:", error);
            finish(false);
          } finally {
            selecting = false;
          }
        };

        unsubscribe = ctx.sessions.list.subscribe(reconcile);
        timeout = window.setTimeout(() => {
          if (settled) return;
          console.warn(`openin dsh session link target not found: ${targetId}`);
          finish(true);
        }, WAIT_TIMEOUT_MS);
        reconcile();

        return () => {
          finish(false);
        };
      }, "openin: dsh session deep link");
    }

    exports.apply = apply;
    exports.inject = ["sessions"];
    return module.exports;
  }
});
