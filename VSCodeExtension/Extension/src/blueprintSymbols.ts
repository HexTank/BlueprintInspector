// Navigation model for rendered Blueprint source.
//
// The pseudo-source is a virtual document, so VS Code can only navigate it through what we tell it.
// This module turns the rendered text into functions and variables; the extension registers it as a
// document symbol provider (Outline, Ctrl+Shift+O) and a definition provider (F12 / Ctrl+click).

export interface RenderedVariable {
    name: string;
    /** Line where the variable is first defined: its parameter list or its first assignment. */
    line: number;
    isParameter: boolean;
}

export interface RenderedFunction {
    name: string;
    line: number;
    endLine: number;
    parameters: string[];
    variables: RenderedVariable[];
}

export interface RenderedSource {
    functions: RenderedFunction[];
    /** Variables in definition order, deduplicated by name. */
    variables: RenderedVariable[];
}

const FUNCTION_HEADER = /^([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*$/;
// A leading identifier-ish run, then a single `=` that is not part of `==`.
const ASSIGNMENT = /^\s*([A-Za-z_][^=();{}]*?)\s*=(?!=)\s*\S/;
const PARAMETER_NAME = /([A-Za-z_][A-Za-z0-9_]*)\s*$/;
// `if (...)`, `switch (...)` and friends have header shape but are not functions.
const KEYWORDS = new Set(['if', 'else', 'for', 'foreach', 'while', 'switch', 'case', 'return', 'do']);

// A header line that really declares a function: `Name(args)`, not `if (x)`.
function declaredFunction(line: string): string | undefined {
    const match = FUNCTION_HEADER.exec(line.trim());
    if (!match || KEYWORDS.has(match[1])) {
        return undefined;
    }
    return match[1];
}

function parameterNames(list: string): string[] {
    return list
        .split(',')
        .map(parameter => PARAMETER_NAME.exec(parameter.trim()))
        .filter((match): match is RegExpExecArray => match !== null)
        .map(match => match[1]);
}

function assignmentTarget(line: string): string | undefined {
    const match = ASSIGNMENT.exec(line);
    if (!match) {
        return undefined;
    }
    const name = match[1].trim();
    // Keywords and compound statements would otherwise land in the list.
    if (!name || /^(if|while|for|switch|return|else|case)\b/.test(name)) {
        return undefined;
    }
    return name;
}

export function parseRenderedSource(text: string): RenderedSource {
    const lines = text.split('\n');
    const functions: RenderedFunction[] = [];
    const variables: RenderedVariable[] = [];
    const defined = new Set<string>();

    let index = 0;
    while (index < lines.length) {
        const header = FUNCTION_HEADER.exec(lines[index].trim());
        if (!header || !declaredFunction(lines[index])) {
            index += 1;
            continue;
        }

        const name = header[1];
        const parameters = parameterNames(header[2]);
        const start = index;
        let end = start + 1;
        let closed = false;
        // The renderer closes a function with `}` at column 0; the next header is the fallback bound.
        while (end < lines.length) {
            if (lines[end] === '}') {
                closed = true;
                break;
            }
            if (end > start + 1 && declaredFunction(lines[end]) !== undefined) {
                break;
            }
            end += 1;
        }

        const own: RenderedVariable[] = [];
        const seen = new Set<string>();
        for (const parameter of parameters) {
            own.push({ name: parameter, line: start, isParameter: true });
            seen.add(parameter);
            if (!defined.has(parameter)) {
                defined.add(parameter);
                variables.push({ name: parameter, line: start, isParameter: true });
            }
        }
        for (let line = start + 1; line < end && line < lines.length; line += 1) {
            const target = assignmentTarget(lines[line]);
            if (!target || seen.has(target)) {
                continue;
            }
            seen.add(target);
            own.push({ name: target, line, isParameter: false });
            if (!defined.has(target)) {
                defined.add(target);
                variables.push({ name: target, line, isParameter: false });
            }
        }

        functions.push({ name, line: start, endLine: Math.min(end, lines.length - 1), parameters, variables: own });
        index = closed ? end + 1 : end;
    }

    return { functions, variables };
}

function isWordCharacter(character: string | undefined): boolean {
    return character !== undefined && /[a-z0-9]/.test(character);
}

/**
 * The variable whose name covers `position` on `line`, preferring the longest match.
 *
 * The renderer sanitises every Blueprint name to an identifier, so a name has exactly one spelling in
 * the signature and in the body. Matching is therefore case-insensitive only; there is no second
 * spelling to fold, and no offset map to build.
 */
export function variableAt(source: RenderedSource, lineText: string, character: number): RenderedVariable | undefined {
    const text = lineText.toLowerCase();
    let best: RenderedVariable | undefined;
    let bestLength = 0;

    for (const variable of source.variables) {
        const needle = variable.name.toLowerCase();
        if (needle.length === 0 || needle.length <= bestLength) {
            continue;
        }

        let from = text.indexOf(needle);
        while (from >= 0) {
            const endsCleanly = !isWordCharacter(text[from + needle.length]);
            const startsCleanly = !isWordCharacter(text[from - 1]);
            if (startsCleanly && endsCleanly && character >= from && character <= from + needle.length) {
                best = variable;
                bestLength = needle.length;
                break;
            }
            from = text.indexOf(needle, from + 1);
        }
    }

    return best;
}
