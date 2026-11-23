// --pre-js for browser games: keeps saves and user data in IndexedDB (IDBFS) across page reloads.
// Mounted and restored before main() so the game's first save scan sees them; flushed to IndexedDB
// every second and when the page is hidden. A failed sync is logged and the session keeps playing.
var Module = typeof Module != 'undefined' ? Module : {};
Module.preRun = (Module.preRun || []).concat(function () {
  // Development builds save into the project (/game/saves); user_data_path is $HOME/.local/share/...
  var dirs = ['/game/saves', '/home/web_user/.local/share'];
  dirs.forEach(function (d) {
    FS.mkdirTree(d);
    FS.mount(IDBFS, {}, d);
  });
  addRunDependency('iyi-persist');
  FS.syncfs(true, function (err) {
    if (err) console.error('[persist] restore failed:', err);
    removeRunDependency('iyi-persist');
  });
  var busy = false;
  function flush() {
    if (busy) return;
    busy = true;
    FS.syncfs(false, function (err) {
      busy = false;
      if (err) console.error('[persist] save failed:', err);
    });
  }
  setInterval(flush, 1000);
  addEventListener('pagehide', flush);
  addEventListener('visibilitychange', function () { if (document.hidden) flush(); });
  Module.iyiFlush = flush;
});
