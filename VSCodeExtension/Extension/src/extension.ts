import * as childProcess from 'child_process';
import * as fs from 'fs';
import * as net from 'net';
import * as path from 'path';
import * as vscode from 'vscode';

import {
    CoverageLine,
    coverageJson as formatCoverageJson,
    coverageLcov as formatCoverageLcov,
} from './coverage';
import { parseRenderedSource, variableAt } from './blueprintSymbols';
import {
    DiffEntry,
    Snapshot,
    SnapshotDiff,
    createSnapshot,
    diffSnapshots,
    parseSnapshot,
} from './snapshot';

const SCHEME = 'blueprint-source';
const DEBUGGER_TYPE = 'blueprint-inspector';

function log(message: string): void {
    console.log(`[BlueprintInspector] ${message}`);
}

interface LoadedSource {
    name: string;
    path: string;
    sourceReference: number;
}

let requestChain: Promise<unknown> = Promise.resolve();

function serialize<T>(work: () => Promise<T>): Promise<T> {
    const result = requestChain.then(work, work);
    requestChain = result.then(() => undefined, () => undefined);
    return result;
}

function activeSession(): vscode.DebugSession | undefined {
    const session = vscode.debug.activeDebugSession;
    return session?.type === DEBUGGER_TYPE ? session : undefined;
}

// cppdap sends no response for a command the loaded plugin does not register, so a stale or
// differently configured module would leave a request pending forever. Bound every custom request and
// name the likely cause when it expires.
function sessionRequest<T>(session: vscode.DebugSession, command: string, argumentsValue?: unknown,
                           timeoutMs = 8000): Promise<T> {
    return new Promise<T>((resolve, reject) => {
        const timer = setTimeout(() => {
            reject(new Error(`${command} timed out after ${Math.round(timeoutMs / 1000)} s. The running ` +
                'BlueprintInspector module may be older or built in a different configuration; check the ' +
                'loaded module path in the game log.'));
        }, timeoutMs);
        session.customRequest(command, argumentsValue).then(
            value => {
                clearTimeout(timer);
                resolve(value as T);
            },
            error => {
                clearTimeout(timer);
                reject(error);
            });
    });
}

async function fetchLoadedSources(session: vscode.DebugSession): Promise<LoadedSource[]> {
    const response = await sessionRequest<{ sources: LoadedSource[] }>(session, 'loadedSources');
    return response.sources;
}

// The adapter writes a fresh token per client connection to Saved/BlueprintInspector/dap-token and to
// %LOCALAPPDATA%/BlueprintInspector/dap-token (the shared copy). Only one process can own the listening
// port, so the shared file is unambiguous, and it is the copy that works for a packaged game whose
// Saved directory is wherever the build was staged. The project copy covers editor sessions and staged
// builds opened at their Windows folder; blueprintInspector.tokenFile overrides both.
function candidateTokenPaths(): string[] {
    const paths: string[] = [];
    const localAppData = process.env.LOCALAPPDATA;

    const configured = vscode.workspace.getConfiguration('blueprintInspector').get<string>('tokenFile');
    if (configured) {
        paths.push(configured);
    }
    if (localAppData) {
        paths.push(path.join(localAppData, 'BlueprintInspector', 'dap-token'));
    }

    const folders = vscode.workspace.workspaceFolders ?? [];
    for (const folder of folders) {
        const root = folder.uri.fsPath;
        paths.push(path.join(root, 'Saved', 'BlueprintInspector', 'dap-token'));

        // A staged build opened at its Windows folder: <root>/<Project>/Saved.
        try {
            for (const name of fs.readdirSync(root)) {
                const child = path.join(root, name);
                if (fs.statSync(child).isDirectory()) {
                    paths.push(path.join(child, 'Saved', 'BlueprintInspector', 'dap-token'));
                }
            }
        } catch {
            // The folder may be unreadable; the other candidates still apply.
        }

        // Source project: walk up to the .uproject.
        let dir: string | undefined = root;
        while (dir) {
            try {
                if (fs.readdirSync(dir).some(name => name.toLowerCase().endsWith('.uproject'))) {
                    paths.push(path.join(dir, 'Saved', 'BlueprintInspector', 'dap-token'));
                    break;
                }
            } catch {
                // Keep walking.
            }
            const parent = path.dirname(dir);
            dir = parent === dir ? undefined : parent;
        }
    }

    return [...new Set(paths)];
}

// A stale token from another project can sit in one of the candidate locations, so take the newest
// file that is not the token we already tried. The adapter writes its token when the client connects.
function newestToken(candidates: string[], previousToken?: string): { tokenPath: string; token: string } | undefined {
    let newest: { tokenPath: string; token: string; modified: number } | undefined;
    for (const tokenPath of candidates) {
        try {
            const token = fs.readFileSync(tokenPath, 'utf8').trim();
            if (!token || token === previousToken) {
                continue;
            }
            const modified = fs.statSync(tokenPath).mtimeMs;
            if (!newest || modified > newest.modified) {
                newest = { tokenPath, token, modified };
            }
        } catch {
            // The accept thread may still be writing it.
        }
    }
    return newest;
}

async function readSessionToken(previousToken?: string): Promise<string | undefined> {
    const candidates = candidateTokenPaths();
    if (candidates.length === 0) {
        return undefined;
    }

    for (let attempt = 0; attempt < 30; ++attempt) {
        const found = newestToken(candidates, previousToken);
        if (found) {
            log(`session token read from ${found.tokenPath}`);
            return found.token;
        }
        await new Promise(resolve => setTimeout(resolve, 100));
    }

    log(`no session token in: ${candidates.join(', ')}`);
    return undefined;
}

// VS Code's debugServer handshake fixes adapterID and sends no launch arguments, so the token is
// presented in a second launch request's __restart payload. The adapter holds data requests until it
// arrives and rejects them after a few seconds, so this has to run before the picker asks for sources.
async function tryAuthenticate(session: vscode.DebugSession, token: string): Promise<string | undefined> {
    try {
        await sessionRequest<unknown>(session, 'launch', { __restart: { token } });
        return undefined;
    } catch (error) {
        return error instanceof Error ? error.message : String(error);
    }
}

async function authenticateSession(session: vscode.DebugSession): Promise<boolean> {
    const first = await readSessionToken();
    if (!first) {
        log('session token was not readable; see Saved/BlueprintInspector/dap-token');
        vscode.window.showErrorMessage(
            'Blueprint Inspector: the adapter did not publish a session token. Start the game first. ' +
            'The token is normally at <Project>/Saved/BlueprintInspector/dap-token, or under ' +
            '%LOCALAPPDATA%/<Project>/Saved for a packaged build. Open the folder that contains ' +
            'Saved as the workspace, or set blueprintInspector.tokenFile.');
        return false;
    }

    log('session token read; authenticating');
    let failure = await tryAuthenticate(session, first);
    if (!failure) {
        log('authenticated');
        return true;
    }

    // A crashed editor can leave the previous token on disk. The adapter writes a fresh token for
    // every connection, so wait for that write and retry once before surfacing the failure.
    log(`authentication failed: ${failure}; waiting for a fresh token`);
    const fresh = await readSessionToken(first);
    if (fresh) {
        log('session token changed; retrying authentication');
        failure = await tryAuthenticate(session, fresh);
        if (!failure) {
            log('authenticated');
            return true;
        }
    }

    log(`authentication failed: ${failure}`);
    vscode.window.showErrorMessage(`Blueprint Inspector: authentication failed: ${failure}`);
    return false;
}

class BlueprintSourceProvider implements vscode.TextDocumentContentProvider {
    async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
        const session = activeSession();
        if (!session) {
            throw new Error('No blueprint-inspector debug session is active.');
        }

        // The query carries the identity; the handle beside it and the document
        // path are only hints for the editor.
        const name = new URLSearchParams(uri.query).get('name');
        if (!name) {
            throw new Error(`'${uri.toString()}' names no blueprint.`);
        }

        return serialize(async () => {
            const sources = await fetchLoadedSources(session);
            const source = sources.find(candidate => candidate.name === name);
            if (!source) {
                throw new Error(`The current session holds no blueprint named '${name}'.`);
            }

            const response = await sessionRequest<{ content: string }>(session, 'source', {
                source: { sourceReference: source.sourceReference },
                sourceReference: source.sourceReference
            }, 30000);

            return response.content;
        });
    }
}

