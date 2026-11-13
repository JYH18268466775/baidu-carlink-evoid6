const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const project = path.resolve(__dirname, '../..');
const probe = path.join(project, 'build/obj/usb-capture-host.exe');
const reference = fs.readFileSync(path.join(project, 'samples/carlife-g9980-mdev-replay.bin'));
const configStart = 116;
const configEnd = configStart + reference.readUInt32LE(configStart - 4);
const videoSize = reference.readUInt32LE(configEnd + 20) - 8;
const idr = Buffer.concat([
    reference.subarray(configStart, configEnd),
    reference.subarray(configEnd + 32, configEnd + 32 + videoSize)
]);

function outer(channel, bytes) {
    const header = Buffer.alloc(8);
    header.writeUInt32BE(channel);
    header.writeUInt32BE(bytes.length, 4);
    return Buffer.concat([header, bytes]);
}

function cmd(type, body = []) {
    const header = Buffer.alloc(8);
    header.writeUInt16BE(body.length);
    header.writeUInt32BE(type, 4);
    return Buffer.concat([header, Buffer.from(body)]);
}

function run(scenario, chunks, status, messages) {
    const result = spawnSync(probe, [scenario], {
        input: Buffer.concat(chunks), timeout: 3000
    });
    if (result.error)
        throw result.error;
    const log = result.stderr.toString();
    assert.equal(result.status, status, log);
    for (const message of messages)
        assert.ok(log.includes(message), log);
    assert.deepEqual(result.stdout, status === 0 ? idr : Buffer.alloc(0));
    return log;
}

const match = cmd(0x10002, [8, 1]);
const init = outer(1, cmd(0x10008, [8, 0x80, 10, 16, 0xd0, 5, 24, 30]));
const videoHeader = Buffer.alloc(12);
videoHeader.writeUInt32BE(idr.length);
videoHeader.writeUInt32BE(0x20001, 8);
const video = outer(2, Buffer.concat([videoHeader, idr]));
const splitMatch = [outer(1, match.subarray(0, 8)), outer(4, Buffer.alloc(12)),
    outer(1, match.subarray(8))];

const success = run('delayed', [...splitMatch, init, video], 0, [
    'Phone encoder: 1280x720 @ 30 fps', 'Captured Annex B SPS + PPS + first IDR'
]);
assert.deepEqual([...success.matchAll(/Mock phone saw CMD 0x([0-9a-f]+)/g)]
    .map(match => parseInt(match[1], 16)),
[0x18001, 0x18027, 0x18003, 0x18007, 0x18028, 0x18009, 0x1801d]);

function transfer(bytes) {
    const length = Buffer.alloc(4);
    length.writeUInt32BE(bytes.length);
    return Buffer.concat([length, bytes]);
}
const wrappedMatch = outer(1, match);
const retryLog = run('usb-transfers', [wrappedMatch.subarray(0, 8),
    outer(1, cmd(0x1001b)).subarray(0, 8), cmd(0x1001b),
    outer(1, cmd(0x10004, Buffer.alloc(307))), outer(1, cmd(0x10026, Buffer.alloc(32))),
    wrappedMatch, init, video].map(transfer), 0,
    ['Missing AOA payload', 'retrying version request once', 'CarLife 1.0 accepted']);
assert.equal((retryLog.match(/Mock phone saw CMD 0x00018001/g) || []).length, 2);
const largeCommand = outer(1, cmd(0x10035, Buffer.alloc(26878, 0xa5)));
assert.equal(largeCommand.readUInt32BE(4), 26886);
run('usb-transfers', [wrappedMatch, init, largeCommand.subarray(0, 8),
    largeCommand.subarray(8, 8 + 16384), largeCommand.subarray(8 + 16384), video].map(transfer),
    0, ['CarLife CMD RX 0x00010035 (26878 bytes)', 'Captured Annex B SPS + PPS + first IDR']);
// Reproduce the observed shape, not the unknown contents of the real transfer:
// a channel-3 header declaring 20 bytes followed by a 3596-byte read.
const shortAudioEnvelope = outer(3, Buffer.alloc(20)).subarray(0, 8);
run('usb-transfers', [wrappedMatch, init, shortAudioEnvelope, Buffer.alloc(3596)].map(transfer),
    12, ['Invalid AOA header 00000000 00000000 at RX #4+20',
        'prior AOA channel=3 length=20', 'matches complete media record=0']);
const unwrappedMedia = Buffer.alloc(3596);
unwrappedMedia.writeUInt32BE(3584);
unwrappedMedia.writeUInt32BE(0x30006, 8);
run('usb-transfers', [wrappedMatch, init, shortAudioEnvelope, unwrappedMedia].map(transfer),
    12, ['prior AOA channel=3 length=20',
        'word0=00000e00 word2=00030006 matches complete media record=1']);
// A correctly framed audio record of the same size must not break video.
run('usb-transfers', [wrappedMatch, init, outer(3, Buffer.alloc(3596)), video].map(transfer),
    0, ['Captured Annex B SPS + PPS + first IDR']);
const authResult = cmd(0x1004b, [8, 1]);
run('usb-transfers', [wrappedMatch.subarray(0, 8), match, authResult, init, video].map(transfer),
    0, ['Complete CarLife CMD received without AOA header', 'CarLife CMD RX 0x0001004b',
        'Captured Annex B SPS + PPS + first IDR']);
run('usb-transfers', [wrappedMatch, authResult.subarray(0, 9)].map(transfer),
    12, ['Invalid AOA header 00020000 0001004b']);
