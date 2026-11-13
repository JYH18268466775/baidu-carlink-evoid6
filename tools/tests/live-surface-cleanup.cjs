const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');

const bash = process.env.CARLINK_BASH || 'E:/Git/bin/bash.exe';
const wideTest = process.argv.includes('--wide');
const longTest = wideTest || process.argv.includes('--30s');
const unplug = longTest || process.argv.includes('--unplug');
const candidate = wideTest ? 'live-wide-candidate/tools' : longTest ? 'live-30s-candidate/tools' : unplug ? 'live-unplug-candidate' : 'live-cleanup-candidate';
const root = path.resolve(__dirname, '../../build', candidate);
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'carlink-cleanup-'));
const posix = dir.replaceAll('\\', '/');
const quote = s => "'" + s.replaceAll("'", "'\\''") + "'";

function run(args, options = {}) {
    const result = spawnSync(bash, args, {encoding: 'utf8', timeout: 8000, ...options});
    if (result.error) throw result.error;
    return result;
}

try {
    const display = fs.readFileSync(path.join(root, 'carlife_visible_first_frame.sh'), 'utf8');
    const launch = fs.readFileSync(path.join(root, 'carlife_usb_visible.sh'), 'utf8');
    for (const script of [display, launch]) assert.equal(run(['-n'], {input: script}).status, 0);
    fs.writeFileSync(path.join(dir, 'lm'), `#!/bin/sh
case "$*" in
  'get surface 1510')
    if [ "$CASE_NAME" = absent ] || [ -f "$TEST_DIR/destroyed" ]; then
      echo 'No surface with ID 1510 found'
      exit 0
    fi
    if [ "$CASE_NAME" = foreign ]; then echo 'created by pid: 43'; else echo 'created by pid: 42'; fi
    if [ "$CASE_NAME" = active ]; then echo 'native surface: 2115640'; else echo 'native surface: 4294967295'; fi
    if [ "$CASE_NAME" = attached ]; then echo 'on layer: 2147483648'; else echo 'on layer:'; fi ;;
  'get layer 0x80000000')
    echo 'layer 2147483648 (0x80000000)'
    if [ "$CASE_NAME" = changed ]; then echo 'surface render order: 1510(0x5e6),';
    else echo 'surface render order: 256(0x100),'; fi ;;
  'get surface 256') echo 'surface 256 (0x100)'; echo 'visibility: 1' ;;
  'set layer 0x80000000 render order 1510')
    : > "$TEST_DIR/selected"
    if [ "$CASE_NAME" = unplug ]; then rm -f "$CARLINK_USB_NODE"; fi
    if [ "$CASE_NAME" = stopped ]; then : > "$CARLINK_USB_STOPPED"; fi ;;
  'set layer 0x80000000 render order 0x100') echo restored >> "$TEST_DIR/restored" ;;
  'set surface 256 visibility 1'|'set surface 1510 '*) : ;;
  'destroy surface 1510')
    : > "$TEST_DIR/destroy-called"
    if [ "$CASE_NAME" != refused ]; then : > "$TEST_DIR/destroyed"; fi ;;
  *) exit 99 ;;
esac
`);
    assert.equal(run(['-c', 'chmod +x ' + quote(posix + '/lm')]).status, 0);
    const mock = display.replace('L=/opt/sys/bin/LayerManagerControl', 'L=' + quote(posix + '/lm'))
        .replaceAll('then sleep 1; fi', 'then :; fi');
    for (const [scenario, expected, shouldDestroy] of [
        ['owned', 0, true], ['absent', 0, false], ['foreign', 79, false],
        ['active', 79, false], ['attached', 79, false], ['changed', 79, false], ['refused', 79, true]
    ]) {
        for (const name of ['destroy-called', 'destroyed']) fs.rmSync(path.join(dir, name), {force: true});
        const result = run(['-s', '--', '--cleanup', '42'], {
            input: mock, env: {...process.env, TEST_DIR: posix, CASE_NAME: scenario}
        });
        assert.equal(result.status, expected, scenario + ': ' + result.stdout + result.stderr);
        assert.equal(fs.existsSync(path.join(dir, 'destroy-called')), shouldDestroy, scenario);
    }

    if (unplug) {
        const usb = path.join(dir, 'oap04');
        const stopped = path.join(dir, 'stopped');
        for (const scenario of ['unplug', 'stopped', 'absent_at_start', 'normal']) {
            for (const name of ['selected', 'restored', 'destroyed', 'polls', 'stopped', 'oap04', 'done'])
                fs.rmSync(path.join(dir, name), {force: true});
            if (scenario !== 'absent_at_start') fs.writeFileSync(usb, '');
            const result = run(['-s', '--', '--show-live'], {
                // Leave the real 20-second watchdog. The 8-second test
                // timeout verifies the launcher can cancel it early.
                input: mock.replace(/\bsleep 1\b/g, 'echo poll >> "$TEST_DIR/polls"'),
                env: {...process.env, TEST_DIR: posix, CASE_NAME: scenario,
                    CARLINK_USB_NODE: posix + '/oap04', CARLINK_USB_STOPPED: posix + '/stopped',
                    CARLINK_DISPLAY_DONE: posix + '/done'}
            });
            assert.equal(result.status, scenario === 'absent_at_start' ? 76 : 0, result.stdout + result.stderr);
            if (longTest) assert.equal(fs.existsSync(path.join(dir, 'done')), scenario === 'normal');
            if (scenario === 'absent_at_start') {
                assert.ok(!fs.existsSync(path.join(dir, 'selected')));
            } else {
                assert.equal(fs.readFileSync(path.join(dir, 'restored'), 'utf8').trim(), 'restored');
                const polls = path.join(dir, 'polls');
                if (scenario === 'normal') assert.equal(fs.readFileSync(polls, 'utf8').trim().split('\n').length, longTest ? 30 : 10);
                else {
                    assert.match(result.stdout, /restoring iDrive now/);
                    assert.ok(!fs.existsSync(polls));
                }
            }
        }
    }

    const start = launch.lastIndexOf('if [ -n "$replay" ]; then', launch.indexOf('echo "Starting live'));
    assert.ok(start >= 0);
    const pipeline = launch.slice(start, launch.indexOf('\numask 077', start));
    fs.writeFileSync(path.join(dir, 'capture'), '#!/bin/sh\nprintf "test USB stream"\nexit "$CAPTURE_STATUS"\n');
    fs.writeFileSync(path.join(dir, 'replay'), `#!/bin/sh
[ "$1" = --visible-stream ] || exit 98
cat > "$TEST_DIR/stream"
echo "$$" > "$TEST_DIR/pid"
if [ "$VERIFY_USB_ENV" = 1 ]; then
    [ "$CARLINK_USB_NODE" = /dev/oap04 ] && [ -n "$CARLINK_USB_STOPPED" ] || exit 96
    echo "$CARLINK_USB_STOPPED" > "$TEST_DIR/marker-path"
    if [ "$CAPTURE_STATUS" -ne 0 ]; then
        [ -f "$CARLINK_USB_STOPPED" ] || exit 96
    else
        [ ! -e "$CARLINK_USB_STOPPED" ] || exit 96
    fi
fi
exit "$REPLAY_STATUS"
`);
    fs.writeFileSync(path.join(dir, 'cleanup'), '#!/bin/sh\n[ "$1" = --cleanup ] && [ "$2" = "$(cat "$TEST_DIR/pid")" ] || exit 97\n: > "$TEST_DIR/cleanup-ran"\nexit "$CLEANUP_STATUS"\n');
    assert.equal(run(['-c', `chmod +x ${quote(posix + '/capture')} ${quote(posix + '/replay')}`]).status, 0);
    const pipelineTest = `probe=${quote(posix + '/capture')}\nreplay=${quote(posix + '/replay')}\noap=/dev/oap04\n` +
        pipeline.replace('/dev/shmem/carlife_visible_first_frame.sh', quote(posix + '/cleanup'))
            .replace('/dev/shmem/carlife-usb-', posix + '/carlife-usb-');
    for (const [replayStatus, cleanupStatus, expected] of [[0,0,0], [0,79,79], [65,79,65]]) {
        fs.rmSync(path.join(dir, 'cleanup-ran'), {force: true});
        const result = run(['-s'], {input: pipelineTest, env: {...process.env, TEST_DIR: posix,
            REPLAY_STATUS: String(replayStatus), CLEANUP_STATUS: String(cleanupStatus),
            VERIFY_USB_ENV: unplug ? '1' : '0', CAPTURE_STATUS: replayStatus === 0 ? '0' : '12'}});
        assert.equal(result.status, expected, result.stdout + result.stderr);
        assert.equal(fs.readFileSync(path.join(dir, 'stream'), 'utf8'), 'test USB stream');
        assert.ok(fs.existsSync(path.join(dir, 'cleanup-ran')));
        if (unplug) {
            const marker = fs.readFileSync(path.join(dir, 'marker-path'), 'utf8').trim();
            assert.ok(!fs.existsSync(marker), 'launcher removed the capture failure marker');
        }
    }
    if (longTest) {
        // A blocked producer must terminate on completion, rather than wait 60 seconds.
        fs.writeFileSync(path.join(dir, 'capture'), '#!/bin/sh\nexec sleep 60\n');
        fs.writeFileSync(path.join(dir, 'replay'), '#!/bin/sh\necho "$$" > "$TEST_DIR/pid"\n: > "$CARLINK_DISPLAY_DONE"\ncat >/dev/null\nexit 0\n');
        const result = run(['-s'], {input: pipelineTest, env: {...process.env, TEST_DIR: posix, CLEANUP_STATUS: '0'}});
        assert.equal(result.status, 0, result.stdout + result.stderr);
        assert.match(result.stderr, /USB capture stopped after completed display/);
        // A real capture error remains failure even if display completion races it.
        fs.writeFileSync(path.join(dir, 'capture'), '#!/bin/sh\nexit 12\n');
        fs.writeFileSync(path.join(dir, 'replay'), '#!/bin/sh\necho "$$" > "$TEST_DIR/pid"\ncat >/dev/null\n: > "$CARLINK_DISPLAY_DONE"\nexit 0\n');
        const failed = run(['-s'], {input: pipelineTest, env: {...process.env, TEST_DIR: posix, CLEANUP_STATUS: '0'}});
        assert.equal(failed.status, 65, failed.stdout + failed.stderr);
    }
    console.log('PASS: replay input and PID, guarded surface cleanup and exit status');
} finally {
    fs.rmSync(dir, {recursive: true, force: true});
}
