// Reconstruct logged sizes and known bytes, not the unavailable real USB capture.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const exe = path.join(root, 'build/live-30s-candidate/build/obj/usb-capture-host.exe');
function envelope(channel, body) {
    const h = Buffer.alloc(8); h.writeUInt32BE(channel); h.writeUInt32BE(body.length, 4);
    return Buffer.concat([h, body]);
}
function command(type, body = Buffer.alloc(0)) {
    const h = Buffer.alloc(8); h.writeUInt16BE(body.length); h.writeUInt32BE(type, 4);
    return Buffer.concat([h, body]);
}
function run(chunks) {
    const input = Buffer.concat(chunks.flatMap(b => {
        const h = Buffer.alloc(4); h.writeUInt32BE(b.length); return [h, b];
    }));
    const r = spawnSync(exe, ['usb-transfers'], {input, timeout: 3000});
    if (r.error) throw r.error;
    return {status: r.status, log: r.stderr.toString(), output: r.stdout};
}
const match = envelope(1, command(0x10002, Buffer.from([8, 1])));
const body = Buffer.alloc(307);
Buffer.from('0a07416e64726f6964', 'hex').copy(body);
const info = command(0x10004, body); // Type inferred for the synthetic test only.
const staleHeader = envelope(1, command(0x1001b)).subarray(0, 8);
const broken = run([match.subarray(0, 8), staleHeader, info]);
assert.equal(broken.status, 12, broken.log);
for (const s of ['Missing AOA payload at RX #2: channel=1 remaining=10',
    'Invalid AOA header 0a07416e 64726f69 at RX #3+8',
    'prior AOA channel=1 length=8; inner pending cmd=8 video=0',
    'word0=01330000']) assert.ok(broken.log.includes(s), broken.log);
assert.equal(broken.output.length, 0);
console.log('PASS: reproduced logged 8-byte envelope / 315-byte record mismatch (synthetic).');

const reference = fs.readFileSync(path.join(root, 'samples/carlife-g9980-mdev-replay.bin'));
const end = 116 + reference.readUInt32LE(112);
const idr = Buffer.concat([reference.subarray(116, end),
    reference.subarray(end + 32, end + 24 + reference.readUInt32LE(end + 20))]);
const vh = Buffer.alloc(12); vh.writeUInt32BE(idr.length); vh.writeUInt32BE(0x20001, 8);
const init = envelope(1, command(0x10008, Buffer.from([8, 0x80, 10, 16, 0xd0, 5, 24, 30])));
const video = envelope(2, Buffer.concat([vh, idr]));
const full = envelope(1, info);
for (const split of [1, 7, 8, 9, 16, 17, 100, 322]) {
    const r = run([match, full.subarray(0, split), full.subarray(split), init, video]);
    assert.equal(r.status, 0, `split=${split}\n${r.log}`);
    assert.deepEqual(r.output, idr);
}
const sliced = run([match, envelope(1, info.subarray(0, 8)), envelope(1, info.subarray(8)), init, video]);
assert.equal(sliced.status, 0, sliced.log);
assert.deepEqual(sliced.output, idr);
console.log('PASS: correct 315-byte envelope across 8 USB split positions and two outer envelopes.');
const noMatch = run([match.subarray(0, 8), envelope(1, info), init, video]);
assert.notEqual(noMatch.status, 0);
assert.equal(noMatch.output.length, 0);
assert.ok(!noMatch.log.includes('CarLife 1.0 accepted'));
console.log('PASS: missing version response is not falsely accepted as a successful handshake.');
