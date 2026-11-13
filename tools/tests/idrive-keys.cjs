const assert = require('node:assert/strict');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { idrivePackets } = require('../../src/carlife_idrive_keys.cjs');

function expected(code) {
    return Buffer.from(`000000060000000a000200000006800808${code}`, 'hex');
}

const actions = [
    ['OK', ['14']], ['BACK', ['0e']], ['LEFT', ['15']], ['RIGHT', ['16']],
    ['UP', ['17']], ['DOWN', ['18']], ['+2', ['06', '06']], ['-1', ['07']], ['+0', []]
];
for (const [action, codes] of actions) {
    const packets = [...idrivePackets(action)];
    assert.equal(packets.length, codes.length);
    for (let i = 0; i < codes.length; ++i) {
        const { outer, inner } = packets[i];
        assert.equal(outer.length, 8);
        assert.equal(inner.length, 10);
        assert.deepEqual(Buffer.concat([outer, inner]), expected(codes[i]));
    }
}

for (const action of ['MENU', 'ROTATE', '2', '+1.5', '+32768', '-32769'])
    assert.throws(() => [...idrivePackets(action)], /Unknown iDrive action|Rotary delta out of range/);

const program = path.resolve(__dirname, '../../src/carlife_idrive_keys.cjs');
const result = spawnSync(process.execPath, [program, ...actions.map(([action]) => action)]);
if (result.error)
    throw result.error;
assert.equal(result.status, 0, result.stderr.toString());
assert.deepEqual(result.stdout, Buffer.concat(actions.flatMap(([, codes]) => codes.map(expected))));
console.log('PASS: CarLife hard-key bytes, separate AOA transfers and rotary mapping');
