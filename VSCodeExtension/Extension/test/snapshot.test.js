// Serialization and diff ordering for capture snapshots. Plain node:test against the compiled module,
// so the file format and the ordering can be checked without an extension host.
const test = require('node:test');
const assert = require('node:assert/strict');

const { createSnapshot, parseSnapshot, diffSnapshots, SNAPSHOT_SCHEMA } = require('../out/snapshot.js');

const METADATA = {
    map: 'L_ShooterGym',
    build: 'DebugGame (Editor)',
    engine: '5.8.0',
    project: 'LyraStarterGame',
    capturedAt: '2026-11-11T10:00:00.000Z',
};

function snapshotOf(lines, overrides = {}) {
    return createSnapshot({
        capture: { capturing: false, generation: 3, totalHits: 99 },
        metadata: METADATA,
        lines,
        extensionVersion: '1.6.0',
        created: '2026-11-11T12:00:00.000Z',
        ...overrides,
    });
}

function line(className, functionName, number, hits, kind = 'statement') {
    return { className, functionName, line: number, hits, kind };
}

test('createSnapshot stamps the format and totals the lines', () => {
    const snapshot = snapshotOf([line('BP_A', 'Run', 4, 10), line('BP_A', 'Run', 6, 5, 'transfer')]);

    assert.equal(snapshot.tool, 'blueprint-inspector');
    assert.equal(snapshot.schema, SNAPSHOT_SCHEMA);
    assert.equal(snapshot.created, '2026-11-11T12:00:00.000Z');
    assert.equal(snapshot.extensionVersion, '1.6.0');
    assert.deepEqual(snapshot.capture, { capturing: false, generation: 3, totalHits: 99 });
    assert.deepEqual(snapshot.metadata, METADATA);
    assert.deepEqual(snapshot.totals, { lines: 2, hits: 15 });
    assert.equal(snapshot.lines[1].kind, 'transfer');
});

test('createSnapshot normalizes an unknown kind to statement', () => {
    const snapshot = snapshotOf([line('BP_A', 'Run', 4, 1, 'something-else')]);
    assert.equal(snapshot.lines[0].kind, 'statement');
});

test('a saved snapshot round-trips through parseSnapshot', () => {
    const written = snapshotOf([line('BP_A', 'Run', 4, 10), line('BP_B', 'Tick', 2, 0)]);
    const parsed = parseSnapshot(JSON.stringify(written));

    assert.equal(parsed.error, undefined);
    assert.deepEqual(parsed.snapshot, written);
});

test('parseSnapshot rejects files it cannot compare', () => {
    assert.match(parseSnapshot('{').error, /not valid JSON/);
    assert.match(parseSnapshot(JSON.stringify({ tool: 'other', schema: 1, lines: [] })).error, /not a Blueprint Inspector snapshot/);
    assert.match(parseSnapshot(JSON.stringify({ tool: 'blueprint-inspector', schema: 2, lines: [] })).error, /unsupported snapshot schema/);
    assert.match(parseSnapshot(JSON.stringify({ tool: 'blueprint-inspector', schema: 1 })).error, /no lines array/);
    assert.match(
        parseSnapshot(JSON.stringify({ tool: 'blueprint-inspector', schema: 1, lines: [{ className: 'BP_A' }] })).error,
        /malformed line entry/);
});

test('parseSnapshot tolerates optional fields written by an older snapshot', () => {
    const parsed = parseSnapshot(JSON.stringify({
        tool: 'blueprint-inspector',
        schema: 1,
        lines: [{ className: 'BP_A', functionName: 'Run', line: 4, hits: 7 }],
    }));

    assert.equal(parsed.error, undefined);
    assert.equal(parsed.snapshot.created, '');
    assert.equal(parsed.snapshot.extensionVersion, '');
    assert.equal(parsed.snapshot.metadata.map, '');
    assert.equal(parsed.snapshot.lines[0].kind, 'statement');
    assert.deepEqual(parsed.snapshot.totals, { lines: 1, hits: 7 });
});

