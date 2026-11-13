const assert = require('node:assert/strict');
const {once} = require('node:events');
const net = require('node:net');
const {probe} = require('../carlife-wifi-probe.cjs');

async function run() {
    const servers = [];
    let response;
    try {
        for (const port of [7240, 8240, 9240, 9241, 9242, 9340]) {
            const server = net.createServer(socket => {
                if (port !== 7240) return socket.resume();
                let data = Buffer.alloc(0);
                socket.on('data', chunk => {
                    data = Buffer.concat([data, chunk]);
                    if (data.length < 12) return;
                    assert.equal(data.toString('hex'), '000400000001800108011000');
                    socket.write(response.subarray(0, 5));
                    setTimeout(() => socket.end(response.subarray(5)), 20);
                });
            });
            servers.push(server);
            server.listen(port, '127.0.0.1');
            await once(server, 'listening');
        }
        response = Buffer.from('000000000001001800020000000100020801', 'hex');
        await probe('127.0.0.1', () => {});
        response = Buffer.from('00020000000100020800', 'hex');
        await assert.rejects(probe('127.0.0.1', () => {}), /did not accept/);
        response = Buffer.from('000000000001006a', 'hex');
        await assert.rejects(probe('127.0.0.1', () => {}), /requested encryption/);
        response = Buffer.from('000200000001000208', 'hex');
        await assert.rejects(probe('127.0.0.1', () => {}), /closed the control channel/);
        console.log('PASS: six TCP channels, split/coalesced commands, rejection, encryption request and truncated reply');
    } finally {
        for (const server of servers) server.close();
    }
}

run().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
