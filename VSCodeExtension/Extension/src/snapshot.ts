// Capture snapshots and their diff, kept free of the `vscode` module so the file format and the diff
// ordering can be exercised without an extension host.

export interface SnapshotLine {
    className: string;
    functionName: string;
    line: number;
    hits: number;
    // "statement" or "transfer" (a break/continue arm).
    kind: string;
}

export interface SnapshotMetadata {
    map: string;
    build: string;
    engine: string;
    project: string;
    capturedAt: string;
}

export interface SnapshotCapture {
    capturing: boolean;
    generation: number;
    totalHits: number;
}

export const SNAPSHOT_SCHEMA = 1;

export interface Snapshot {
    tool: 'blueprint-inspector';
    schema: number;
    // When the file was written, and by which extension build.
    created: string;
    extensionVersion: string;
    capture: SnapshotCapture;
    metadata: SnapshotMetadata;
    totals: { lines: number; hits: number };
    lines: SnapshotLine[];
}

export interface SnapshotInput {
    capture: SnapshotCapture;
    metadata: SnapshotMetadata;
    lines: SnapshotLine[];
    extensionVersion: string;
    created?: string;
}

export function createSnapshot(input: SnapshotInput): Snapshot {
    const lines = input.lines.map(line => ({
        className: line.className,
        functionName: line.functionName,
        line: line.line,
        hits: line.hits,
        kind: line.kind === 'transfer' ? 'transfer' : 'statement',
    }));
    return {
        tool: 'blueprint-inspector',
        schema: SNAPSHOT_SCHEMA,
        created: input.created ?? new Date().toISOString(),
        extensionVersion: input.extensionVersion,
        capture: {
            capturing: input.capture.capturing === true,
            generation: Number(input.capture.generation) || 0,
            totalHits: Number(input.capture.totalHits) || 0,
        },
        metadata: {
            map: input.metadata.map ?? '',
            build: input.metadata.build ?? '',
            engine: input.metadata.engine ?? '',
            project: input.metadata.project ?? '',
            capturedAt: input.metadata.capturedAt ?? '',
        },
        totals: {
            lines: lines.length,
            hits: lines.reduce((total, line) => total + line.hits, 0),
        },
        lines,
    };
}

export interface ParsedSnapshot {
    snapshot?: Snapshot;
    error?: string;
}

// Reads a file written by createSnapshot. Strict on the schema, tolerant of missing optional strings:
// a snapshot from an older writer should load rather than fail.
export function parseSnapshot(text: string): ParsedSnapshot {
    let value: any;
    try {
        value = JSON.parse(text);
    } catch (error) {
        return { error: `not valid JSON: ${error instanceof Error ? error.message : String(error)}` };
    }
    if (!value || typeof value !== 'object') {
        return { error: 'not a snapshot object' };
    }
    if (value.tool !== 'blueprint-inspector') {
        return { error: `not a Blueprint Inspector snapshot (tool: ${String(value.tool)})` };
    }
    if (value.schema !== SNAPSHOT_SCHEMA) {
        return { error: `unsupported snapshot schema ${String(value.schema)} (expected ${SNAPSHOT_SCHEMA})` };
    }
    if (!Array.isArray(value.lines)) {
        return { error: 'snapshot has no lines array' };
    }

    const lines: SnapshotLine[] = [];
    for (const entry of value.lines) {
        if (!entry || typeof entry.className !== 'string' || typeof entry.functionName !== 'string' ||
            typeof entry.line !== 'number' || typeof entry.hits !== 'number') {
            return { error: 'snapshot has a malformed line entry' };
        }
        lines.push({
            className: entry.className,
            functionName: entry.functionName,
            line: entry.line,
            hits: entry.hits,
            kind: entry.kind === 'transfer' ? 'transfer' : 'statement',
        });
    }

    const metadata = value.metadata ?? {};
    return {
        snapshot: {
            tool: 'blueprint-inspector',
            schema: SNAPSHOT_SCHEMA,
            created: typeof value.created === 'string' ? value.created : '',
            extensionVersion: typeof value.extensionVersion === 'string' ? value.extensionVersion : '',
            capture: {
                capturing: value.capture?.capturing === true,
                generation: Number(value.capture?.generation) || 0,
                totalHits: Number(value.capture?.totalHits) || 0,
            },
            metadata: {
                map: typeof metadata.map === 'string' ? metadata.map : '',
                build: typeof metadata.build === 'string' ? metadata.build : '',
                engine: typeof metadata.engine === 'string' ? metadata.engine : '',
                project: typeof metadata.project === 'string' ? metadata.project : '',
                capturedAt: typeof metadata.capturedAt === 'string' ? metadata.capturedAt : '',
            },
            totals: {
                lines: lines.length,
                hits: lines.reduce((total, line) => total + line.hits, 0),
            },
            lines,
        },
    };
}

