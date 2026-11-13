const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const project = path.resolve(__dirname, '../..');
const probe = process.argv[2] || path.join(project, 'build/obj/usb-capture-host.exe');
const reference = fs.readFileSync(path.join(project, 'samples/carlife-g9980-mdev-replay.bin'));
const expected = [reference.subarray(0, 8)];
const transfers = [];
const frameLimit = Number(process.argv[3] || 300);
assert.ok(frameLimit === 300 || frameLimit === 900);

function transfer(bytes) {
    const header = Buffer.alloc(4);
    header.writeUInt32BE(bytes.length);
    transfers.push(header, bytes);
}

function envelope(channel, inner) {
    const header = Buffer.alloc(8);
    header.writeUInt32BE(channel);
    header.writeUInt32BE(inner.length, 4);
    transfer(header);
    for (let offset = 0; offset < inner.length; offset += 16384)
        transfer(inner.subarray(offset, offset + 16384));
}

function command(type, body) {
    const header = Buffer.alloc(8);
    header.writeUInt16BE(body.length);
    header.writeUInt32BE(type, 4);
    envelope(1, Buffer.concat([header, Buffer.from(body)]));
}

command(0x10002, [8, 1]);
command(0x10008, [8, 0x80, 10, 16, 0xd0, 5, 24, 30]);
let parameterSets;
const sampleFrames = [];
for (let offset = 8; offset < reference.length && sampleFrames.length < 300;) {
    const kind = reference.readUInt32LE(offset + 16);
    const length = reference.readUInt32LE(offset + 20);
    const record = Buffer.from(reference.subarray(offset, offset + 24 + length));
    offset += record.length;
    if (kind === 4) parameterSets = record.subarray(24);
    if (kind === 3) sampleFrames.push(record);
    else expected.push(record);
}
assert.equal(sampleFrames.length, 300);

for (let frames = 0; frames < frameLimit; frames++) {
    const record = Buffer.from(sampleFrames[frames % sampleFrames.length]);
    const picture = record.subarray(32);
    const annexB = frames === 0 ? Buffer.concat([parameterSets, picture]) : picture;
    const header = Buffer.alloc(12);
    header.writeUInt32BE(annexB.length);
    header.writeUInt32BE(0x20001, 8);
    envelope(2, Buffer.concat([header, annexB]));
    record.writeUInt32LE(Math.floor(frames * 1000000 / 30), 24);
    record.writeUInt32LE(0, 28);
    expected.push(record);
}

const result = spawnSync(probe, ['live-visible-usb-transfers'], {
    input: Buffer.concat(transfers), timeout: 8000, maxBuffer: 16 * 1024 * 1024
});
if (result.error) throw result.error;
assert.equal(result.status, 0, result.stderr.toString());
assert.ok(result.stderr.toString().includes(`Queued ${frameLimit} live USB frames`));
if (frameLimit === 900) {
    const log = result.stderr.toString();
    assert.match(log, /USB video progress: 840 frames queued/);
    assert.match(log, /USB receive progress: 512 reads, \d+ frames queued, waiting for IDR=0/);
}
assert.deepEqual(result.stdout, Buffer.concat(expected));
console.log(`PASS: ${frameLimit}-frame visible USB capture matched the reference payloads and timestamps.`);
