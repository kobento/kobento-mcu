#!/usr/bin/env node
// build.mjs — compile the knob documents: tools/knob/*.json -> documents/knob/*.rc.
//
// Uses the json2rc converter of the sibling repo ../kobento-mcp (which loads
// ../kobento-tools/json2rc/build), because these documents use what the vendored
// tools/splash/json2rc.js predates: computed (hsv) colours, exported colours, library
// components (Icon). KOBENTO_MCP overrides where kobento-mcp is.
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { dirname, resolve, basename } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const mcp = process.env.KOBENTO_MCP ?? resolve(here, '../../../kobento-mcp');
const { jsonToRc } = await import(pathToFileURL(resolve(mcp, 'mcp/lib/json2rc.mjs')).href);
const outDir = resolve(here, '../../documents/knob');
const only = process.argv.slice(2);
for (const f of readdirSync(here).filter((f) => f.endsWith('.json')).sort()) {
    const name = basename(f, '.json');
    if (only.length && !only.includes(name)) continue;
    const out = jsonToRc(readFileSync(resolve(here, f), 'utf8'), here);
    const bytes = out.bytes ?? out;
    writeFileSync(resolve(outDir, name + '.rc'), Buffer.from(bytes));
    console.log(`documents/knob/${name}.rc: ${bytes.length} bytes`);
}
