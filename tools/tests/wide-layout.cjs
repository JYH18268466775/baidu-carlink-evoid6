const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const script = fs.readFileSync(path.join(root, 'build/live-wide-candidate/tools/carlife_visible_first_frame.sh'), 'utf8');
const start = script.indexOf('    vw=${');
const end = script.indexOf('    $L set surface 1510 opacity', start);
assert.ok(start > 0 && end > start);
const body = 'L=echo\n' + script.slice(start, end);
for (const [w,h,region] of [[1280,480,'0 0 1280 480'], [1280,720,'213 0 853 480'],
    [1920,1080,'213 0 853 480'], [720,1280,'505 0 270 480'], [1920,480,'0 80 1280 320']]) {
    const r = spawnSync('E:/Git/bin/bash.exe', ['-s'], {input: body, encoding: 'utf8',
        env: {...process.env, CARLINK_VIDEO_WIDTH: String(w), CARLINK_VIDEO_HEIGHT: String(h)}});
    assert.equal(r.status, 0, r.stderr);
    assert.ok(r.stdout.includes('destination region ' + region), r.stdout);
}
for (const w of ['0', '-1', '65536', 'bad']) {
    const r = spawnSync('E:/Git/bin/bash.exe', ['-s'], {input: body, encoding: 'utf8',
        env: {...process.env, CARLINK_VIDEO_WIDTH: w, CARLINK_VIDEO_HEIGHT: '480'}});
    assert.equal(r.status, 76);
}
console.log('PASS: native wide, 720p fallback, portrait, wider aspect and invalid sizes');
