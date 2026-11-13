// Tests build/live-input-candidate/tools/check-build.cjs using the real verified ELFs as fixtures.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const project = path.resolve(__dirname, '../..');
const checker = path.join(project, 'build/live-input-candidate/tools/check-build.cjs');
const verified = path.join(project, 'build/live-30s-candidate/build');
const wide = path.join(project, 'build/live-wide-candidate/build');
const names = ['carlife_qnx_mmr_replay.elf', 'carlife_qnx_usb_status.elf', 'carlife_qnx_usb_capture.elf'];

function flipLastByte(file) {
    const data = fs.readFileSync(file);
    data[data.length - 1] ^= 0xff;
    fs.writeFileSync(file, data);
}

function scenario(label, mutate, expectedStatus, expectedText) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'carlink-check-'));
    try {
        const tools = path.join(dir, 'live-input-candidate/tools');
        const build = path.join(dir, 'live-input-candidate/build');
        const base = path.join(dir, 'live-30s-candidate/build');
        const wideBase = path.join(dir, 'live-wide-candidate/build');
        for (const d of [tools, build, base, wideBase]) fs.mkdirSync(d, { recursive: true });
        fs.copyFileSync(checker, path.join(tools, 'check-build.cjs'));
        for (const name of names) {
            fs.copyFileSync(path.join(verified, name), path.join(base, name));
            // replay is built from the wide-display source: its reference is the wide candidate's build
            const source = name === 'carlife_qnx_mmr_replay.elf' ? wide : verified;
            fs.copyFileSync(path.join(source, name), path.join(build, name));
            if (name === 'carlife_qnx_mmr_replay.elf') fs.copyFileSync(path.join(wide, name), path.join(wideBase, name));
        }
        for (const extra of ['carlife_qnx_most_watch.elf', 'carlife_qnx_ctl.elf', 'carlife_qnx_cp_purge.elf'])
            fs.copyFileSync(path.join(verified, 'carlife_qnx_usb_status.elf'), path.join(build, extra)); // any complete ARM ELF
        mutate(build);
        const result = spawnSync(process.execPath, [path.join(tools, 'check-build.cjs')], { encoding: 'utf8' });
        assert.equal(result.status, expectedStatus, `${label}\n${result.stdout}${result.stderr}`);
        assert.ok(result.stdout.includes(expectedText), `${label}: expected "${expectedText}"\n${result.stdout}`);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

const capture = build => path.join(build, 'carlife_qnx_usb_capture.elf');
const replay = build => path.join(build, 'carlife_qnx_mmr_replay.elf');

const status = build => path.join(build, 'carlife_qnx_usb_status.elf');
scenario('new capture and replay ELFs, status identical', build => { flipLastByte(capture(build)); flipLastByte(replay(build)); }, 0, 'Build check passed.');
scenario('toolchain mismatch on the status ELF', build => { flipLastByte(capture(build)); flipLastByte(replay(build)); flipLastByte(status(build)); },
         1, 'FAIL: carlife_qnx_usb_status.elf rebuilds identically');
scenario('replay ELF not rebuilt', build => flipLastByte(capture(build)), 1, 'FAIL: carlife_qnx_mmr_replay.elf differs from the wide-candidate ELF');
scenario('capture ELF not rebuilt', build => flipLastByte(replay(build)), 1, 'FAIL: carlife_qnx_usb_capture.elf differs from the verified ELF');
scenario('truncated capture ELF', build => fs.writeFileSync(capture(build), Buffer.alloc(0)), 1, 'FAIL: candidate carlife_qnx_usb_capture.elf is a complete ARM ELF');
scenario('missing replay ELF', build => fs.rmSync(replay(build)), 1, 'is missing');
scenario('missing ctl ELF', build => { flipLastByte(capture(build)); flipLastByte(replay(build)); fs.rmSync(path.join(build, 'carlife_qnx_ctl.elf')); }, 1, 'candidate carlife_qnx_ctl.elf is missing');
console.log('PASS: build check accepts a good rebuild and refuses toolchain mismatch, stale, truncated and missing files');
