// Exercise the actual native-to-JavaScript export bridge without WebView2.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const root = path.join(__dirname, '..');
const native = fs.readFileSync(path.join(root, 'markedit.c'), 'utf8');
const exportCode = native.slice(native.indexOf('static void do_export_html(void) {'));
const formatMatch = exportCode.match(/snprintf\(script, scriptCap,([\s\S]*?)baseEscaped, titleEscaped, g_exportRequest, g_exportRequest\);/);
assert.ok(formatMatch, 'could not locate the export bridge snprintf() in markedit.c - update this test to match');
const literals = formatMatch[1].match(/"(?:\\.|[^"\\])*"/g);
assert.ok(literals && literals.length, 'no string literals found in the export bridge format');
let bridge = literals.map(s => JSON.parse(s)).join('');
bridge = bridge.replace('%s', 'file:///example/').replace('%s', 'Title').replaceAll('%lu', '42');
async function run() {
    let resolve;
    const messages = [];
    const pending = new Promise(done => { resolve = done; });
    const context = {window: {exportRenderedHTML: () => pending}, chrome: {webview: {postMessage: s => messages.push(s)}}};
    assert.equal(vm.runInNewContext(bridge, context), true);
    await new Promise(setImmediate);
    assert.deepEqual(messages, []);
    resolve('<html>complete</html>');
    await new Promise(setImmediate);
    assert.deepEqual(messages, ['export-html:42:"<html>complete</html>"']);
    messages.length = 0;
    context.window.exportRenderedHTML = () => { throw Error('failed rendering'); };
    assert.equal(vm.runInNewContext(bridge, context), true);
    await new Promise(setImmediate);
    assert.deepEqual(messages, ['export-html:42:null']);
    assert.equal(vm.runInNewContext(bridge, {window: {}}), false);
    // Syntax-check all inline preview scripts, including the embedded fallback.
    const html = fs.readFileSync(path.join(root, 'preview_shell.html'), 'utf8');
    for (const match of html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)) new vm.Script(match[1]);
    console.log('All preview bridge tests passed.');
}
run().catch(error => { console.error(error); process.exitCode = 1; });
