// Export formatting for the Coverage view, kept free of the `vscode` module so the text can be
// exercised without an extension host.

export interface CoverageLine {
    className: string;
    functionName: string;
    line: number;
    hits: number;
    // "statement" or "transfer" (a break/continue arm).
    kind: string;
}

export interface CoverageCapture {
    capturing: boolean;
    generation: number;
    totalHits: number;
}

export interface CoverageMeta {
    renderedClasses: number;
    totalLines: number;
    coveredLines: number;
    uncoveredLines: number;
    truncated: boolean;
}

export function coverageJson(
    capture: CoverageCapture, meta: CoverageMeta, lines: CoverageLine[]): string {
    return JSON.stringify({
        tool: 'blueprint-inspector',
        schema: 1,
        generated: new Date().toISOString(),
        capture,
        renderedClasses: meta.renderedClasses,
        truncated: meta.truncated,
        totals: {
            lines: meta.totalLines,
            covered: meta.coveredLines,
            uncovered: meta.uncoveredLines,
            percent: meta.totalLines > 0
                ? Number(((meta.coveredLines / meta.totalLines) * 100).toFixed(1))
                : 0,
        },
        lines: lines.map(line => ({
            className: line.className,
            functionName: line.functionName,
            line: line.line,
            hits: line.hits,
            kind: line.kind === 'transfer' ? 'transfer' : 'statement',
        })),
    }, null, 2);
}

function groupBy<T>(items: T[], key: (item: T) => string): Map<string, T[]> {
    const groups = new Map<string, T[]>();
    for (const item of items) {
        const name = key(item);
        const group = groups.get(name);
        if (group) {
            group.push(item);
        } else {
            groups.set(name, [item]);
        }
    }
    return groups;
}

// LCOV, as genhtml/lcov-merge expect it: one record per rendered source, DA per statement, and BRDA
// for a break/continue arm (which has no line of its own in the graph, only a jump/landing pair).
export function coverageLcov(lines: CoverageLine[]): string {
    const byClass = groupBy(lines, line => line.className);
    const out: string[] = [];

    for (const className of [...byClass.keys()].sort((a, b) => a.localeCompare(b))) {
        const classLines = byClass.get(className)!.slice().sort((a, b) => a.line - b.line);
        out.push('TN:blueprint-inspector');
        out.push(`SF:Blueprints/${className}.cpp`);

        const byFunction = groupBy(classLines, line => line.functionName);
        for (const [functionName, entries] of byFunction) {
            const start = Math.min(...entries.map(entry => entry.line));
            const hits = entries.reduce((total, entry) => total + entry.hits, 0);
            out.push(`FN:${start},${functionName}`);
            out.push(`FNDA:${hits},${functionName}`);
        }

        const branches = classLines.filter(line => line.kind === 'transfer');
        for (const line of classLines) {
            out.push(`DA:${line.line},${line.hits}`);
        }
        // One arm per line is all the plugin renders, so the branch index within the block is 0.
        for (const line of branches) {
            out.push(`BRDA:${line.line},0,0,${line.hits}`);
        }

        out.push(`LF:${classLines.length}`);
        out.push(`LH:${classLines.filter(line => line.hits > 0).length}`);
        if (branches.length > 0) {
            out.push(`BRF:${branches.length}`);
            out.push(`BRH:${branches.filter(line => line.hits > 0).length}`);
        }
        out.push('end_of_record');
    }

    return `${out.join('\n')}\n`;
}
