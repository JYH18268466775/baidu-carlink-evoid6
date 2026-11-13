// Observe the phone-side CarLife discovery broadcast on a test LAN.
const dgram = require('node:dgram');

const socket = dgram.createSocket('udp4');
let timer;
let found = false;
socket.on('error', error => {
    console.error(`UDP 7999: ${error.message}`);
    process.exitCode = 1;
    socket.close();
});
socket.on('close', () => clearTimeout(timer));
socket.on('message', (message, peer) => {
    if (!message.subarray(0, 11).equals(Buffer.from('CarlifeHost'))) return;
    if (!found) {
        found = true;
        console.log(`CarlifeHost broadcast from ${peer.address}`);
        socket.close();
    }
});
socket.bind(7999, '0.0.0.0', () => {
    console.log('Listening for CarlifeHost on UDP 7999 for 30 seconds...');
    timer = setTimeout(() => {
        console.error('No CarlifeHost broadcast received in 30 seconds.');
        process.exitCode = 1;
        socket.close();
    }, 30_000);
});
