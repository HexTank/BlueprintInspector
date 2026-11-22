// Export formatting for the Coverage view. Plain node:test against the compiled module, so the JSON
// and LCOV text can be checked without an extension host.
const test = require('node:test');
const assert = require('node:assert/strict');

const { coverageJson, coverageLcov } = require('../out/coverage.js');

const META = { renderedClasses: 2, totalLines: 4, coveredLines: 2, uncoveredLines: 2, truncated: false };
const CAPTURE = { capturing: false, generation: 2, totalHits: 5 };

function line(className, functionName, number, hits, kind = 'statement') {
    return { className, functionName, line: number, hits, kind };
}

test('coverageJson writes the schema, capture state and totals', () => {
    const parsed = JSON.parse(coverageJson(CAPTURE, META, [
        line('BP_A', 'Run', 4, 3),
        line('BP_A', 'Run', 6, 0, 'transfer'),
    ]));

    assert.equal(parsed.tool, 'blueprint-inspector');
    assert.equal(parsed.schema, 1);
    assert.deepEqual(parsed.capture, CAPTURE);
    assert.equal(parsed.renderedClasses, 2);
    assert.equal(parsed.truncated, false);
    assert.deepEqual(parsed.totals, { lines: 4, covered: 2, uncovered: 2, percent: 50 });
    assert.deepEqual(parsed.lines[1], { className: 'BP_A', functionName: 'Run', line: 6, hits: 0, kind: 'transfer' });
});

test('coverageJson reports zero percent when nothing rendered', () => {
    const parsed = JSON.parse(coverageJson(CAPTURE,
        { renderedClasses: 0, totalLines: 0, coveredLines: 0, uncoveredLines: 0, truncated: false }, []));
    assert.equal(parsed.totals.percent, 0);
    assert.deepEqual(parsed.lines, []);
});

test('coverageLcov writes one record per class, sorted, with DA and function records', () => {
    const text = coverageLcov([
        line('BP_Beta', 'Run', 12, 0),
        line('BP_Alpha', 'ReceiveBeginPlay', 9, 0, 'transfer'),
        line('BP_Alpha', 'ReceiveBeginPlay', 5, 3),
        line('BP_Alpha', 'ReceiveBeginPlay', 7, 2),
    ]);
    const records = text.trim().split('end_of_record').slice(0, -1).map(record => record.trim().split('\n'));

    assert.equal(records.length, 2);
    assert.deepEqual(records[0], [
        'TN:blueprint-inspector',
        'SF:Blueprints/BP_Alpha.cpp',
        'FN:5,ReceiveBeginPlay',
        'FNDA:5,ReceiveBeginPlay',
        'DA:5,3',
        'DA:7,2',
        'DA:9,0',
        'BRDA:9,0,0,0',
        'LF:3',
        'LH:2',
        'BRF:1',
        'BRH:0',
    ]);
    assert.deepEqual(records[1], [
        'TN:blueprint-inspector',
        'SF:Blueprints/BP_Beta.cpp',
        'FN:12,Run',
        'FNDA:0,Run',
        'DA:12,0',
        'LF:1',
        'LH:0',
    ]);
});

test('coverageLcov omits branch records for a class without arms', () => {
    const text = coverageLcov([line('BP_A', 'Run', 4, 1)]);
    assert.ok(!text.includes('BRDA:'));
    assert.ok(!text.includes('BRF:'));
    assert.ok(text.endsWith('end_of_record\n'));
});

test('coverageLcov writes nothing for an empty snapshot', () => {
    assert.equal(coverageLcov([]), '\n');
});
