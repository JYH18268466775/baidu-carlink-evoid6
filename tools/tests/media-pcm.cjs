const assert = require('node:assert/strict');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { mediaEvents } = require('../../src/carlife_media_pcm.cjs');

const program = path.resolve(__dirname, '../../src/carlife_media_pcm.cjs');
const init48k = Buffer.from([8, 0x80, 0xf7, 2, 16, 2, 24, 16]);
const init8k = Buffer.from([8, 0xc0, 0x3e, 16, 1, 24, 8]);

function record(id, payload = Buffer.alloc(0), timestamp = 0) {
    const header = Buffer.alloc(12);
    header.writeUInt32BE(payload.length);
    header.writeUInt32BE(timestamp, 4);
    header.writeUInt32BE(0x30000 + id, 8);
    return Buffer.concat([header, payload]);
}

function envelope(channel, payload) {
    const header = Buffer.alloc(8);
    header.writeUInt32BE(channel);
    header.writeUInt32BE(payload.length, 4);
    return Buffer.concat([header, payload]);
}

function run(args, data, status = 0) {
    const result = spawnSync(process.execPath, [program, ...args], { input: data });
    if (result.error)
        throw result.error;
    assert.equal(result.status, status, result.stderr.toString());
    return result;
}

async function main() {
    const pcm = Buffer.alloc(4800 * 4);
    for (let i = 0; i < 4800; ++i) {
        const sample = Math.round(12000 * Math.sin(2 * Math.PI * 440 * i / 48000));
        pcm.writeInt16LE(sample, i * 4);
        pcm.writeInt16LE(sample, i * 4 + 2);
    }
    const first = record(6, pcm.subarray(0, 9600), 100);
    const second = record(6, pcm.subarray(9600), 200);
    // Include a legal unknown protobuf field and split the entire media channel
    // at awkward boundaries while another AOA channel is interleaved.
    const init = record(1, Buffer.concat([init48k, Buffer.from([0x22, 2, 0xaa, 0x55])]));
    const stop = record(2);
    const session = Buffer.concat([init, first, record(3), record(4), second, stop]);
    const wrapped = [];
    for (let i = 0; i < session.length; i += 139)
        wrapped.push(envelope(3, session.subarray(i, i + 139)), envelope(2, Buffer.from([0])));
    for (const result of [run([], session), run(['--aoa'], Buffer.concat(wrapped))]) {
        assert.deepEqual(result.stdout, pcm);
        assert.match(result.stderr.toString(), /48000 Hz, 2 channel\(s\), 16 bits/);
    }

    async function* splitBytes() {
        for (let i = 0; i < session.length; i += 7)
            yield session.subarray(i, i + 7);
    }
    const events = [];
    for await (const { type, timestamp } of mediaEvents(splitBytes()))
        events.push([type, timestamp]);
    assert.deepEqual(events, [['init', 0], ['data', 100], ['pause', 0], ['resume', 0],
        ['data', 200], ['stop', 0]]);

    const mono = Buffer.from([0, 128, 255]);
    assert.deepEqual(run([], Buffer.concat([record(1, init8k), record(6, mono)])).stdout, mono);

    const cases = [
        [first, /without an active MEDIA_INIT/],
        [Buffer.concat([init, stop, first]), /without an active MEDIA_INIT/],
        [Buffer.concat([init, record(6, Buffer.alloc(3))]), /partial PCM sample frame/],
        [Buffer.concat([init, first.subarray(0, 50)]), /CarLife media message truncated/],
        [record(1, Buffer.from([8, 0x80])), /protobuf varint/],
        [record(1, Buffer.from([8, 1, 16, 3, 24, 16])), /Unsupported MEDIA_INIT/],
        [Buffer.concat([init, stop, record(1, init8k)]), /PCM format changed/],
        [Buffer.concat([init, stop]), /No media PCM samples/]
    ];
    for (const [input, error] of cases) {
        const result = run([], input, 1);
        assert.match(result.stderr.toString(), error);
        assert.equal(result.stdout.length, 0);
    }
    console.log('PASS: 48 kHz stereo and 8 kHz mono PCM, AOA fragmentation, media controls and invalid input');
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