run('usb-transfers', [wrappedMatch, cmd(0x1004b, [8, 0])].map(transfer),
    12, ['Phone rejected HU authentication: 0']);
const missingCommand = outer(1, cmd(0x10004, [8, 1, 16, 1])).subarray(0, 8);
const foreground = outer(1, cmd(0x1001b));
run('usb-transfers', [wrappedMatch, authResult, cmd(0x10051), missingCommand,
    foreground.subarray(0, 8), foreground.subarray(8), init.subarray(0, 8),
    init.subarray(8), video].map(transfer), 0,
    ['Missing AOA payload', 'CarLife CMD RX 0x0001001b', 'Phone encoder: 1280x720 @ 30 fps',
        'Captured Annex B SPS + PPS + first IDR']);
run('usb-transfers', [wrappedMatch, outer(1, cmd(0x08))].map(transfer), 12,
    ['Invalid CarLife inner CMD header: length=00000000 type=00000008']);
run('usb-transfers', [wrappedMatch, init, video.subarray(0, 8),
    video.subarray(0, 8), video.subarray(8)].map(transfer), 0,
    ['Missing AOA payload', 'Captured Annex B SPS + PPS + first IDR']);

run('version-reject', [outer(1, cmd(0x10002, [8, 2]))], 12,
    ['CarLife 1.0 version rejected or invalid: 2']);
run('encryption-required', [outer(1, match), outer(1, cmd(0x1006a))], 12,
    ['Phone requires CarLife encryption']);
run('disconnect-after-version', [], 12,
    ['HU_PROTOCOL_VERSION 1.0 sent', 'USB read failed, errno: 19']);
run('disconnect-during-init', [outer(1, match), init.subarray(0, 11)], 12,
    ['requesting 1280x720 @ 30 fps', 'USB read failed, errno: 19']);
run('disconnect-mid-idr', [outer(1, match), init, video.subarray(0, 100)], 12,
    ['waiting for SPS, PPS and IDR', 'USB read failed, errno: 19']);
run('invalid-envelope', [outer(1, match), Buffer.alloc(8)], 12,
    ['Invalid AOA header 00000000 00000000 at RX #', 'prior AOA channel=1 length=10']);
run('silent', [], 124, ['HU_PROTOCOL_VERSION 1.0 sent']);

const gop = fs.readFileSync(path.join(project, 'samples/carlife-mdev-first-gop.bin'));
const liveFrames = [];
for (let cursor = configEnd; cursor < gop.length;) {
    const length = gop.readUInt32LE(cursor + 20);
    const annexB = liveFrames.length === 0 ? idr : gop.subarray(cursor + 32, cursor + 24 + length);
    const header = Buffer.from(videoHeader);
    header.writeUInt32BE(annexB.length);
    liveFrames.push(outer(2, Buffer.concat([header, annexB])));
    cursor += 24 + length;
}
assert.equal(liveFrames.length, 20);
const live = spawnSync(probe, ['live'], { input: Buffer.concat([wrappedMatch, init, ...liveFrames]),
    timeout: 3000 });
if (live.error)
    throw live.error;
assert.equal(live.status, 0, live.stderr.toString());
assert.match(live.stderr.toString(), /Queued 20 live USB frames/);
assert.deepEqual(live.stdout, gop);

const recoveryTransfers = [wrappedMatch, init, liveFrames[0], liveFrames[1].subarray(0, 8),
    liveFrames[1].subarray(0, 8), liveFrames[1].subarray(8), liveFrames[0], ...liveFrames.slice(2)];
const recovered = spawnSync(probe, ['live-usb-transfers'],
    { input: Buffer.concat(recoveryTransfers.map(transfer)), timeout: 3000 });
if (recovered.error)
    throw recovered.error;
assert.equal(recovered.status, 0, recovered.stderr.toString());
assert.match(recovered.stderr.toString(), /Missing AOA payload/);
assert.match(recovered.stderr.toString(), /Recovered video at an IDR/);
const secondRecord = configEnd + 24 + gop.readUInt32LE(configEnd + 20);
assert.equal(recovered.stdout.readUInt32LE(secondRecord + 20), videoSize + 8);
assert.deepEqual(recovered.stdout.subarray(secondRecord + 32, secondRecord + 32 + videoSize),
    idr.subarray(configEnd - configStart));
const corruptVideo = outer(2, Buffer.alloc(51206, 0xfd));
corruptVideo.writeUInt32BE(0x3f22fdfd, 8);
corruptVideo.writeUInt32BE(0x80dbfe30, 16);
const corruptTransfers = [corruptVideo.subarray(0, 8)];
for (let offset = 8; offset < corruptVideo.length; offset += 16384)
    corruptTransfers.push(corruptVideo.subarray(offset, offset + 16384));
const afterCorruption = [...recoveryTransfers.slice(0, 4), ...corruptTransfers,
    shortAudioEnvelope, unwrappedMedia,
    ...recoveryTransfers.slice(4)];
const repaired = spawnSync(probe, ['live-usb-transfers'], {
    input: Buffer.concat(afterCorruption.map(transfer)), timeout: 3000
});
if (repaired.error) throw repaired.error;
assert.equal(repaired.status, 0, repaired.stderr.toString());
assert.match(repaired.stderr.toString(), /Discarding damaged video payload while waiting for IDR/);
assert.match(repaired.stderr.toString(), /Skipped complete CarLife media record with mismatched AOA length/);
assert.deepEqual(repaired.stdout, recovered.stdout);
console.log('PASS: USB handshake and failures, including 20 live records with a blocked pipe writer');
