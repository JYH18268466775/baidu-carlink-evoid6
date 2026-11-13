// Run after tools\build-qnx-probes.ps1 in this candidate: node tools\check-build.cjs [BASELINE_BUILD_DIR]
//
// 1. The two ELFs whose sources are unchanged from the car-verified 30 s candidate must
//    rebuild byte for byte. If they do not, this toolchain is not the one that produced the
//    verified files, and the new capture ELF must not be trusted either.
// 2. The capture ELF must differ from the verified one (the new source was really compiled).
// 3. All three files must be complete ARM ELF executables, not truncated uploads.
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const candidate = path.resolve(__dirname, '..', 'build');
const baseline = path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'live-30s-candidate', 'build'));
const unchanged = ['carlife_qnx_usb_status.elf'];
// The wide display change (1280x480 request, negotiated size passed to the display script) changed the replay
// source, so its reference is the wide candidate's build, which this toolchain produced. Status is unchanged.
const wideBaseline = path.join(__dirname, '..', '..', 'live-wide-candidate', 'build');
const baselineDir = name => (!process.argv[2] && name === 'carlife_qnx_mmr_replay.elf') ? wideBaseline : baseline;
const changed = 'carlife_qnx_usb_capture.elf';
// Continuous-session support changed the replay source again (no frame cap), so it must differ from the wide build.
const alsoChanged = ['carlife_qnx_mmr_replay.elf'];
// New programs without a verified baseline: only checked for being complete ARM executables here.
const added = ['carlife_qnx_most_watch.elf', 'carlife_qnx_ctl.elf', 'carlife_qnx_cp_purge.elf'];

const sha = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex').toUpperCase();
let failed = false;
function report(ok, message) {
    console.log(`${ok ? 'PASS' : 'FAIL'}: ${message}`);
    if (!ok) failed = true;
}

function isArmElf(file) {
    const data = fs.readFileSync(file);
    return data.length > 1000 && data.readUInt32BE(0) === 0x7f454c46 && data[4] === 1 && data[5] === 1 &&
        data.readUInt16LE(18) === 0x28 && data.readUInt16LE(16) === 2;  // 32-bit LE, EM_ARM, ET_EXEC
}

for (const name of [...unchanged, changed, ...alsoChanged]) {
    for (const [label, dir] of [['candidate', candidate], ['baseline', baselineDir(name)]]) {
        const file = path.join(dir, name);
        if (!fs.existsSync(file)) {
            report(false, `${label} ${name} is missing (${file})`);
            continue;
        }
        report(isArmElf(file), `${label} ${name} is a complete ARM ELF executable (${fs.statSync(file).size} bytes)`);
    }
}
for (const name of added) {
    const file = path.join(candidate, name);
    if (!fs.existsSync(file)) { report(false, `candidate ${name} is missing (${file})`); continue; }
    report(isArmElf(file), `candidate ${name} is a complete ARM ELF executable (${fs.statSync(file).size} bytes)`);
}
if (!failed) {
    for (const name of alsoChanged)
        report(sha(path.join(candidate, name)) !== sha(path.join(baselineDir(name), name)),
               `${name} differs from the wide-candidate ELF (continuous-session source compiled)`);
    for (const name of [...alsoChanged, ...added])
        console.log(`${name} SHA-256: ${sha(path.join(candidate, name))}`);
    for (const name of unchanged)
        report(sha(path.join(candidate, name)) === sha(path.join(baselineDir(name), name)),
               `${name} rebuilds identically to the verified file`);
    const rebuilt = sha(path.join(candidate, changed));
    report(rebuilt !== sha(path.join(baseline, changed)), `${changed} differs from the verified ELF (new source compiled)`);
    console.log(`\n${changed} SHA-256: ${rebuilt}`);
    console.log('Record this value in the README before uploading.');
}
if (failed) {
    console.log('\nDo not upload. Rebuild with the same LLVM 15 toolchain as the verified candidates.');
    process.exit(1);
}
console.log('\nBuild check passed.');
