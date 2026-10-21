// Offline test: no browser, device, credentials or network access.
const { readFileSync } = require('node:fs');
const { resolve } = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const html = readFileSync(resolve(__dirname, '../web/settings_wifi.html'), 'utf8');
const from = html.indexOf('let voiceTestBusy = false;');
const to = html.indexOf('setInterval(refreshVoiceTestStatus, 1000);', from);
assert(from > 0 && to > from);
const source = html.slice(from, to + 'setInterval(refreshVoiceTestStatus, 1000);'.length);
const nodes = new Map();
const $ = id => {
  if (!nodes.has(id)) nodes.set(id, { value: id === 'provider' ? '3' : '', disabled: false,
    addEventListener: (_event, handler) => { nodes.get(id).handler = handler; } });
  return nodes.get(id);
};
let confirmed = false, confirmations = 0, interval, statusGets = 0, fail = false;
const posts = [];
const context = vm.createContext({ $, JSON, Math, Error,
  window: { confirm(message) { confirmations++; assert(message.includes('付费')); assert(message.includes('60')); return confirmed; } },
  fetchJSON: async path => { assert.equal(path, '/api/voice-test'); statusGets++; return { phase: 'idle', reason: 'none', available: true, remaining_ms: 0, approval_id: 123 }; },
  fetch: async (path, request) => { assert.equal(path, '/api/voice-test'); posts.push(JSON.parse(request.body)); if (fail) throw Error('offline'); return { ok: true }; },
  setInterval: (fn, ms) => { interval = fn; assert.equal(ms, 1000); }
});
(async () => {
  vm.runInContext(source, context);
  assert.equal(posts.length, 0); // Page load cannot start.
  await interval(); await interval();
  assert.equal(statusGets, 2); assert.equal(posts.length, 0); // Polling is local GET only.
  await $('btn-voice-test-start').handler();
  assert.equal(confirmations, 1); assert.equal(posts.length, 0); // Declining cannot start.
  confirmed = true;
  await $('btn-voice-test-start').handler();
  assert.deepEqual(posts[0], { op: 'start', confirm_paid: true, max_rounds: 1, max_seconds: 60, approval_id: 123 });
  fail = true;
  await $('btn-voice-test-start').handler();
  assert.equal(posts.length, 2); // Failed POST isn't retried.
  assert($('voice-test-status').textContent.includes('不会自动重试'));
  await interval(); assert.equal(posts.length, 2);
  fail = false;
  await $('btn-voice-test-stop').handler();
  assert.deepEqual(posts[2], { op: 'stop' });
  assert.equal(confirmations, 3); // Stop never asks permission to stop.
  console.log('Paid voice UI: load/poll/decline/start/error/stop passed, zero network calls.');
})().catch(error => { console.error(error); process.exitCode = 1; });
