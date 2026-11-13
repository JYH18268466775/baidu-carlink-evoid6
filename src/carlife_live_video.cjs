// Convert plaintext CarLife video or AOA bulk-IN bytes to a streaming mdev replay.
const { pipeline } = require('node:stream/promises');

const VIDEO_DATA = 0x00020001;
const VIDEO_HEARTBEAT = 0x00020002;
const MAX_PAYLOAD = 64 * 1024 * 1024;
const MAX_FIRST_IDR = 2 * 1024 * 1024 + 2 * 4096; // Video message plus cached SPS/PPS.

async function* framedRecords(source, headerLength, lengthOffset, label, validate) {
    const header = Buffer.alloc(headerLength);
    let headerBytes = 0;
    let payload;
    let payloadBytes = 0;
    let tag;

    for await (const chunk of source) {
        let offset = 0;
        while (offset < chunk.length) {
            if (!payload) {
                const count = Math.min(header.length - headerBytes, chunk.length - offset);
                chunk.copy(header, headerBytes, offset, offset + count);
                headerBytes += count;
                offset += count;
                if (headerBytes !== header.length)
                    continue;

                const length = header.readUInt32BE(lengthOffset);
                if (length > MAX_PAYLOAD)
                    throw new Error(`Invalid ${label} payload size: ${length}`);
                tag = validate(header, length);
                payload = Buffer.allocUnsafe(length);
                payloadBytes = 0;
            }

            const count = Math.min(payload.length - payloadBytes, chunk.length - offset);
            chunk.copy(payload, payloadBytes, offset, offset + count);
            payloadBytes += count;
            offset += count;
            if (payloadBytes === payload.length) {
                yield { tag, payload };
                payload = undefined;
                headerBytes = 0;
            }
        }
    }
    if (headerBytes !== 0 || payload)
        throw new Error(`${label} message truncated`);
}

async function* videoRecords(source) {
    const frames = framedRecords(source, 12, 0, 'CarLife video', (header, length) => {
        const kind = header.readUInt32BE(8);
        if (kind === VIDEO_DATA && length === 0)
            throw new Error('Empty CarLife video payload');
        if (kind !== VIDEO_DATA && kind !== VIDEO_HEARTBEAT)
            throw new Error(`Unexpected CarLife video message: 0x${kind.toString(16)}`);
        return kind;
    });
    for await (const { tag, payload } of frames)
        if (tag === VIDEO_DATA)
            yield payload;
}

async function* aoaChannel(source, channelId) {
    const packets = framedRecords(source, 8, 4, 'AOA', header => {
        const channel = header.readUInt32BE(0);
        if (channel < 1 || channel > 6)
            throw new Error(`Unknown AOA channel: ${channel}`);
        return channel;
    });
    for await (const { tag, payload } of packets)
        if (tag === channelId && payload.length)
            yield payload;
}

function aoaVideo(source) {
    return aoaChannel(source, 2);
}

function annexBNals(payload) {
    const starts = [...payload.toString('latin1').matchAll(/\x00\x00(?:\x00)?\x01(?=[\x01-\xff])/g)];
    if (!starts.length || starts[0].index !== 0)
        throw new Error('CarLife video payload does not start at an Annex B NAL boundary');
    return starts.map((start, i) => ({
        offset: start.index,
        prefix: start[0].length,
        bytes: payload.subarray(start.index, starts[i + 1]?.index ?? payload.length),
        kind: payload[start.index + start[0].length] & 31
    }));
}

function mdevMessage(kind, ...parts) {
    const header = Buffer.alloc(24);
    header.writeUInt32LE(kind, 16);
    header.writeUInt32LE(parts.reduce((length, part) => length + part.length, 0), 20);
    return [header, ...parts];
}

async function* replayFrames(frames, width, height, fps) {
    let sps;
    let pps;
    let frame = 0n;

    for await (let payload of frames) {
        const nals = annexBNals(payload);
        if (frame === 0n) {
            const idr = nals.find(nal => nal.kind === 5);
            for (const nal of nals) {
                if (nal === idr)
                    break;
                if (nal.kind === 7)
                    sps = nal;
                if (nal.kind === 8)
                    pps = nal;
            }
            if (!sps || !pps || !idr)
                continue;

            const sync = Buffer.alloc(4);
            sync.writeUInt32LE(1);
            const format = Buffer.alloc(32);
            [2, 0x48323634, width, height, 16, fps].forEach((value, i) =>
                format.writeUInt32LE(value, i * 4));
            yield Buffer.from('MDEVv1\r\n');
            yield* mdevMessage(5, sync);
            yield* mdevMessage(2, format);
            yield* mdevMessage(4, sps.bytes, pps.bytes);
            payload = payload.subarray(idr.offset);
        } else {
            for (const nal of nals) {
                const previous = nal.kind === 7 ? sps : nal.kind === 8 ? pps : null;
                if (previous && !nal.bytes.subarray(nal.prefix).equals(
                    previous.bytes.subarray(previous.prefix)))
                    throw new Error('CarLife codec settings changed; start a new renderer session');
            }
            if (!nals.some(nal => nal.kind >= 1 && nal.kind <= 5))
                continue;
        }

        // The captured CarLife timestamp has one-second granularity.
        const timestamp = Buffer.alloc(8);
        timestamp.writeBigUInt64LE(frame * 1000000n / BigInt(fps));
        yield* mdevMessage(3, timestamp, payload);
        ++frame;
    }
    if (frame === 0n)
        throw new Error('CarLife stream ended before SPS, PPS and an IDR were available');
}

function toMdev(source, width, height, fps) {
    return replayFrames(videoRecords(source), width, height, fps);
}

async function* firstIdr(source) {
    const chunks = [];
    let size = 0;
    for await (const chunk of source) {
        size += chunk.length;
        if (size > MAX_FIRST_IDR)
            throw new Error(`First-IDR capture exceeds ${MAX_FIRST_IDR} bytes`);
        chunks.push(chunk);
    }

    const payload = Buffer.concat(chunks, size);
    const nals = annexBNals(payload);
    if (nals[0]?.kind !== 7 || nals[1]?.kind !== 8 || nals[2]?.kind !== 5 ||
        nals.slice(3).some(nal => (nal.kind >= 1 && nal.kind < 5) ||
            nal.kind === 7 || nal.kind === 8 || nal.kind === 9))
        throw new Error('Expected a first-IDR capture with SPS, PPS and one IDR access unit');
    yield payload;
}

async function main() {
    const args = process.argv.slice(2);
    const aoa = args[0] === '--aoa';
    const captured = args[0] === '--first-idr';
    if (aoa || captured)
        args.shift();
    const [width, height, fps] = args.length ? args.map(Number) : [1280, 720, 30];
    if ((args.length !== 3 && (captured || args.length !== 0)) ||
        ![width, height, fps].every((value, i) =>
            Number.isInteger(value) && value >= 1 && value <= (i === 2 ? 1000 : 65535)))
        throw new Error('Usage: node carlife_live_video.cjs [--aoa] [width height fps]\n' +
            '       node carlife_live_video.cjs --first-idr width height fps');
    await pipeline(process.stdin, source => captured
        ? replayFrames(firstIdr(source), width, height, fps)
        : toMdev(aoa ? aoaVideo(source) : source, width, height, fps), process.stdout);
}

if (require.main === module) {
    main().catch(error => {
        console.error(error.message);
        process.exitCode = 1;
    });
}

module.exports = { aoaChannel, aoaVideo, framedRecords, toMdev };
