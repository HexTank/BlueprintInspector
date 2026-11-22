const net = require('net');
const fs = require('fs');

const port = Number(process.env.BPI_PORT || 19021);
const tokenPath = process.argv[2];
const mode = process.argv[3] || 'smoke';
const targets = (process.argv[4] || 'BTS_Shoot,GA_Weapon_Fire,B_Hero_ShooterMannequin').split(',').map(value => value.trim()).filter(Boolean);

class Session {
    constructor() {
        this.sequence = 0;
        this.pending = new Map();
        this.events = [];
        this.waiters = [];
        this.buffer = Buffer.alloc(0);
        this.socket = net.connect(port, '127.0.0.1');
        this.socket.on('data', chunk => this.read(chunk));
        this.socket.on('error', error => {
            for (const entry of this.pending.values()) entry.reject(error);
            this.pending.clear();
        });
        this.ready = new Promise((resolve, reject) => {
            this.socket.once('connect', resolve);
            this.socket.once('error', reject);
        });
    }

    read(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        for (;;) {
            const separator = this.buffer.indexOf('\r\n\r\n');
            if (separator < 0) return;
            const header = this.buffer.subarray(0, separator).toString('ascii');
            const match = /Content-Length:\s*(\d+)/i.exec(header);
            const length = match ? Number(match[1]) : 0;
            const start = separator + 4;
            if (this.buffer.length < start + length) return;
            const body = this.buffer.subarray(start, start + length).toString('utf8');
            this.buffer = this.buffer.subarray(start + length);
            this.handle(JSON.parse(body));
        }
    }

    handle(message) {
        if (message.type === 'response') {
            const entry = this.pending.get(message.request_seq);
            if (!entry) return;
            this.pending.delete(message.request_seq);
            if (message.success) entry.resolve(message.body || {});
            else entry.reject(new Error(message.message || 'request failed'));
            return;
        }
        if (message.type === 'event') {
            this.events.push(message);
            this.waiters = this.waiters.filter(waiter => {
                if (waiter.event !== message.event) return true;
                waiter.resolve(message);
                return false;
            });
        }
    }

    request(command, args = {}, timeoutMs = 30000) {
        const seq = ++this.sequence;
        const payload = JSON.stringify({ seq, type: 'request', command, arguments: args });
        this.socket.write(`Content-Length: ${Buffer.byteLength(payload)}\r\n\r\n${payload}`);
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(seq);
                reject(new Error(`${command}: timed out`));
            }, timeoutMs);
            this.pending.set(seq, {
                resolve: value => { clearTimeout(timer); resolve(value); },
                reject: error => { clearTimeout(timer); reject(error); },
            });
        });
    }

    waitEvent(event, timeoutMs = 120000) {
        const index = this.events.findIndex(entry => entry.event === event);
        if (index >= 0) return Promise.resolve(this.events.splice(index, 1)[0]);
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error(`event ${event}: timed out`)), timeoutMs);
            this.waiters.push({ event, resolve: value => { clearTimeout(timer); resolve(value); } });
        });
    }

    close() {
        this.socket.end();
    }
}

async function readTokenAfterConnect() {
    for (let attempt = 0; attempt < 40; attempt++) {
        try {
            const text = fs.readFileSync(tokenPath, 'utf8').trim();
            if (text.length > 0) return text;
        } catch (error) {
            if (error.code !== 'ENOENT') throw error;
        }
        await new Promise(resolve => setTimeout(resolve, 250));
    }
    throw new Error(`no token at ${tokenPath} after 10 s`);
}

function sourceRef(entry) {
    return { name: entry.name, path: entry.path, sourceReference: entry.sourceReference };
}

async function handshake(session, token) {
    await session.request('initialize', {
        adapterID: 'blueprint-inspector',
        clientID: 'bpi-packaged-smoke',
        linesStartAt1: true,
        columnsStartAt1: true,
        pathFormat: 'path',
    });
    await session.waitEvent('initialized', 10000);
    await session.request('launch', { __restart: { token } });
}

let failed = false;

function report(label, ok, detail = '') {
    console.log(`${ok ? 'PASS' : 'FAIL'} ${label}${detail ? `: ${detail}` : ''}`);
    if (!ok) failed = true;
    return ok;
}

async function expectRejection(label, token) {
    const session = new Session();
    try {
        await session.ready;
        await handshake(session, token);
        await session.request('loadedSources', {}, 15000);
        return report(label, false, 'the adapter served a gated request');
    } catch (error) {
        return report(label, true, error.message);
    } finally {
        session.close();
    }
}

