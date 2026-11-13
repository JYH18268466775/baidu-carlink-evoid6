// Normalized events only. MOST subscription and HMI ownership are not implemented here.
const {idrivePackets} = require('./carlife_idrive_keys.cjs');
const supported = new Set(['OK', 'BACK', 'LEFT', 'RIGHT', 'UP', 'DOWN']);
class IdriveAdapter {
    constructor() { this.active = false; this.held = new Map(); this.time = -1; }
    accept(event) {
        if (!event || !Number.isSafeInteger(event.ms) || event.ms < this.time)
            throw new Error('Expected monotonic millisecond timestamp');
        this.time = event.ms;
        if (event.type === 'focus') {
            if (typeof event.active !== 'boolean') throw new Error('Invalid focus');
            this.active = event.active; this.held.clear(); return [];
        }
        if (event.type === 'disconnect') {
            this.active = false; this.held.clear(); return [];
        }
        if (event.type === 'rotate') {
            if (!Number.isInteger(event.delta) || Math.abs(event.delta) > 32)
                throw new Error('Expected bounded rotary delta (-32..32)');
            if (!this.active || !event.delta) return [];
            return [...idrivePackets(`${event.delta > 0 ? '+' : ''}${event.delta}`)];
        }
        if (event.type !== 'key' || !supported.has(event.key) || typeof event.down !== 'boolean')
            throw new Error('Unsupported normalized key event');
        if (!this.active) return [];
        if (event.cancel === true) { this.held.delete(event.key); return []; }
        if (event.down) {
            if (this.held.has(event.key)) return [];
            this.held.set(event.key, event.ms);
            return event.key === 'BACK' ? [] : [...idrivePackets(event.key)];
        }
        const pressed = this.held.get(event.key);
        this.held.delete(event.key);
        // Long BACK remains reserved for the future local exit action.
        return event.key === 'BACK' && pressed !== undefined && event.ms - pressed < 800
            ? [...idrivePackets('BACK')] : [];
    }
}
module.exports = {IdriveAdapter};