export type DiffChange = 'regressed' | 'added' | 'improved' | 'removed';

export interface DiffEntry {
    className: string;
    functionName: string;
    line: number;
    kind: string;
    change: DiffChange;
    // Absent when the line only exists in one of the two runs.
    before?: number;
    after?: number;
    delta: number;
}

export interface SnapshotDiff {
    before: Snapshot;
    after: Snapshot;
    entries: DiffEntry[];
    regressed: number;
    improved: number;
    added: number;
    removed: number;
    unchanged: number;
    beforeHits: number;
    afterHits: number;
}

function lineKey(line: SnapshotLine): string {
    return `${line.className}\u0000${line.functionName}\u0000${line.line}\u0000${line.kind}`;
}

const CHANGE_ORDER: Record<DiffChange, { rank: number; magnitude: (entry: DiffEntry) => number }> = {
    regressed: { rank: 0, magnitude: entry => Math.abs(entry.delta) },
    added: { rank: 1, magnitude: entry => entry.after ?? 0 },
    improved: { rank: 2, magnitude: entry => Math.abs(entry.delta) },
    removed: { rank: 3, magnitude: entry => entry.before ?? 0 },
};

export function diffSnapshots(before: Snapshot, after: Snapshot): SnapshotDiff {
    const prior = new Map<string, SnapshotLine>();
    for (const line of before.lines) {
        prior.set(lineKey(line), line);
    }

    const entries: DiffEntry[] = [];
    let unchanged = 0;
    let regressed = 0;
    let improved = 0;
    let added = 0;
    let removed = 0;

    for (const line of after.lines) {
        const key = lineKey(line);
        const previous = prior.get(key);
        if (!previous) {
            entries.push({
                className: line.className,
                functionName: line.functionName,
                line: line.line,
                kind: line.kind,
                change: 'added',
                after: line.hits,
                delta: line.hits,
            });
            added++;
            continue;
        }
        prior.delete(key);
        const delta = line.hits - previous.hits;
        if (delta === 0) {
            unchanged++;
            continue;
        }
        entries.push({
            className: line.className,
            functionName: line.functionName,
            line: line.line,
            kind: line.kind,
            change: delta > 0 ? 'regressed' : 'improved',
            before: previous.hits,
            after: line.hits,
            delta,
        });
        if (delta > 0) {
            regressed++;
        } else {
            improved++;
        }
    }

    for (const line of prior.values()) {
        entries.push({
            className: line.className,
            functionName: line.functionName,
            line: line.line,
            kind: line.kind,
            change: 'removed',
            before: line.hits,
            delta: -line.hits,
        });
        removed++;
    }

    entries.sort((a, b) => {
        const orderA = CHANGE_ORDER[a.change];
        const orderB = CHANGE_ORDER[b.change];
        if (orderA.rank !== orderB.rank) {
            return orderA.rank - orderB.rank;
        }
        const aMagnitude = orderA.magnitude(a);
        const bMagnitude = orderB.magnitude(b);
        if (aMagnitude !== bMagnitude) {
            return bMagnitude - aMagnitude;
        }
        if (a.className !== b.className) {
            return a.className.localeCompare(b.className);
        }
        if (a.functionName !== b.functionName) {
            return a.functionName.localeCompare(b.functionName);
        }
        if (a.line !== b.line) {
            return a.line - b.line;
        }
        return a.kind.localeCompare(b.kind);
    });

    return {
        before,
        after,
        entries,
        regressed,
        improved,
        added,
        removed,
        unchanged,
        beforeHits: before.totals?.hits ?? before.lines.reduce((total, line) => total + line.hits, 0),
        afterHits: after.totals?.hits ?? after.lines.reduce((total, line) => total + line.hits, 0),
    };
}