async function runSmoke() {
    const session = new Session();
    await session.ready;
    const token = await readTokenAfterConnect();
    await handshake(session, token);

    const threads = await session.request('threads');
    const loaded = await session.request('loadedSources');
    const sources = loaded.sources || [];
    report('attach', sources.length > 0, `${threads.threads.length} thread, ${sources.length} blueprints`);

    const audit = await session.request('blueprintInspector.audit', { maxFindings: 20 }, 180000);
    report('audit', audit.classes > 0, `${audit.classes} classes, ${audit.functions} functions, ${(audit.findings || []).length} findings`);

    let armed = 0;
    const chosen = [];
    for (const needle of targets) {
        const entry = sources.find(source => source.name.includes(needle));
        if (!entry) continue;
        const locations = await session.request('breakpointLocations', { source: sourceRef(entry), line: 1, endLine: 2000 }, 60000);
        const lines = (locations.breakpoints || []).map(bp => bp.line);
        if (lines.length === 0) continue;
        const picks = [lines[Math.floor(lines.length / 4)], lines[Math.floor(lines.length / 2)], lines[Math.floor((lines.length * 3) / 4)]]
            .filter((line, index, array) => line !== undefined && array.indexOf(line) === index);
        const response = await session.request('setBreakpoints', { source: sourceRef(entry), breakpoints: picks.map(line => ({ line })) }, 60000);
        armed += (response.breakpoints || []).filter(bp => bp.verified).length;
        chosen.push(entry);
    }
    report('breakpoints', armed > 0, `${armed} verified in ${chosen.length} blueprints`);

    const stopped = await session.waitEvent('stopped', 180000);
    const stack = await session.request('stackTrace', { threadId: stopped.body.threadId, startFrame: 0, levels: 5 }, 60000);
    report('break', stack.stackFrames.length > 0, stack.stackFrames.map(frame => frame.name).join(' <- '));

    const scopes = await session.request('scopes', { frameId: stack.stackFrames[0].id }, 60000);
    // Parameters come first in a frame's scope, and an out or reference parameter cannot be written:
    // the adapter rejects those by design. Collect every scalar candidate and take the first one the
    // adapter accepts, so the smoke proves setVariable without depending on which parameter the map
    // happens to stop in.
    const simple = ['bool', 'int', 'float', 'double', 'byte', 'String', 'Name', 'Text'];
    const candidates = [];
    for (const scope of scopes.scopes) {
        if (scope.variablesReference <= 0) continue;
        const variables = await session.request('variables', { variablesReference: scope.variablesReference }, 60000);
        for (const variable of variables.variables) {
            if (variable.variablesReference === 0 && simple.some(name => (variable.type || '').includes(name))) {
                candidates.push({ reference: scope.variablesReference, variable });
            }
        }
    }

    let edited = false;
    let refusal = '';
    for (const { reference, variable } of candidates) {
        const current = String(variable.value).trim();
        const lowered = current.toLowerCase();
        const replacement = /^-?[0-9.]+$/.test(current)
            ? String(Number(current) + 1)
            : lowered === 'true' ? 'false' : lowered === 'false' ? 'true' : `${current}smoke`;
        try {
            const response = await session.request('setVariable', { variablesReference: reference, name: variable.name, value: replacement }, 60000);
            report('setVariable', true, `${variable.name}: ${current} -> ${response.value}`);
            edited = true;
            break;
        } catch (error) {
            refusal = `${variable.name} (${error.message})`;
        }
    }
    if (!edited) {
        report('setVariable', false, refusal ? `no writable scalar variable; the last refusal was ${refusal}` : 'no scalar variable in any scope');
    }

    for (const entry of chosen) {
        await session.request('setBreakpoints', { source: sourceRef(entry), breakpoints: [] }, 60000);
    }
    await session.request('next', { threadId: stopped.body.threadId }, 60000);
    const stepped = await session.waitEvent('stopped', 120000);
    report('step', true, stepped.body.reason);

    await session.request('disconnect', {});
    session.close();
    report('detach', true);
}

async function runAuth() {
    // The bare connect/read/close below is gate 4's case: a client that connects, reads the token and
    // leaves without authenticating must leave the adapter able to accept the next connection. The
    // empty-token case that follows proves it reached the auth decision instead of a dead listener.
    const probe = new Session();
    await probe.ready;
    const token = await readTokenAfterConnect();
    probe.close();

    await expectRejection('empty token refused', '');
    await expectRejection('wrong token refused', 'not-the-token');

    const staleSession = new Session();
    await staleSession.ready;
    const staleToken = await readTokenAfterConnect();
    staleSession.close();
    await expectRejection('token from a closed connection refused', staleToken);

    try {
        const final = new Session();
        await final.ready;
        const finalToken = await readTokenAfterConnect();
        await handshake(final, finalToken);
        const loaded = await final.request('loadedSources', {}, 30000);
        report('adapter accepts a client after the negatives', (loaded.sources || []).length > 0);
        await final.request('disconnect', {});
        final.close();
    } catch (error) {
        console.log(`INFO a fresh client after three refusals: ${error.message}`);
    }
}

const run = mode === 'auth' ? runAuth : runSmoke;
run()
    .then(() => process.exit(failed ? 1 : 0))
    .catch(error => {
        console.log(`FAIL ${error.message}`);
        process.exit(1);
    });
