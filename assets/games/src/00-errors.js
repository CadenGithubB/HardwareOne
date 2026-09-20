
// Global error handler — catches syntax errors in the main script block
// and displays them visibly instead of a silent black screen.
window.onerror = function(msg, url, line, col) {
  var d = document.createElement('div');
  d.style.cssText = 'position:fixed;top:0;left:0;right:0;padding:16px;background:#220000;color:#ff4444;font:14px monospace;z-index:99999;white-space:pre-wrap';
  d.textContent = 'JS ERROR line ' + line + ':' + col + '\n' + msg;
  document.body.appendChild(d);
  console.error('[FATAL]', msg, 'line', line, 'col', col);
};
