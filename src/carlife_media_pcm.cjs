// Extract legacy plaintext CarLife media PCM from channel 3.
const { pipeline } = require('node:stream/promises');
const { aoaChannel, framedRecords } = require('./carlife_live_video.cjs');

const mediaTypes = new Map([
    [0x00030001, 'init'], [0x00030002, 'stop'], [0x00030003, 'pause'],
    [0x00030004, 'resume'], [0x00030005, 'seek'], [0x00030006, 'data']
]);

function mediaFormat(payload) {
    let offset = 0;
    const fields = [];

    function varint() {
        let value = 0n;
        for (let shift = 0n; offset < payload.length && shift < 64n; shift += 7n) {
            const byte = payload[offset++];
            if (shift === 63n && byte > 1)
                break;
            value |= BigInt(byte & 0x7f) << shift;
            if (byte < 0x80)
                return value;
        }
        throw new Error('Invalid MEDIA_INIT protobuf varint');
    }

    while (offset < payload.length) {
        const key = Number(varint());
        const field = key >>> 3;
        if (key > 0xffffffff || !field)
            throw new Error('Invalid MEDIA_INIT protobuf field');

        const wire = key & 7;
        if (wire === 0) {
            const value = varint();
            if (field <= 3)
                fields[field - 1] = Number(value);
            continue;
        }
        if (field <= 3)
            throw new Error('Invalid MEDIA_INIT PCM field type');
        switch (wire) {
        case 1: offset += 8; break;
        case 2: {
            const length = Number(varint());
            offset += length;
            break;
        }
        case 5: offset += 4; break;
        default: throw new Error('Invalid MEDIA_INIT protobuf wire type');
        }
        if (offset > payload.length)
            throw new Error('Truncated MEDIA_INIT protobuf field');
    }

    const [rate, channels, bits] = fields;
    if (!Number.isInteger(rate) || rate < 1 || rate > 384000 ||
        (channels !== 1 && channels !== 2) || (bits !== 8 && bits !== 16))
        throw new Error('Unsupported MEDIA_INIT PCM format (expected rate, mono/stereo, 8/16 bits)');
    return { rate, channels, bits };
}

async function* mediaEvents(source) {
    const records = framedRecords(source, 12, 0, 'CarLife media', (header, length) => {
        const id = header.readUInt32BE(8);
        const type = mediaTypes.get(id);
        if (!type || length > 100 * 1024)
            throw new Error(`Invalid CarLife media record 0x${id.toString(16)} (${length} bytes)`);
        return { type, timestamp: header.readUInt32BE(4) };
    });

    let format;
    for await (const { tag, payload } of records) {
        switch (tag.type) {
        case 'init':
            format = mediaFormat(payload);
            yield { ...tag, format };
            break;
        case 'data':
            if (!format)
                throw new Error('MEDIA_DATA without an active MEDIA_INIT');
            if (payload.length % (format.channels * format.bits / 8))
                throw new Error('MEDIA_DATA ends with a partial PCM sample frame');
            if (payload.length)
                yield { ...tag, payload };
            break;
        case 'stop':
            format = undefined;
            yield tag;
            break;
        default:
            yield tag;
        }
    }
}

async function* extractPcm(source) {
    let format;
    let bytes = 0;
    for await (const event of mediaEvents(source)) {
        if (event.type === 'init') {
            const { rate, channels, bits } = event.format;
            if (format && (rate !== format.rate || channels !== format.channels || bits !== format.bits))
                throw new Error('PCM format changed; extract each format separately');
            format = event.format;
            console.error(`PCM: ${rate} Hz, ${channels} channel(s), ${bits} bits`);
        } else if (event.type === 'data') {
            bytes += event.payload.length;
            yield event.payload;
        }
    }
    if (!bytes)
        throw new Error('No media PCM samples found');
    console.error(`Extracted ${bytes} PCM bytes`);
}

async function main() {
    const args = process.argv.slice(2);
    const aoa = args.length === 1 && args[0] === '--aoa';
    if (args.length && !aoa)
        throw new Error('Usage: node carlife_media_pcm.cjs [--aoa] < media-capture > audio.pcm');
    await pipeline(process.stdin, source => extractPcm(aoa ? aoaChannel(source, 3) : source),
        process.stdout);
}

if (require.main === module) {
    main().catch(error => {
        console.error(error.message);
        process.exitCode = 1;
    });
}

module.exports = { mediaEvents };