test('diffSnapshots classifies regressions, improvements, new and gone lines', () => {
    const before = snapshotOf([
        line('BP_A', 'Run', 4, 10),
        line('BP_A', 'Run', 6, 8),
        line('BP_A', 'Run', 9, 0),
        line('BP_A', 'Run', 12, 5),
    ]);
    const after = snapshotOf([
        line('BP_A', 'Run', 4, 30),   // regressed by 20
        line('BP_A', 'Run', 6, 3),    // improved by 5
        line('BP_A', 'Run', 9, 0),    // unchanged
        line('BP_A', 'Run', 15, 4),   // added
        // line 12 is gone
    ]);

    const diff = diffSnapshots(before, after);
    assert.equal(diff.regressed, 1);
    assert.equal(diff.improved, 1);
    assert.equal(diff.added, 1);
    assert.equal(diff.removed, 1);
    assert.equal(diff.unchanged, 1);
    assert.equal(diff.entries.length, 4);

    const byChange = Object.fromEntries(diff.entries.map(entry => [entry.change, entry]));
    assert.deepEqual(
        { before: byChange.regressed.before, after: byChange.regressed.after, delta: byChange.regressed.delta },
        { before: 10, after: 30, delta: 20 });
    assert.equal(byChange.improved.delta, -5);
    assert.equal(byChange.added.after, 4);
    assert.equal(byChange.added.before, undefined);
    assert.equal(byChange.removed.before, 5);
    assert.equal(byChange.removed.after, undefined);
    assert.equal(byChange.removed.delta, -5);
});

test('diffSnapshots orders regressions first, then new lines, improvements and removals', () => {
    const before = snapshotOf([
        line('BP_A', 'Run', 1, 100),
        line('BP_A', 'Run', 2, 100),
        line('BP_A', 'Run', 3, 100),
        line('BP_A', 'Run', 4, 100),
    ]);
    const after = snapshotOf([
        line('BP_A', 'Run', 1, 110),  // +10
        line('BP_A', 'Run', 2, 160),  // +60  (biggest regression)
        line('BP_A', 'Run', 3, 40),   // -60  (biggest improvement)
        line('BP_A', 'Run', 5, 9),    // added
        // line 4 gone, used to cost 100
    ]);

    const changes = diffSnapshots(before, after).entries.map(entry => [entry.change, entry.line]);
    assert.deepEqual(changes, [
        ['regressed', 2],
        ['regressed', 1],
        ['added', 5],
        ['improved', 3],
        ['removed', 4],
    ]);
});

test('diffSnapshots orders within a group by magnitude and breaks ties by identity', () => {
    const before = snapshotOf([
        line('BP_Z', 'Run', 9, 1),
        line('BP_A', 'Run', 9, 1),
        line('BP_A', 'Run', 4, 1),
    ]);
    const after = snapshotOf([
        line('BP_Z', 'Run', 9, 11),
        line('BP_A', 'Run', 9, 11),
        line('BP_A', 'Run', 4, 11),
    ]);

    const order = diffSnapshots(before, after).entries.map(entry => `${entry.className}:${entry.line}`);
    // Same delta, so class then line decide.
    assert.deepEqual(order, ['BP_A:4', 'BP_A:9', 'BP_Z:9']);
});

test('diffSnapshots treats a kind change as a different line', () => {
    const before = snapshotOf([line('BP_A', 'Run', 4, 10, 'statement')]);
    const after = snapshotOf([line('BP_A', 'Run', 4, 10, 'transfer')]);

    const diff = diffSnapshots(before, after);
    assert.equal(diff.added, 1);
    assert.equal(diff.removed, 1);
    assert.equal(diff.unchanged, 0);
});

test('diffSnapshots reports the hit totals of both runs', () => {
    const before = snapshotOf([line('BP_A', 'Run', 4, 10), line('BP_A', 'Run', 6, 5)]);
    const after = snapshotOf([line('BP_A', 'Run', 4, 20)]);

    const diff = diffSnapshots(before, after);
    assert.equal(diff.beforeHits, 15);
    assert.equal(diff.afterHits, 20);
});
