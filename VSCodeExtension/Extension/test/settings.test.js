// The launcher reads an executable path and its arguments from settings and spawns the executable.
// A workspace-supplied .vscode/settings.json must not be able to redirect either: that would let a
// hostile repository have Start Game and Attach launch an arbitrary binary, or hand engine flags such
// as -ExecCmds to the player's own game. Machine scope keeps both to the user's own settings.
const test = require('node:test');
const assert = require('node:assert/strict');

const packageJson = require('../package.json');

function configurationProperties() {
    const configuration = packageJson.contributes.configuration;
    const entries = Array.isArray(configuration) ? configuration : [configuration];
    return Object.assign({}, ...entries.map(entry => entry.properties || {}));
}

test('the launcher executable and arguments are machine-scoped', () => {
    const properties = configurationProperties();

    for (const name of ['blueprintInspector.gameExecutable', 'blueprintInspector.gameArguments']) {
        assert.ok(properties[name], `${name} is declared in contributes.configuration`);
        assert.equal(properties[name].scope, 'machine', `${name} must be machine-scoped`);
    }
});

test('no launcher setting is reachable from workspace settings', () => {
    const properties = configurationProperties();
    const overridable = Object.entries(properties)
        .filter(([name]) => name.startsWith('blueprintInspector.game'))
        .filter(([, value]) => value.scope === undefined || value.scope === 'window' || value.scope === 'resource')
        .map(([name]) => name);

    assert.deepEqual(overridable, [], 'workspace settings must not reach the launcher');
});
