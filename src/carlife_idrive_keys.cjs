// Encode normalized iDrive actions as legacy CarLife AOA channel-6 messages.
const { Readable } = require('node:stream');
const { pipeline } = require('node:stream/promises');

const keys = new Map([
    ['OK', 0x14], ['BACK', 0x0e], ['LEFT', 0x15], ['RIGHT', 0x16],
    ['UP', 0x17], ['DOWN', 0x18]
]);

function hardKey(code) {
    const inner = Buffer.alloc(10);
    inner.writeUInt16BE(2, 0);
    inner.writeUInt32BE(0x00068008, 4);
    inner[8] = 0x08; // Protobuf field 1, varint.
    inner[9] = code;

    const outer = Buffer.alloc(8);
    outer.writeUInt32BE(6, 0);
    outer.writeUInt32BE(inner.length, 4);
    return { outer, inner };
}

function* idrivePackets(action) {
    const key = keys.get(action);
    if (key !== undefined) {
        yield hardKey(key);
        return;
    }
    if (!/^[+-][0-9]+$/.test(action))
        throw new Error(`Unknown iDrive action: ${action}`);
    const delta = Number(action);
    if (!Number.isInteger(delta) || delta < -32768 || delta > 32767)
        throw new Error(`Rotary delta out of range: ${action}`);

    // Candidate mapping until the phone's response and rotary direction are verified.
    const selector = delta > 0 ? 0x06 : 0x07;
    for (let step = 0; step < Math.abs(delta); ++step)
        yield hardKey(selector);
}

function* fixture(actions) {
    for (const action of actions) {
        for (const { outer, inner } of idrivePackets(action)) {
            yield outer;
            yield inner;
        }
    }
}

async function main() {
    const actions = process.argv.slice(2);
    if (!actions.length)
        throw new Error('Usage: node carlife_idrive_keys.cjs OK BACK UP DOWN LEFT RIGHT +2 -1');
    await pipeline(Readable.from(fixture(actions)), process.stdout);
}

if (require.main === module) {
    main().catch(error => {
        console.error(error.message);
        process.exitCode = 1;
    });
}

module.exports = { idrivePackets };