const DAP_PORT = 19021;
function isPortOpen(port: number, timeoutMs = 500): Promise<boolean> {
    return new Promise(resolve => {
        const socket = net.connect({ host: '127.0.0.1', port });
        const finish = (open: boolean): void => {
            socket.removeAllListeners();
            socket.destroy();
            resolve(open);
        };
        socket.setTimeout(timeoutMs, () => finish(false));
        socket.once('connect', () => finish(true));
        socket.once('error', () => finish(false));
    });
}

async function waitForPort(port: number, timeoutMs: number): Promise<boolean> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (await isPortOpen(port)) {
            return true;
        }
        await new Promise(resolve => setTimeout(resolve, 500));
    }
    return false;
}

function splitArguments(value: string): string[] {
    return (value.match(/"[^"]*"|\S+/g) ?? []).map(argument => argument.replace(/^"|"$/g, ''));
}

// Start the packaged game when nothing is listening yet, wait for the adapter, then attach. A game that
// is already listening is reused, which is also the attach path when gameExecutable is empty (the
// setting description promises that).
async function startGameAndAttach(): Promise<void> {
    const configuration = vscode.workspace.getConfiguration('blueprintInspector');
    const executable = configuration.get<string>('gameExecutable', '').trim();

    if (!(await isPortOpen(DAP_PORT))) {
        if (!executable) {
            const choice = await vscode.window.showErrorMessage(
                'Blueprint Inspector: no game is listening on port 19021. Start the game, or set "blueprintInspector.gameExecutable" so this command can launch it.',
                'Open Settings');
            if (choice === 'Open Settings') {
                await vscode.commands.executeCommand('workbench.action.openSettings', 'blueprintInspector.gameExecutable');
            }
            return;
        }
        if (!fs.existsSync(executable)) {
            vscode.window.showErrorMessage(`Blueprint Inspector: the game executable does not exist: ${executable}`);
            return;
        }

        const argumentsList = splitArguments(configuration.get<string>('gameArguments', ''));
        try {
            const child = childProcess.spawn(executable, argumentsList, {
                cwd: path.dirname(executable),
                detached: true,
                stdio: 'ignore',
            });
            child.unref();
            log(`started ${executable}`);
        } catch (error) {
            vscode.window.showErrorMessage(`Blueprint Inspector: could not start the game: ${String(error)}`);
            return;
        }

        const ready = await vscode.window.withProgress(
            { location: vscode.ProgressLocation.Notification, title: 'Waiting for the game to open port 19021…' },
            () => waitForPort(DAP_PORT, 60000));
        if (!ready) {
            vscode.window.showErrorMessage(
                'Blueprint Inspector: the game did not open port 19021 within 60 s. Check that the plugin is enabled in that build.');
            return;
        }
    }

    const started = await vscode.debug.startDebugging(undefined, {
        type: DEBUGGER_TYPE,
        name: 'Attach to Blueprint Inspector server',
        request: 'launch',
        debugServer: DAP_PORT,
    });
    if (!started) {
        vscode.window.showErrorMessage('Blueprint Inspector: the debug session did not start.');
    }
}

interface HotLine {
    className: string;
    functionName: string;
    line: number;
    hits: number;
    kind: string;
}

interface CaptureState {
    capturing: boolean;
    generation: number;
    totalHits: number;
}

interface CaptureMetadata {
    map: string;
    build: string;
    engine: string;
    project: string;
    capturedAt: string;
}

interface HotLinesResponse {
    state: CaptureState;
    metadata?: CaptureMetadata;
    lines: HotLine[];
}

interface AuditFinding {
    rule: string;
    severity: string;
    category: string;
    className: string;
    functionName: string;
    line: number;
    message: string;
    evidence: string;
}

interface AuditResponse {
    findings: AuditFinding[];
    classes: number;
    functions: number;
    truncated: boolean;
}

const HEAT_ALPHAS = [0.10, 0.18, 0.28, 0.40];

let captureState: CaptureState = { capturing: false, generation: 0, totalHits: 0 };
// `captureState.totalHits` is the raw dispatcher count. `resolvedHits` is the sum of the hits that map
// onto rendered lines; the two differ by operand addresses, which own no line.
let resolvedHits = 0;
let hotLines = new Map<string, Map<number, number>>();
let hotLinesTimer: ReturnType<typeof setInterval> | undefined;
let statusBar: vscode.StatusBarItem | undefined;
let heatDecorations: vscode.TextEditorDecorationType[] = [];
const heatChanged = new vscode.EventEmitter<void>();

type HotPathNodeKind = 'class' | 'function' | 'line';

interface HotPathNode {
    kind: HotPathNodeKind;
    className: string;
    functionName?: string;
    line?: number;
    hits: number;
    transfer?: boolean;
}

let hotLineEntries: HotLine[] = [];
// VS Code preserves expansion and selection by element identity. Reuse the node objects across
// refreshes so the 3 s hot-lines poll does not collapse the tree under the user.
const hotPathNodes = new Map<string, HotPathNode>();

function hotPathNode(key: string, create: () => HotPathNode): HotPathNode {
    const existing = hotPathNodes.get(key);
    if (existing) {
        return existing;
    }
    const node = create();
    hotPathNodes.set(key, node);
    return node;
}

function aggregateHotLines(entries: HotLine[], key: (entry: HotLine) => string): Array<[string, number]> {
    const totals = new Map<string, number>();
    for (const entry of entries) {
        const name = key(entry);
        totals.set(name, (totals.get(name) ?? 0) + entry.hits);
    }
    return [...totals.entries()].sort((a, b) => b[1] - a[1]);
}

class HotPathsProvider implements vscode.TreeDataProvider<HotPathNode> {
    private readonly changed = new vscode.EventEmitter<HotPathNode | undefined | void>();
    readonly onDidChangeTreeData = this.changed.event;

    refresh(): void {
        this.changed.fire();
    }

    getTreeItem(node: HotPathNode): vscode.TreeItem {
        const label = node.kind === 'line'
            ? `line ${node.line}`
            : node.kind === 'function' ? (node.functionName ?? '') : node.className;
        const item = new vscode.TreeItem(
            label,
            node.kind === 'line' ? vscode.TreeItemCollapsibleState.None : vscode.TreeItemCollapsibleState.Collapsed);
        item.description = node.kind === 'line'
            ? `${node.hits} hit(s)${node.transfer ? ' · arm' : ''}`
            : `${node.hits} hit(s)`;
        item.iconPath = new vscode.ThemeIcon(
            node.kind === 'class' ? 'symbol-class' : node.kind === 'function' ? 'symbol-method' : 'flame');
        item.tooltip = node.kind === 'line'
            ? `${node.className}::${node.functionName} line ${node.line} · ${node.hits} hit(s)`
            : `${node.className}${node.functionName ? `::${node.functionName}` : ''} · ${node.hits} hit(s)`;
        if (node.kind === 'line') {
            item.command = {
                command: 'blueprintInspector.openHotLine',
                title: 'Open Hot Line',
                arguments: [node],
            };
        }
        return item;
    }

    getChildren(node?: HotPathNode): HotPathNode[] {
        if (!node) {
            return aggregateHotLines(hotLineEntries, entry => entry.className)
                .map(([className, hits]): HotPathNode => {
                    const classNode = hotPathNode(`class:${className}`,
                        () => ({ kind: 'class', className, hits }));
                    classNode.hits = hits;
                    return classNode;
                });
        }
        if (node.kind === 'class') {
            return aggregateHotLines(
                hotLineEntries.filter(entry => entry.className === node.className),
                entry => entry.functionName)
                .map(([functionName, hits]): HotPathNode => {
                    const functionNode = hotPathNode(`function:${node.className}:${functionName}`,
                        () => ({ kind: 'function', className: node.className, functionName, hits }));
                    functionNode.hits = hits;
                    return functionNode;
                });
        }
        if (node.kind === 'function') {
            return hotLineEntries
                .filter(entry => entry.className === node.className && entry.functionName === node.functionName)
                .sort((a, b) => b.hits - a.hits)
                .map((entry): HotPathNode => {
                    const lineNode = hotPathNode(
                        `line:${entry.className}:${entry.functionName}:${entry.line}:${entry.kind}`,
                        () => ({
                            kind: 'line',
                            className: entry.className,
                            functionName: entry.functionName,
                            line: entry.line,
                            hits: entry.hits,
                            transfer: entry.kind === 'transfer',
                        }));
                    lineNode.hits = entry.hits;
                    return lineNode;
                });
        }
        return [];
    }
}

const hotPathsProvider = new HotPathsProvider();

interface AuditNode {
    kind: 'severity' | 'finding' | 'clean';
    key?: string;
    label?: string;
    count?: number;
    finding?: AuditFinding;
}

const AUDIT_SEVERITIES = [
    { key: 'error', label: 'Error' },
    { key: 'warning', label: 'Warning' },
    { key: 'info', label: 'Info' },
];

let auditFindings: AuditFinding[] = [];
let auditMeta = { classes: 0, functions: 0, truncated: false };
let auditRan = false;
const auditNodes = new Map<string, AuditNode>();

function auditSeverity(value: string): string {
    const normalized = (value ?? '').toLowerCase();
    return normalized === 'error' || normalized === 'warning' ? normalized : 'info';
}

function auditLocation(finding: AuditFinding): string {
    if (!finding.className) {
        return finding.category;
    }
    const functionPart = finding.functionName ? `::${finding.functionName}` : '';
    const linePart = finding.line > 0 ? `:${finding.line}` : '';
    return `${finding.className}${functionPart}${linePart}`;
}

class AuditProvider implements vscode.TreeDataProvider<AuditNode> {
    private readonly changed = new vscode.EventEmitter<AuditNode | undefined | void>();
    readonly onDidChangeTreeData = this.changed.event;

    refresh(): void {
        this.changed.fire();
    }

    getTreeItem(node: AuditNode): vscode.TreeItem {
        if (node.kind === 'clean') {
            const item = new vscode.TreeItem('No findings, clean run', vscode.TreeItemCollapsibleState.None);
            item.iconPath = new vscode.ThemeIcon('pass');
            return item;
        }

        if (node.kind === 'severity') {
            const severity = node.key ?? 'info';
            const item = new vscode.TreeItem(`${node.label} (${node.count ?? 0})`,
                vscode.TreeItemCollapsibleState.Collapsed);
            item.iconPath = new vscode.ThemeIcon(
                severity === 'error' ? 'error' : severity === 'warning' ? 'warning' : 'info');
            return item;
        }

        const finding = node.finding!;
        const message = finding.message.length > 100 ? `${finding.message.slice(0, 97)}…` : finding.message;
        const item = new vscode.TreeItem(`[${finding.rule}] ${message}`, vscode.TreeItemCollapsibleState.None);
        item.description = auditLocation(finding);
        const severity = auditSeverity(finding.severity);
        item.iconPath = new vscode.ThemeIcon(
            severity === 'error' ? 'error' : severity === 'warning' ? 'warning' : 'info');
        item.tooltip = new vscode.MarkdownString(
            `**${finding.rule}** · ${finding.category}\n\n${finding.message}` +
            (finding.evidence ? `\n\nEvidence: ${finding.evidence}` : ''));
        if (finding.className) {
            item.command = {
                command: 'blueprintInspector.openAuditFinding',
                title: 'Open Finding',
                arguments: [finding],
            };
        }
        return item;
    }

    getChildren(node?: AuditNode): AuditNode[] {
        if (!node) {
            if (!auditRan) {
                return [];
            }
            if (auditFindings.length === 0) {
                return [this.node('clean', () => ({ kind: 'clean' }))];
            }
            return AUDIT_SEVERITIES
                .map(severity => {
                    const count = auditFindings.filter(finding =>
                        auditSeverity(finding.severity) === severity.key).length;
                    if (count === 0) {
                        return undefined;
                    }
                    const severityNode = this.node(`severity:${severity.key}`,
                        () => ({ kind: 'severity', key: severity.key, label: severity.label, count }));
                    severityNode.count = count;
                    return severityNode;
                })
                .filter((value): value is AuditNode => value !== undefined);
        }

        if (node.kind === 'severity') {
            return auditFindings
                .filter(finding => auditSeverity(finding.severity) === node.key)
                .map((finding, index) => this.node(
                    `finding:${node.key}:${finding.rule}:${finding.className}:${finding.functionName}:${finding.line}:${index}`,
                    () => ({ kind: 'finding', finding })));
        }

        return [];
    }

    private node(key: string, create: () => AuditNode): AuditNode {
        const existing = auditNodes.get(key);
        if (existing) {
            return existing;
        }
        const created = create();
        auditNodes.set(key, created);
        return created;
    }
}

const auditProvider = new AuditProvider();

interface CoverageResponse {
    state: CaptureState;
    lines: CoverageLine[];
    renderedClasses: number;
    totalLines: number;
    coveredLines: number;
    uncoveredLines: number;
    truncated: boolean;
}

type CoverageNodeKind = 'class' | 'function' | 'line' | 'empty';

interface CoverageNode {
    kind: CoverageNodeKind;
    className?: string;
    functionName?: string;
    line?: number;
    label?: string;
    hits?: number;
    transfer?: boolean;
    covered?: number;
    total?: number;
}

let coverageLines: CoverageLine[] = [];
let coverageCapture: CaptureState = { capturing: false, generation: 0, totalHits: 0 };
let coverageMeta = { renderedClasses: 0, totalLines: 0, coveredLines: 0, uncoveredLines: 0, truncated: false };
let coverageRan = false;
// The view lists every statement by default; the toggle narrows it to the ones that never ran.
let coverageUncoveredOnly = false;
const coverageNodes = new Map<string, CoverageNode>();

function coveragePercent(covered: number, total: number): string {
    return total > 0 ? `${((covered / total) * 100).toFixed(1)}%` : 'n/a';
}

function coverageSpan(lines: CoverageLine[]): { covered: number; total: number } {
    let covered = 0;
    for (const line of lines) {
        if (line.hits > 0) {
            covered++;
        }
    }
    return { covered, total: lines.length };
}

class CoverageProvider implements vscode.TreeDataProvider<CoverageNode> {
    private readonly changed = new vscode.EventEmitter<CoverageNode | undefined | void>();
    readonly onDidChangeTreeData = this.changed.event;

    refresh(): void {
        this.changed.fire();
    }

    getTreeItem(node: CoverageNode): vscode.TreeItem {
        if (node.kind === 'empty') {
            const item = new vscode.TreeItem(node.label ?? '', vscode.TreeItemCollapsibleState.None);
            item.iconPath = new vscode.ThemeIcon('info');
            return item;
        }

        if (node.kind === 'line') {
            const covered = (node.hits ?? 0) > 0;
            const item = new vscode.TreeItem(`line ${node.line}`, vscode.TreeItemCollapsibleState.None);
            item.description = covered
                ? `${node.hits} hit(s)${node.transfer ? ' · arm' : ''}`
                : `not executed${node.transfer ? ' · arm' : ''}`;
            item.iconPath = covered
                ? new vscode.ThemeIcon('pass')
                : new vscode.ThemeIcon('circle-slash', new vscode.ThemeColor('problemsWarningIcon.foreground'));
            item.tooltip = `${node.className}::${node.functionName} line ${node.line} · ` +
                (covered ? `${node.hits} hit(s)` : 'never executed') + (node.transfer ? ' · break/continue arm' : '');
            item.command = {
                command: 'blueprintInspector.openCoverageLine',
                title: 'Open Coverage Line',
                arguments: [node],
            };
            return item;
        }

        const label = node.kind === 'class' ? node.className ?? '' : node.functionName ?? '';
        const item = new vscode.TreeItem(
            `${label} (${node.covered ?? 0}/${node.total ?? 0})`, vscode.TreeItemCollapsibleState.Collapsed);
        item.description = coveragePercent(node.covered ?? 0, node.total ?? 0);
        item.iconPath = new vscode.ThemeIcon(node.kind === 'class' ? 'symbol-class' : 'symbol-method');
        item.tooltip = `${node.kind === 'class' ? node.className : `${node.className}::${node.functionName}`} · ` +
            `${node.covered ?? 0} of ${node.total ?? 0} statement(s) executed`;
        return item;
    }

    getChildren(node?: CoverageNode): CoverageNode[] {
        if (!node) {
            if (!coverageRan) {
                return [];
            }
            if (coverageLines.length === 0) {
                return [this.node('empty', () => ({
                    kind: 'empty',
                    label: coverageMeta.renderedClasses > 0
                        ? 'No breakpointable statements in the rendered classes'
                        : 'No rendered class yet. Resolve blueprints, then refresh',
                }))];
            }
            const byClass = new Map<string, CoverageLine[]>();
            for (const line of coverageLines) {
                const group = byClass.get(line.className);
                if (group) {
                    group.push(line);
                } else {
                    byClass.set(line.className, [line]);
                }
            }
            return [...byClass.entries()]
                .map(([className, lines]): CoverageNode => {
                    const span = coverageSpan(lines);
                    const classNode = this.node(`class:${className}`,
                        () => ({ kind: 'class', className, covered: span.covered, total: span.total }));
                    classNode.covered = span.covered;
                    classNode.total = span.total;
                    return classNode;
                })
                // Least-covered class first: that is the one a playtest left untested.
                .sort((a, b) => (a.covered ?? 0) / (a.total || 1) - (b.covered ?? 0) / (b.total || 1));
        }

        if (node.kind === 'class') {
            const byFunction = new Map<string, CoverageLine[]>();
            for (const line of coverageLines) {
                if (line.className !== node.className) {
                    continue;
                }
                const group = byFunction.get(line.functionName);
                if (group) {
                    group.push(line);
                } else {
                    byFunction.set(line.functionName, [line]);
                }
            }
            return [...byFunction.entries()]
                .sort((a, b) => a[0].localeCompare(b[0]))
                .map(([functionName, lines]): CoverageNode => {
                    const span = coverageSpan(lines);
                    const functionNode = this.node(`function:${node.className}:${functionName}`,
                        () => ({ kind: 'function', className: node.className, functionName, covered: span.covered, total: span.total }));
                    functionNode.covered = span.covered;
                    functionNode.total = span.total;
                    return functionNode;
                });
        }

        if (node.kind === 'function') {
            return coverageLines
                .filter(line => line.className === node.className && line.functionName === node.functionName)
                .filter(line => !coverageUncoveredOnly || line.hits === 0)
                .sort((a, b) => a.line - b.line)
                .map((entry): CoverageNode => {
                    const lineNode = this.node(
                        `line:${entry.className}:${entry.functionName}:${entry.line}:${entry.kind}`,
                        () => ({
                            kind: 'line',
                            className: entry.className,
                            functionName: entry.functionName,
                            line: entry.line,
                            hits: entry.hits,
                            transfer: entry.kind === 'transfer',
                        }));
                    lineNode.hits = entry.hits;
                    return lineNode;
                });
        }

        return [];
    }

    private node(key: string, create: () => CoverageNode): CoverageNode {
        const existing = coverageNodes.get(key);
        if (existing) {
            return existing;
        }
        const created = create();
        coverageNodes.set(key, created);
        return created;
    }
}

const coverageProvider = new CoverageProvider();

function coverageSummary(): string {
    const percent = coveragePercent(coverageMeta.coveredLines, coverageMeta.totalLines);
    return `${coverageMeta.coveredLines}/${coverageMeta.totalLines} statement(s) covered (${percent}) ` +
        `across ${coverageMeta.renderedClasses} rendered class(es)`;
}

async function refreshCoverage(announce = true): Promise<void> {
    const session = activeSession();
    if (!session) {
        if (announce) {
            vscode.window.showInformationMessage('Start a Blueprint Inspector debug session first.');
        }
        return;
    }
    // The adapter resolves against cached renders; the cap matches what a large project can hold.
    const response = await sessionRequest<CoverageResponse>(
        session, 'blueprintInspector.coverage', { maxLines: 50000 }, 120000);
    coverageLines = response.lines ?? [];
    coverageCapture = response.state ?? { capturing: false, generation: 0, totalHits: 0 };
    coverageMeta = {
        renderedClasses: response.renderedClasses ?? 0,
        totalLines: response.totalLines ?? coverageLines.length,
        coveredLines: response.coveredLines ?? 0,
        uncoveredLines: response.uncoveredLines ?? 0,
        truncated: response.truncated === true,
    };
    coverageRan = true;
    // Keep the node objects: VS Code preserves expansion and selection by identity, and a refresh
    // (manual, or on stop) should not collapse the tree under the user.
    coverageProvider.refresh();
    log(`coverage: ${coverageSummary()}, ${coverageLines.length} line(s) returned` +
        `${coverageMeta.truncated ? ' (truncated)' : ''}`);
    if (announce) {
        vscode.window.showInformationMessage(
            `Blueprint coverage: ${coverageSummary()}${coverageMeta.truncated ? ' (truncated)' : ''}.`);
    }
}

function clearCoverage(): void {
    coverageLines = [];
    coverageCapture = { capturing: false, generation: 0, totalHits: 0 };
    coverageMeta = { renderedClasses: 0, totalLines: 0, coveredLines: 0, uncoveredLines: 0, truncated: false };
    coverageRan = false;
    coverageNodes.clear();
    coverageProvider.refresh();
}

async function toggleCoverageUncoveredOnly(): Promise<void> {
    coverageUncoveredOnly = !coverageUncoveredOnly;
    await vscode.commands.executeCommand(
        'setContext', 'blueprintInspector.coverageUncoveredOnly', coverageUncoveredOnly);
    coverageProvider.refresh();
}

function coverageJson(): string {
    return formatCoverageJson(coverageCapture, coverageMeta, coverageLines);
}

function coverageLcov(): string {
    return formatCoverageLcov(coverageLines);
}

async function ensureCoverage(): Promise<boolean> {
    if (!coverageRan) {
        await refreshCoverage();
    }
    return coverageRan;
}

async function exportCoverageJson(): Promise<void> {
    if (!await ensureCoverage()) {
        return;
    }
    const folder = vscode.workspace.workspaceFolders?.[0]?.uri;
    const target = await vscode.window.showSaveDialog({
        defaultUri: folder ? vscode.Uri.joinPath(folder, 'blueprint-coverage.json') : undefined,
        filters: { JSON: ['json'] },
    });
    if (!target) {
        return;
    }
    await vscode.workspace.fs.writeFile(target, Buffer.from(coverageJson(), 'utf8'));
    vscode.window.showInformationMessage(`Blueprint coverage written to ${target.fsPath}.`);
}

async function exportCoverageLcov(): Promise<void> {
    if (!await ensureCoverage()) {
        return;
    }
    const folder = vscode.workspace.workspaceFolders?.[0]?.uri;
    const target = await vscode.window.showSaveDialog({
        defaultUri: folder ? vscode.Uri.joinPath(folder, 'blueprint-coverage.lcov') : undefined,
        filters: { LCOV: ['lcov', 'info'] },
    });
    if (!target) {
        return;
    }
    await vscode.workspace.fs.writeFile(target, Buffer.from(coverageLcov(), 'utf8'));
    vscode.window.showInformationMessage(`Blueprint coverage written to ${target.fsPath}.`);
}

async function openCoverageLine(node: CoverageNode): Promise<void> {
    if (node?.kind !== 'line' || !node.className || node.line === undefined) {
        return;
    }
    await openBlueprintAt(node.className, node.line);
}

type SnapshotNodeKind = 'summary' | 'class' | 'function' | 'line';

interface SnapshotNode {
    kind: SnapshotNodeKind;
    className?: string;
    functionName?: string;
    entry?: DiffEntry;
    label?: string;
    description?: string;
    tooltip?: string;
}

let snapshotDiff: SnapshotDiff | undefined;
let snapshotDiffPath: string | undefined;
const snapshotNodes = new Map<string, SnapshotNode>();

function diffDescription(entry: DiffEntry): string {
    switch (entry.change) {
        case 'regressed':
            return `+${entry.delta} hit(s) (${entry.before} → ${entry.after})`;
        case 'improved':
            return `${entry.delta} hit(s) (${entry.before} → ${entry.after})`;
        case 'added':
            return `new · ${entry.after} hit(s)`;
        case 'removed':
            return `gone · was ${entry.before} hit(s)`;
    }
}

function diffIcon(entry: DiffEntry): vscode.ThemeIcon {
    switch (entry.change) {
        case 'regressed':
            return new vscode.ThemeIcon('arrow-up', new vscode.ThemeColor('problemsErrorIcon.foreground'));
        case 'improved':
            return new vscode.ThemeIcon('arrow-down', new vscode.ThemeColor('charts.green'));
        case 'added':
            return new vscode.ThemeIcon('add');
        default:
            return new vscode.ThemeIcon('trash');
    }
}

function describeRun(snapshot: Snapshot): string {
    const map = snapshot.metadata?.map || 'unknown map';
    const build = snapshot.metadata?.build || 'unknown build';
    const when = snapshot.created || snapshot.metadata?.capturedAt || 'unknown date';
    return `${map} · ${build} · ${when}`;
}

class SnapshotDiffProvider implements vscode.TreeDataProvider<SnapshotNode> {
    private readonly changed = new vscode.EventEmitter<SnapshotNode | undefined | void>();
    readonly onDidChangeTreeData = this.changed.event;

    refresh(): void {
        this.changed.fire();
    }

    getTreeItem(node: SnapshotNode): vscode.TreeItem {
        if (node.kind === 'line') {
            const entry = node.entry!;
            const item = new vscode.TreeItem(`line ${entry.line}`, vscode.TreeItemCollapsibleState.None);
            item.description = diffDescription(entry);
            item.iconPath = diffIcon(entry);
            item.tooltip = `${entry.className}::${entry.functionName} line ${entry.line} · ${entry.change}` +
                (entry.kind === 'transfer' ? ' · break/continue arm' : '');
            item.command = {
                command: 'blueprintInspector.openSnapshotDiffLine',
                title: 'Open Diff Line',
                arguments: [node],
            };
            return item;
        }

        const item = new vscode.TreeItem(node.label ?? '', vscode.TreeItemCollapsibleState.Expanded);
        item.description = node.description ?? '';
        if (node.kind === 'summary') {
            item.iconPath = new vscode.ThemeIcon('diff');
            item.tooltip = node.tooltip ?? '';
        } else {
            item.iconPath = new vscode.ThemeIcon(node.kind === 'class' ? 'symbol-class' : 'symbol-method');
        }
        return item;
    }

    getChildren(node?: SnapshotNode): SnapshotNode[] {
        const diff = snapshotDiff;
        if (!diff) {
            return [];
        }

        if (!node) {
            const summary = this.node('summary', () => ({
                kind: 'summary',
                label: `Diff: ${diff.regressed} regressed · ${diff.improved} improved`,
                description: `+${diff.added} new · -${diff.removed} gone · ${diff.unchanged} unchanged`,
                tooltip: `Before: ${describeRun(diff.before)}\nAfter: ${describeRun(diff.after)}\n` +
                    `Hits: ${diff.beforeHits} → ${diff.afterHits}` +
                    (snapshotDiffPath ? `\nSnapshot: ${snapshotDiffPath}` : ''),
            }));
            return [summary];
        }

        if (node.kind === 'summary') {
            const classes = new Map<string, DiffEntry[]>();
            for (const entry of diff.entries) {
                const group = classes.get(entry.className);
                if (group) {
                    group.push(entry);
                } else {
                    classes.set(entry.className, [entry]);
                }
            }
            return [...classes.entries()].map(([className, entries]) => {
                const classNode = this.node(`class:${className}`, () => ({
                    kind: 'class',
                    className,
                    label: className,
                    description: `${entries.length} change(s)`,
                }));
                classNode.description = `${entries.length} change(s)`;
                return classNode;
            });
        }

        if (node.kind === 'class') {
            const functions = new Map<string, DiffEntry[]>();
            for (const entry of diff.entries) {
                if (entry.className !== node.className) {
                    continue;
                }
                const group = functions.get(entry.functionName);
                if (group) {
                    group.push(entry);
                } else {
                    functions.set(entry.functionName, [entry]);
                }
            }
            return [...functions.entries()].map(([functionName, entries]) => {
                const functionNode = this.node(`function:${node.className}:${functionName}`, () => ({
                    kind: 'function',
                    className: node.className,
                    functionName,
                    label: functionName,
                    description: `${entries.length} change(s)`,
                }));
                functionNode.description = `${entries.length} change(s)`;
                return functionNode;
            });
        }

        return diff.entries
            .filter(entry => entry.className === node.className && entry.functionName === node.functionName)
            .map(entry => this.node(
                `line:${entry.className}:${entry.functionName}:${entry.line}:${entry.kind}:${entry.change}`,
                () => ({ kind: 'line', className: entry.className, functionName: entry.functionName, entry })));
    }

    private node(key: string, create: () => SnapshotNode): SnapshotNode {
        const existing = snapshotNodes.get(key);
        if (existing) {
            return existing;
        }
        const created = create();
        snapshotNodes.set(key, created);
        return created;
    }
}

const snapshotDiffProvider = new SnapshotDiffProvider();

function extensionVersion(): string {
    const version = vscode.extensions.getExtension('HexTank.blueprint-inspector')?.packageJSON?.version;
    return typeof version === 'string' ? version : 'unknown';
}

// A snapshot is the capture as it stands now: the counters the Hot Paths view polls, requested with a
// far higher cap so a saved run is not cut down to the view's page size.
async function fetchSnapshot(session: vscode.DebugSession): Promise<Snapshot> {
    const response = await sessionRequest<HotLinesResponse>(
        session, 'blueprintInspector.hotLines', { maxLines: 20000 }, 120000);
    return createSnapshot({
        capture: response.state ?? { capturing: false, generation: 0, totalHits: 0 },
        metadata: response.metadata ?? { map: '', build: '', engine: '', project: '', capturedAt: '' },
        lines: response.lines ?? [],
        extensionVersion: extensionVersion(),
    });
}

async function saveSnapshot(): Promise<void> {
    const session = activeSession();
    if (!session) {
        vscode.window.showInformationMessage('Start a Blueprint Inspector debug session first.');
        return;
    }
    const snapshot = await fetchSnapshot(session);
    if (snapshot.lines.length === 0) {
        vscode.window.showWarningMessage(
            'Blueprint Inspector: the capture has no rendered lines yet. Start a capture, play, and open the blueprints first.');
        return;
    }

    const folder = vscode.workspace.workspaceFolders?.[0]?.uri;
    const map = (snapshot.metadata.map || 'run').replace(/[^\w.-]+/g, '_');
    const stamp = snapshot.created.replace(/[:.]/g, '-');
    const target = await vscode.window.showSaveDialog({
        defaultUri: folder
            ? vscode.Uri.joinPath(folder, `blueprint-capture-${map}-${stamp}.json`)
            : undefined,
        filters: { 'Capture snapshot': ['json'] },
    });
    if (!target) {
        return;
    }
    await vscode.workspace.fs.writeFile(target, Buffer.from(JSON.stringify(snapshot, null, 2), 'utf8'));
    vscode.window.showInformationMessage(
        `Capture snapshot written to ${target.fsPath} (${snapshot.totals.lines} line(s), ${snapshot.totals.hits} hit(s)).`);
}

async function compareSnapshot(): Promise<void> {
    const session = activeSession();
    if (!session) {
        vscode.window.showInformationMessage('Start a Blueprint Inspector debug session first.');
        return;
    }

    const picked = await vscode.window.showOpenDialog({
        canSelectMany: false,
        filters: { 'Capture snapshot': ['json'] },
        openLabel: 'Compare With This Run',
    });
    if (!picked || picked.length === 0) {
        return;
    }

    let text: string;
    try {
        text = Buffer.from(await vscode.workspace.fs.readFile(picked[0])).toString('utf8');
    } catch (error) {
        vscode.window.showErrorMessage(
            `Blueprint Inspector: could not read ${picked[0].fsPath}: ${error instanceof Error ? error.message : String(error)}`);
        return;
    }
    const parsed = parseSnapshot(text);
    if (!parsed.snapshot) {
        vscode.window.showErrorMessage(
            `Blueprint Inspector: ${picked[0].fsPath} is not a usable snapshot: ${parsed.error}`);
        return;
    }

    const current = await fetchSnapshot(session);
    snapshotDiff = diffSnapshots(parsed.snapshot, current);
    snapshotDiffPath = picked[0].fsPath;
    snapshotNodes.clear();
    snapshotDiffProvider.refresh();

    const diff = snapshotDiff;
    const targets = `${describeRun(diff.before)} → ${describeRun(diff.after)}`;
    if (diff.entries.length === 0) {
        vscode.window.showInformationMessage(
            `Capture diff: no changes across ${diff.unchanged} line(s) (${targets}).`);
        return;
    }
    vscode.window.showInformationMessage(
        `Capture diff: ${diff.regressed} regressed, ${diff.improved} improved, ${diff.added} new, ` +
        `${diff.removed} gone (${targets}).`);
}

function clearSnapshotDiff(): void {
    snapshotDiff = undefined;
    snapshotDiffPath = undefined;
    snapshotNodes.clear();
    snapshotDiffProvider.refresh();
}

async function openSnapshotDiffLine(node: SnapshotNode): Promise<void> {
    if (node?.kind !== 'line' || !node.className || node.entry === undefined) {
        return;
    }
    await openBlueprintAt(node.className, node.entry.line);
}

function classNameOf(document: vscode.TextDocument): string | undefined {
    if (document.uri.scheme !== SCHEME) {
        return undefined;
    }
    const named = new URLSearchParams(document.uri.query).get('name');
    if (named) {
        return named;
    }
    // Documents opened by VS Code's own debug UI use the DAP source path
    // (`blueprint-source:Blueprints/<Class>.cpp`) rather than our query.
    const fromPath = /Blueprints\/([^/]+)\.cpp$/.exec(document.uri.path);
    return fromPath ? fromPath[1] : undefined;
}

function ensureHeatDecorations(): void {
    if (heatDecorations.length > 0) {
        return;
    }
    heatDecorations = HEAT_ALPHAS.map(alpha => vscode.window.createTextEditorDecorationType({
        isWholeLine: true,
        backgroundColor: `rgba(255, 132, 0, ${alpha})`,
        overviewRulerColor: 'rgba(255, 132, 0, 0.8)',
        overviewRulerLane: vscode.OverviewRulerLane.Right,
    }));
}

function clearHeat(): void {
    hotLines = new Map();
    resolvedHits = 0;
    hotLineEntries = [];
    hotPathNodes.clear();
    hotPathsProvider.refresh();
    for (const editor of vscode.window.visibleTextEditors) {
        for (const decoration of heatDecorations) {
            editor.setDecorations(decoration, []);
        }
    }
    heatChanged.fire();
}

function applyHeat(): void {
    ensureHeatDecorations();
    for (const editor of vscode.window.visibleTextEditors) {
        const name = classNameOf(editor.document);
        if (!name) {
            continue;
        }
        const hits = hotLines.get(name);
        const buckets: vscode.Range[][] = heatDecorations.map(() => []);
        if (hits && hits.size > 0) {
            const max = Math.max(...hits.values(), 1);
            for (const [line, count] of hits) {
                if (line < 1 || line > editor.document.lineCount) {
                    continue;
                }
                const t = Math.log1p(count) / Math.log1p(Math.max(max, 1));
                const bucket = Math.min(heatDecorations.length - 1, Math.floor(t * heatDecorations.length));
                buckets[bucket].push(new vscode.Range(line - 1, 0, line - 1, editor.document.lineAt(line - 1).text.length));
            }
        }
        heatDecorations.forEach((decoration, index) => editor.setDecorations(decoration, buckets[index]));
    }
    heatChanged.fire();
}

function updateCaptureUi(): void {
    void vscode.commands.executeCommand('setContext', 'blueprintInspector.capturing', captureState.capturing);
    if (!statusBar) {
        return;
    }
    if (captureState.capturing) {
        statusBar.text = `$(debug-stop) Blueprint capture · ${resolvedHits}`;
        statusBar.tooltip = resolvedHits > 0
            ? `${resolvedHits} hit(s) on rendered lines, ${captureState.totalHits} dispatches total. Click to stop.`
            : captureState.totalHits > 0
                ? `${captureState.totalHits} dispatch(es) counted, but no rendered line yet. Open a blueprint source. Click to stop.`
                : 'Capturing, but no Blueprint statement has run yet. Only executed statements count. Click to stop.';
        statusBar.command = 'blueprintInspector.stopCapture';
    } else {
        statusBar.text = '$(record) Blueprint capture';
        statusBar.tooltip = resolvedHits > 0
            ? `${resolvedHits} hit(s) on rendered lines, ${captureState.totalHits} dispatches from the last capture. Click to start a new one.`
            : captureState.totalHits > 0
                ? `${captureState.totalHits} dispatch(es) from the last capture; no rendered line to show. Click to start.`
                : 'Count Blueprint statement hits for this session. Click to start.';
        statusBar.command = 'blueprintInspector.startCapture';
    }
}

function setCapturing(capturing: boolean): void {
    captureState = { ...captureState, capturing };
    if (hotLinesTimer) {
        clearInterval(hotLinesTimer);
        hotLinesTimer = undefined;
    }
    if (capturing) {
        hotLinesTimer = setInterval(
            () => void refreshHotLines().catch(error => log(`hot lines refresh failed: ${String(error)}`)),
            3000);
    }
    updateCaptureUi();
}

async function refreshHotLines(): Promise<void> {
    const session = activeSession();
    if (!session) {
        return;
    }
    const [response, state] = await Promise.all([
        sessionRequest<HotLinesResponse>(session, 'blueprintInspector.hotLines', { maxLines: 2000 }),
        sessionRequest<CaptureState>(session, 'blueprintInspector.captureState'),
    ]);
    // The hotLines state reports the resolved sum; captureState is the raw dispatcher count. Ask for
    // both so "0" cannot mean either "nothing ran" or "nothing rendered".
    captureState = state;
    resolvedHits = response.state.totalHits;
    const next = new Map<string, Map<number, number>>();
    for (const line of response.lines) {
        let byLine = next.get(line.className);
        if (!byLine) {
            byLine = new Map<number, number>();
            next.set(line.className, byLine);
        }
        byLine.set(line.line, (byLine.get(line.line) ?? 0) + line.hits);
    }
    hotLines = next;
    hotLineEntries = response.lines;
    hotPathsProvider.refresh();
    log(`hot lines: ${response.lines.length} line(s), ${resolvedHits} resolved hit(s), ${state.totalHits} dispatch(es), ${next.size} class(es)`);
    updateCaptureUi();
    applyHeat();
}

let pendingVisibleRefresh: ReturnType<typeof setTimeout> | undefined;

function scheduleVisibleRefresh(): void {
    if (!captureState.capturing || pendingVisibleRefresh) {
        return;
    }
    pendingVisibleRefresh = setTimeout(() => {
        pendingVisibleRefresh = undefined;
        void refreshHotLines().catch(error => log(`hot lines refresh failed: ${String(error)}`));
    }, 150);
}

async function openBlueprintAt(className: string, line = 1): Promise<void> {
    // The DAP source path is what the adapter derives breakpoint identity from
    // (`blueprint-source:Blueprints/<Class>.cpp`); the query is only how the provider finds the class.
    // Opening any other shape leaves gutter breakpoints unverifiable in the adapter.
    const uri = vscode.Uri.parse(
        `${SCHEME}:Blueprints/${encodeURIComponent(className)}.cpp?${new URLSearchParams({ name: className }).toString()}`);
    const document = await vscode.workspace.openTextDocument(uri);
    const editor = await vscode.window.showTextDocument(document, { preview: false });
    const target = Math.max(0, Math.min(line - 1, Math.max(0, document.lineCount - 1)));
    const position = new vscode.Position(target, 0);
    editor.selection = new vscode.Selection(position, position);
    editor.revealRange(new vscode.Range(position, position), vscode.TextEditorRevealType.InCenter);
}

let resolvingAll = false;

// Render every loaded blueprint so hotLines can resolve classes the user has not opened. Work runs in
// serialized batches: fetching the source list invalidates the handles issued before it, so a batch
// lists, uses its handles, and then yields the chain to the editor before the next batch.
async function resolveAllHotPaths(silent = false): Promise<void> {
    const session = activeSession();
    if (!session) {
        if (!silent) {
            vscode.window.showInformationMessage('Start a Blueprint Inspector debug session first.');
        }
        return;
    }
    if (resolvingAll) {
        return;
    }
    resolvingAll = true;

    const seen = new Set<string>();
    let total = 0;
    let resolved = 0;
    let failed = 0;

    const run = async (progress?: vscode.Progress<{ message?: string }>,
                        token?: vscode.CancellationToken): Promise<void> => {
        while (!token?.isCancellationRequested && activeSession() === session) {
            const pending = await serialize(async () => {
                const sources = await fetchLoadedSources(session);
                total = sources.length;
                const batch = sources.filter(source => !seen.has(source.name)).slice(0, 25);
                for (const source of batch) {
                    seen.add(source.name);
                    try {
                        await sessionRequest<{ content: string }>(session, 'source', {
                            source: { name: source.name, sourceReference: source.sourceReference },
                            sourceReference: source.sourceReference,
                        }, 30000);
                        resolved++;
                    } catch (error) {
                        failed++;
                        log(`hot paths resolve failed for ${source.name}: ${String(error)}`);
                    }
                }
                return batch.length;
            });
            progress?.report({ message: `${resolved}/${total} blueprint(s)` });
            if (pending === 0) {
                break;
            }
        }
    };

    try {
        if (silent) {
            await run();
        } else {
            await vscode.window.withProgress({
                location: vscode.ProgressLocation.Notification,
                title: 'Resolving blueprints for hot paths…',
                cancellable: true,
            }, (progress, token) => run(progress, token));
        }
        log(`hot paths: resolved ${resolved} blueprint(s), ${failed} failed`);
        if (!silent && failed > 0) {
            vscode.window.showWarningMessage(
                `Blueprint Inspector: resolved ${resolved} blueprint(s); ${failed} could not be read.`);
        }
        await refreshHotLines();
    } catch (error) {
        if (silent) {
            log(`hot paths resolve failed: ${String(error)}`);
        } else {
            vscode.window.showErrorMessage(
                `Blueprint Inspector: ${error instanceof Error ? error.message : String(error)}`);
        }
    } finally {
        resolvingAll = false;
    }
}

let auditRunning = false;

async function runAudit(): Promise<void> {
    const session = activeSession();
    if (!session) {
        vscode.window.showInformationMessage('Start a Blueprint Inspector debug session first.');
        return;
    }
    if (auditRunning) {
        return;
    }
    auditRunning = true;
    try {
        await vscode.window.withProgress({
            location: vscode.ProgressLocation.Notification,
            title: 'Auditing blueprints…',
            cancellable: false,
        }, async () => {
            const response = await sessionRequest<AuditResponse>(
                session, 'blueprintInspector.audit', { maxFindings: 1000 }, 120000);
            auditFindings = response.findings ?? [];
            auditMeta = {
                classes: response.classes ?? 0,
                functions: response.functions ?? 0,
                truncated: response.truncated === true,
            };
            auditRan = true;
            auditNodes.clear();
            auditProvider.refresh();

            const errors = auditFindings.filter(f => auditSeverity(f.severity) === 'error').length;
            const warnings = auditFindings.filter(f => auditSeverity(f.severity) === 'warning').length;
            vscode.window.showInformationMessage(
                `Blueprint audit: ${errors} error(s), ${warnings} warning(s), ` +
                `${auditFindings.length - errors - warnings} info across ${auditMeta.classes} class(es)` +
                `${auditMeta.truncated ? ' (truncated)' : ''}.`);
        });
    } catch (error) {
        vscode.window.showErrorMessage(
            `Blueprint Inspector: ${error instanceof Error ? error.message : String(error)}`);
    } finally {
        auditRunning = false;
    }
}

function auditMarkdown(): string {
    const lines: string[] = ['# Blueprint audit', ''];
    lines.push(`Generated: ${new Date().toISOString()}`);
    lines.push(`Classes scanned: ${auditMeta.classes}; functions: ${auditMeta.functions}; ` +
        `findings: ${auditFindings.length}${auditMeta.truncated ? ' (truncated)' : ''}`, '');

    for (const severity of AUDIT_SEVERITIES) {
        const group = auditFindings.filter(finding => auditSeverity(finding.severity) === severity.key);
        if (group.length === 0) {
            continue;
        }
        lines.push(`## ${severity.label} (${group.length})`, '');
        for (const finding of group) {
            lines.push(`### [${finding.rule}] ${finding.message}`, '');
            lines.push(`- Category: ${finding.category}`);
            if (finding.className) {
                lines.push(`- Location: ${auditLocation(finding)}`);
            }
            if (finding.evidence) {
                lines.push(`- Evidence: ${finding.evidence}`);
            }
            lines.push('');
        }
    }

    if (auditFindings.length === 0) {
        lines.push('No findings.', '');
    }
    return lines.join('\n');
}

async function exportAudit(): Promise<void> {
    if (!auditRan) {
        vscode.window.showInformationMessage('Run an audit first.');
        return;
    }
    const folder = vscode.workspace.workspaceFolders?.[0]?.uri;
    const target = await vscode.window.showSaveDialog({
        defaultUri: folder ? vscode.Uri.joinPath(folder, 'blueprint-audit.md') : undefined,
        filters: { Markdown: ['md'] },
    });
    if (!target) {
        return;
    }
    await vscode.workspace.fs.writeFile(target, Buffer.from(auditMarkdown(), 'utf8'));
    vscode.window.showInformationMessage(`Blueprint audit written to ${target.fsPath}.`);
}

async function openAuditFinding(finding: AuditFinding): Promise<void> {
    if (!finding?.className) {
        return;
    }
    await openBlueprintAt(finding.className, finding.line > 0 ? finding.line : 1);
}

async function runCaptureCommand(command: 'startCapture' | 'stopCapture' | 'resetCapture'): Promise<void> {
    const session = activeSession();
    if (!session) {
        vscode.window.showInformationMessage('Start a Blueprint Inspector debug session first.');
        return;
    }
    try {
        captureState = await sessionRequest<CaptureState>(session, `blueprintInspector.${command}`);
        if (command === 'startCapture' || command === 'resetCapture') {
            clearHeat();
        }
        if (command === 'resetCapture') {
            // The counters are gone, so a previous snapshot would now say "nothing ran" about a run
            // that did run. Drop it and let the next refresh rebuild it.
            clearCoverage();
        }
        setCapturing(captureState.capturing);
        if (command === 'startCapture') {
            vscode.window.showInformationMessage(
                'Blueprint capture started. Only executed statements are counted; open a blueprint source to see the heat.');
        }
        await refreshHotLines();
        if (command === 'stopCapture') {
            vscode.window.showInformationMessage(
                `Blueprint capture stopped: ${resolvedHits} hit(s) on rendered lines, ${captureState.totalHits} dispatch(es) total.`);
            if (coverageRan) {
                // Refresh an open coverage snapshot so it reflects the run that just ended.
                await refreshCoverage(false);
            }
        }
        if (command === 'startCapture' &&
            vscode.workspace.getConfiguration('blueprintInspector').get<boolean>('resolveAllOnCapture', true)) {
            void resolveAllHotPaths(true);
        }
    } catch (error) {
        vscode.window.showErrorMessage(`Blueprint Inspector: ${error instanceof Error ? error.message : String(error)}`);
    }
}

function createInlayProvider(): vscode.InlayHintsProvider {
    return {
        onDidChangeInlayHints: heatChanged.event,
        provideInlayHints(document) {
            const name = classNameOf(document);
            const hits = name ? hotLines.get(name) : undefined;
            if (!hits || hits.size === 0) {
                return [];
            }
            const hints: vscode.InlayHint[] = [];
            for (const [line, count] of hits) {
                if (line < 1 || line > document.lineCount) {
                    continue;
                }
                const hint = new vscode.InlayHint(
                    new vscode.Position(line - 1, document.lineAt(line - 1).text.length),
                    ` ×${count}`,
                    vscode.InlayHintKind.Type);
                hint.tooltip = `${count} Blueprint dispatches`;
                hints.push(hint);
            }
            return hints;
        },
    };
}

class BlueprintDocumentSymbolProvider implements vscode.DocumentSymbolProvider {
    provideDocumentSymbols(document: vscode.TextDocument): vscode.DocumentSymbol[] {
        const model = parseRenderedSource(document.getText());
        return model.functions.map(fn => {
            const range = new vscode.Range(fn.line, 0, fn.endLine, Number.MAX_SAFE_INTEGER);
            const selection = new vscode.Range(fn.line, 0, fn.line, Number.MAX_SAFE_INTEGER);
            const symbol = new vscode.DocumentSymbol(
                fn.name, fn.parameters.join(', '), vscode.SymbolKind.Function, range, selection);
            symbol.children = fn.variables.map(variable => new vscode.DocumentSymbol(
                variable.name,
                variable.isParameter ? 'parameter' : '',
                vscode.SymbolKind.Variable,
                new vscode.Range(variable.line, 0, variable.line, Number.MAX_SAFE_INTEGER),
                new vscode.Range(variable.line, 0, variable.line, Number.MAX_SAFE_INTEGER)));
            return symbol;
        });
    }
}

class BlueprintDefinitionProvider implements vscode.DefinitionProvider {
    provideDefinition(document: vscode.TextDocument, position: vscode.Position): vscode.Location | undefined {
        const model = parseRenderedSource(document.getText());
        const variable = variableAt(model, document.lineAt(position.line).text, position.character);
        if (!variable) {
            return undefined;
        }
        return new vscode.Location(document.uri, new vscode.Position(variable.line, 0));
    }
}

async function goToVariable(): Promise<void> {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.uri.scheme !== SCHEME) {
        vscode.window.showInformationMessage('Blueprint Inspector: open a blueprint-source document first.');
        return;
    }

    const model = parseRenderedSource(editor.document.getText());
    if (model.variables.length === 0) {
        vscode.window.showInformationMessage('Blueprint Inspector: this document declares no variables.');
        return;
    }

    const items = model.variables.map(variable => ({
        label: variable.name,
        description: variable.isParameter ? 'parameter' : 'local',
        line: variable.line,
    }));
    const picked = await vscode.window.showQuickPick(items, {
        placeHolder: 'Variable to jump to',
        matchOnDescription: true,
    });
    if (!picked) {
        return;
    }

    const target = new vscode.Position(picked.line, 0);
    editor.selection = new vscode.Selection(target, target);
    editor.revealRange(new vscode.Range(target, target), vscode.TextEditorRevealType.InCenter);
}

