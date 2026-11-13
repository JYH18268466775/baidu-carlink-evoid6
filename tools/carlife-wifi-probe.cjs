// Probe the legacy CarLife Wi-Fi protocol on a phone selected by the user.
const net = require('node:net');

const ports = [7240, 8240, 9240, 9241, 9242, 9340];
const versionRequest = Buffer.from('000400000001800108011000', 'hex');

async function checkVersion(socket) {
    let pending = Buffer.alloc(0);
    for await (const chunk of socket) {
        pending = Buffer.concat([pending, chunk]);
        while (pending.length >= 8) {
            const size = pending.readUInt16BE(0);
            const type = pending.readUInt32BE(4);
            if (pending.length < 8 + size) break;
            const body = pending.subarray(8, 8 + size);
            pending = pending.subarray(8 + size);
            if (type === 0x1006a) throw new Error('Phone requested encryption. Ended the version probe.');
            if (type !== 0x10002) continue;
            if (body.length < 2 || body[0] !== 8 || body[1] !== 1)
                throw new Error('Phone did not accept CarLife protocol version 1.0.');
            return;
        }
    }
    throw new Error('Phone closed the control channel before version confirmation.');
}

async function probe(host, report = console.log) {
    if (!net.isIP(host)) throw new Error('Usage: node tools/carlife-wifi-probe.cjs PHONE_IP');
    const sockets = [];
    let timer;
    try {
        const results = await Promise.allSettled(ports.map((port, index) => new Promise((resolve, reject) => {
            const socket = net.createConnection({host, port});
            sockets.push(socket);
            const deadline = setTimeout(() => {
                reject(new Error(`TCP ${port}: timed out`));
                socket.destroy();
            }, 3000);
            socket.on('error', error => {
                clearTimeout(deadline);
                reject(new Error(`TCP ${port}: ${error.code || error.message}`));
            });
            socket.once('connect', () => {
                clearTimeout(deadline);
                resolve(socket);
            });
            if (index !== 0) socket.resume();
        })));
        for (const [index, result] of results.entries()) {
            report(result.status === 'fulfilled' ? `TCP ${ports[index]} connected` : result.reason.message);
        }
        if (results.some(result => result.status === 'rejected'))
            throw new Error('Legacy Wi-Fi handshake not started: one or more required TCP channels unavailable.');
        const control = results[0].value;
        const session = (async () => {
            control.write(versionRequest);
            report('CarLife version request sent');
            await checkVersion(control);
            report('Phone accepted CarLife 1.0 over TCP');
        })();
        await Promise.race([session, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error('No complete CarLife handshake within 8 seconds.')), 8000);
        })]);
    } finally {
        clearTimeout(timer);
        for (const socket of sockets) socket.destroy();
    }
}

if (require.main === module) {
    probe(process.argv[2]).catch(error => {
        console.error(error.message);
        process.exitCode = 1;
    });
}

module.exports = {probe};
