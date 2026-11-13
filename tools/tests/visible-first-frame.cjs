const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const cp = require('node:child_process');
const assert = require('node:assert/strict');
const bash = process.env.CARLINK_BASH || 'E:/Git/bin/bash.exe';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'carlink-visible-'));
const posix = dir.replaceAll('\\', '/');
const quote = s => "'" + s.replaceAll("'", "'\\''") + "'";
try {
  const original = fs.readFileSync(path.join(__dirname, '../carlife_visible_first_frame.sh'), 'utf8');
  const syntax = cp.spawnSync(bash, ['-n'], {input: original, encoding:'utf8'});
  assert.equal(syntax.status, 0, syntax.stderr);
  fs.writeFileSync(path.join(dir, 'lm'), `#!/bin/sh
echo "$*" >> "$TEST_DIR/calls"
case "$*" in
 'get layer 0x80000000')
   echo 'layer 2147483648 (0x80000000)'
   if [ "$TEST_CASE" = changed ]; then echo 'surface render order: 1282(0x502),';
   else echo 'surface render order: 256(0x100),'; fi
   if [ "$TEST_CASE" = query_warning ]; then echo 'Failed to get available layers on screen'; exit 1; fi ;;
 'get surface 256') echo 'surface 256 (0x100)'; echo 'visibility: 1' ;;
 'get surface 1510')
   if [ "$TEST_CASE" = missing ]; then exit 1; fi
   echo 'native surface: 12345' ;;
 'set layer 0x80000000 render order 1510')
   if [ "$TEST_CASE" = failure ]; then exit 1; fi
   if [ "$TEST_CASE" = unplug ]; then : > "$TEST_DIR/carlife-usb-42.failed"; fi ;;
esac
exit 0
`);
  let script = original.replace('L=/opt/sys/bin/LayerManagerControl', 'L=' + quote(posix + '/lm'));
  script = script.replace('sh /dev/shmem/carlife_qnx_replay_guard.sh --check', 'true');
  script = script.replace('/dev/shmem/carlife-usb-', posix + '/carlife-usb-');
  // Keep the fallback asleep until the normal exit cancels it. Skip visible hold.
  script = script.replace('sleep 20', 'sleep 0.5').replace('sleep 5', ':')
    .replace(/\bsleep 1\b/g, 'echo poll >> "$TEST_DIR/calls"');
  fs.writeFileSync(path.join(dir, 'probe'), script);
  cp.execFileSync(bash, ['-c', 'chmod +x ' + quote(posix + '/lm')]);
  for (const [mode, option, expected] of [
    ['normal','--show-live',0], ['unplug','--show-live',0], ['unplug_before','--show-live',76],
    ['normal','--show',0], ['query_warning','--show',0], ['failure','--show',76],
    ['changed','--show',75], ['missing','--show',76],
    ['normal','--check',75], ['missing','--check',0]
  ]) {
    fs.writeFileSync(path.join(dir,'calls'), '');
    const marker = path.join(dir, 'carlife-usb-42.failed');
    fs.rmSync(marker, {force:true});
    if (mode === 'unplug_before') fs.writeFileSync(marker, '');
    const args = [posix + '/probe', option];
    if (option === '--show-live') args.push('42');
    const result = cp.spawnSync(bash, args, {
      env: {...process.env, TEST_DIR:posix, TEST_CASE:mode}, encoding:'utf8', timeout:5000
    });
    assert.equal(result.status, expected, mode + ': ' + result.stdout + result.stderr);
    const calls = fs.readFileSync(path.join(dir,'calls'),'utf8');
    if (option === '--show-live' && mode === 'normal') assert.equal((calls.match(/^poll$/gm) || []).length, 10);
    if (mode === 'unplug') {
      assert.doesNotMatch(calls, /^poll$/m);
      assert.match(calls, /render order 1510\r?\nset layer 0x80000000 render order 0x100/);
    }
    if (mode === 'unplug') assert.match(result.stdout, /USB capture stopped; restoring iDrive now/);
    if (mode === 'unplug_before') assert.match(result.stdout, /USB capture stopped before display switch/);
    if (option.startsWith('--show') && ['normal','query_warning','failure','unplug'].includes(mode)) {
      assert.match(calls, /set layer 0x80000000 render order 0x100/);
      assert.match(calls, /set surface 256 visibility 1/);
    } else assert.doesNotMatch(calls, /^set /m);
  }
  console.log('PASS: shell syntax, normal restoration, failed switch restoration, changed baseline, missing/colliding surface');
} finally { fs.rmSync(dir, {recursive:true, force:true}); }