export function activate(context: vscode.ExtensionContext) {
    log('activated (token auth)');
    context.subscriptions.push(
        vscode.workspace.registerTextDocumentContentProvider(SCHEME, new BlueprintSourceProvider())
    );

    ensureHeatDecorations();
    statusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    statusBar.hide();
    context.subscriptions.push(
        statusBar,
        vscode.commands.registerCommand('blueprintInspector.startCapture', () => runCaptureCommand('startCapture')),
        vscode.commands.registerCommand('blueprintInspector.stopCapture', () => runCaptureCommand('stopCapture')),
        vscode.commands.registerCommand('blueprintInspector.resetCapture', () => runCaptureCommand('resetCapture')),
        vscode.commands.registerCommand('blueprintInspector.refreshHotLines', () => {
            void refreshHotLines().catch(error => vscode.window.showErrorMessage(`Blueprint Inspector: ${String(error)}`));
        }),
        vscode.commands.registerCommand('blueprintInspector.resolveAllHotPaths', () => {
            void resolveAllHotPaths();
        }),
        vscode.window.registerTreeDataProvider('blueprintInspector.hotPaths', hotPathsProvider),
        vscode.commands.registerCommand('blueprintInspector.openHotLine', async (node: HotPathNode) => {
            if (node?.kind !== 'line' || node.line === undefined) {
                return;
            }
            try {
                await openBlueprintAt(node.className, node.line);
            } catch (error) {
                vscode.window.showErrorMessage(
                    `Blueprint Inspector: ${error instanceof Error ? error.message : String(error)}`);
            }
        }),
        vscode.window.registerTreeDataProvider('blueprintInspector.audit', auditProvider),
        vscode.window.registerTreeDataProvider('blueprintInspector.coverage', coverageProvider),
        vscode.commands.registerCommand('blueprintInspector.refreshCoverage', () => {
            void refreshCoverage().catch(error =>
                vscode.window.showErrorMessage(`Blueprint Inspector: ${String(error)}`));
        }),
        vscode.commands.registerCommand('blueprintInspector.exportCoverageJson', () => {
            void exportCoverageJson().catch(error =>
                vscode.window.showErrorMessage(`Blueprint Inspector: ${String(error)}`));
        }),
        vscode.commands.registerCommand('blueprintInspector.exportCoverageLcov', () => {
            void exportCoverageLcov().catch(error =>
                vscode.window.showErrorMessage(`Blueprint Inspector: ${String(error)}`));
        }),
        vscode.commands.registerCommand('blueprintInspector.toggleCoverageUncoveredOnly', () => {
            void toggleCoverageUncoveredOnly();
        }),
        vscode.commands.registerCommand('blueprintInspector.openCoverageLine', (node: CoverageNode) => {
            void openCoverageLine(node).catch(error =>
                vscode.window.showErrorMessage(
                    `Blueprint Inspector: ${error instanceof Error ? error.message : String(error)}`));
        }),
        vscode.window.registerTreeDataProvider('blueprintInspector.snapshotDiff', snapshotDiffProvider),
        vscode.commands.registerCommand('blueprintInspector.saveSnapshot', () => {
            void saveSnapshot().catch(error =>
                vscode.window.showErrorMessage(`Blueprint Inspector: ${String(error)}`));
        }),
        vscode.commands.registerCommand('blueprintInspector.compareSnapshot', () => {
            void compareSnapshot().catch(error =>
                vscode.window.showErrorMessage(`Blueprint Inspector: ${String(error)}`));
        }),
        vscode.commands.registerCommand('blueprintInspector.clearSnapshotDiff', () => clearSnapshotDiff()),
        vscode.commands.registerCommand('blueprintInspector.openSnapshotDiffLine', (node: SnapshotNode) => {
            void openSnapshotDiffLine(node).catch(error =>
                vscode.window.showErrorMessage(
                    `Blueprint Inspector: ${error instanceof Error ? error.message : String(error)}`));
        }),
        vscode.commands.registerCommand('blueprintInspector.runAudit', () => {
            void runAudit();
        }),
        vscode.commands.registerCommand('blueprintInspector.exportAudit', () => {
            void exportAudit().catch(error =>
                vscode.window.showErrorMessage(`Blueprint Inspector: ${String(error)}`));
        }),
        vscode.commands.registerCommand('blueprintInspector.openAuditFinding', (finding: AuditFinding) => {
            void openAuditFinding(finding).catch(error =>
                vscode.window.showErrorMessage(
                    `Blueprint Inspector: ${error instanceof Error ? error.message : String(error)}`));
        }),
        vscode.window.onDidChangeVisibleTextEditors(() => {
            applyHeat();
            scheduleVisibleRefresh();
        }),
        vscode.languages.registerInlayHintsProvider({ scheme: SCHEME }, createInlayProvider()),
        vscode.languages.registerDocumentSymbolProvider({ scheme: SCHEME }, new BlueprintDocumentSymbolProvider()),
        vscode.languages.registerDefinitionProvider({ scheme: SCHEME }, new BlueprintDefinitionProvider()),
        vscode.commands.registerCommand('blueprintInspector.goToVariable', () => { void goToVariable(); })
    );

    // The adapter parks the game thread when execution stops. Releasing the game's pointer capture is
    // not enough on its own: this window has to come to the front as well, or the user is left
    // alt-tabbing to the debugger. `stopped` is a DAP event, so watch the adapter traffic rather than
    // waiting for the UI to react to the stopped frame.
    context.subscriptions.push(vscode.debug.registerDebugAdapterTrackerFactory(DEBUGGER_TYPE, {
        createDebugAdapterTracker(session) {
            if (session.type !== DEBUGGER_TYPE) {
                return undefined;
            }
            return {
                onDidSendMessage(message: any) {
                    if (message?.type === 'event' && message.event === 'stopped') {
                        // Internal command: focuses the OS window, not just a widget inside it.
                        void vscode.commands.executeCommand('workbench.action.focusWindow').then(undefined, error => {
                            log(`could not focus the debugger window: ${String(error)}`);
                        });
                    }
                }
            };
        }
    }));

    // The debug type and this extension share a development-host window. Authenticate with the token
    // the adapter published, then open the source picker once the session can serve data.
    context.subscriptions.push(vscode.debug.onDidStartDebugSession(async session => {
        if (session.type !== DEBUGGER_TYPE) {
            return;
        }
        log(`debug session started: ${session.name}`);
        if (!await authenticateSession(session)) {
            return;
        }
        statusBar?.show();
        updateCaptureUi();
        try {
            captureState = await sessionRequest<CaptureState>(session, 'blueprintInspector.captureState');
            setCapturing(captureState.capturing);
        } catch (error) {
            log(`capture state unavailable: ${String(error)}`);
        }
        setTimeout(() => {
            void vscode.commands.executeCommand('blueprint-inspector.openBlueprint');
        }, 300);
    }));

    context.subscriptions.push(vscode.debug.onDidTerminateDebugSession(session => {
        if (session.type !== DEBUGGER_TYPE) {
            return;
        }
        if (hotLinesTimer) {
            clearInterval(hotLinesTimer);
            hotLinesTimer = undefined;
        }
        captureState = { capturing: false, generation: 0, totalHits: 0 };
        clearHeat();
        auditFindings = [];
        auditMeta = { classes: 0, functions: 0, truncated: false };
        auditRan = false;
        auditNodes.clear();
        auditProvider.refresh();
        clearCoverage();
        clearSnapshotDiff();
        updateCaptureUi();
        statusBar?.hide();
    }));

    context.subscriptions.push(vscode.commands.registerCommand("blueprintInspector.startGameAndAttach", startGameAndAttach));

    context.subscriptions.push(vscode.commands.registerCommand("blueprint-inspector.openBlueprint", async () => {
        const session = activeSession();
        if (!session) {
            vscode.window.showErrorMessage('Start a blueprint-inspector debug session first.');
            return;
        }

        // List on open rather than caching at session start: these are the handles
        // that stay valid as long as nothing lists again.
        const sources = await serialize(() => fetchLoadedSources(session));
        if (sources.length === 0) {
            vscode.window.showWarningMessage('The session reported no blueprints.');
            return;
        }

        const picked = await vscode.window.showQuickPick(
            sources.map(source => ({
                label: source.name,
                source
            })),
            {
                placeHolder: "Open Blueprint..."
            }
        );

        if (!picked) {
            return;
        }

        await openBlueprintAt(picked.source.name);
    }));
}
