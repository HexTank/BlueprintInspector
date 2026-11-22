// Navigation model for rendered source: functions, parameters, assignments and "what is under the
// cursor". Plain node:test against the compiled module, so no extension host is needed.
const test = require('node:test');
const assert = require('node:assert/strict');

const { parseRenderedSource, variableAt } = require('../out/blueprintSymbols.js');

const RENDERED = [
    'SlelectHitMontage(FVector HitNormal, AActor* Hit_Actor)',
    '{',
    '    Dot_Product_Forward_Hit = 0.0000;',
    '    CallFunc_GetActorForwardVector_ReturnValue = Hit_Actor->GetActorForwardVector();',
    '    if (CallFunc_InRange_FloatFloat_ReturnValue)',
    '    {',
    '        Temp_bool_Variable = true;',
    '    }',
    '    if (CallFunc_InRange_FloatFloat_ReturnValue == false)',
    '    {',
    '        Temp_bool_Variable = false;',
    '    }',
    '}',
    '',
    'ExecuteUbergraph_GA_Weapon_Fire(int32 EntryPoint)',
    '{',
    '    // Sequence 0.',
    '    CallFunc_CheckAmmoState_ReturnValue = CheckAmmoState();',
    '}',
].join('\n');

test('parses functions with their parameter lists', () => {
    const model = parseRenderedSource(RENDERED);
    assert.deepEqual(model.functions.map(fn => fn.name), ['SlelectHitMontage', 'ExecuteUbergraph_GA_Weapon_Fire']);
    assert.deepEqual(model.functions[0].parameters, ['HitNormal', 'Hit_Actor']);
    assert.deepEqual(model.functions[1].parameters, ['EntryPoint']);
});

test('a function ends at its closing brace', () => {
    const model = parseRenderedSource(RENDERED);
    assert.equal(model.functions[0].line, 0);
    assert.equal(model.functions[0].endLine, 12, 'closing brace line');
});

test('collects assignment targets and parameters as variables', () => {
    const model = parseRenderedSource(RENDERED);
    const names = model.variables.map(variable => variable.name);
    assert.ok(names.includes('HitNormal'), 'parameter');
    assert.ok(names.includes('Hit_Actor'), 'pointer parameter');
    assert.ok(names.includes('Dot_Product_Forward_Hit'), 'sanitised name');
    assert.ok(names.includes('CallFunc_GetActorForwardVector_ReturnValue'));
    assert.ok(names.includes('Temp_bool_Variable'));
    assert.deepEqual(model.variables.filter(v => v.isParameter).map(v => v.name), ['HitNormal', 'Hit_Actor', 'EntryPoint']);
});

test('comparisons are not assignments', () => {
    const model = parseRenderedSource(RENDERED);
    const names = model.variables.map(variable => variable.name);
    assert.ok(!names.some(name => name.includes('==')), 'no variable captured from an == comparison');
    assert.ok(!names.some(name => /^(if|while|switch|return)$/.test(name)), 'no keywords');
});

test('a variable is defined by its first occurrence', () => {
    const model = parseRenderedSource(RENDERED);
    const repeated = model.variables.filter(variable => variable.name === 'Temp_bool_Variable');
    assert.equal(repeated.length, 1, 'deduplicated');
    assert.equal(repeated[0].line, 6, 'first assignment wins');
});

test('finds the variable under the cursor, longest name first', () => {
    const model = parseRenderedSource(RENDERED);
    const line = '    CallFunc_GetActorForwardVector_ReturnValue = Hit_Actor->GetActorForwardVector();';
    assert.equal(variableAt(model, line, 10).name, 'CallFunc_GetActorForwardVector_ReturnValue');
    assert.equal(variableAt(model, line, 55).name, 'Hit_Actor');
    assert.equal(variableAt(model, '    // Sequence 0.', 8), undefined);
});

test('a spaced spelling is no longer a second way to name a variable', () => {
    const model = parseRenderedSource(RENDERED);
    const spaced = '    CallFunc_GetActorForwardVector_ReturnValue = Hit Actor->GetActorForwardVector();';
    assert.equal(variableAt(model, spaced, 55), undefined, 'the renderer emits Hit_Actor, not "Hit Actor"');
});

test('an empty document parses to nothing', () => {
    const model = parseRenderedSource('');
    assert.deepEqual(model.functions, []);
    assert.deepEqual(model.variables, []);
});
