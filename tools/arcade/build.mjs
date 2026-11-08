#!/usr/bin/env node
// build.mjs — compile tools/arcade/*.json -> documents/*.rc with ../kobento-mcp's json2rc
// (library components such as Button). KOBENTO_MCP overrides where kobento-mcp is.
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { dirname, resolve, basename } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const mcp = process.env.KOBENTO_MCP ?? resolve(here, '../../../kobento-mcp');
const { jsonToRc } = await import(pathToFileURL(resolve(mcp, 'mcp/lib/json2rc.mjs')).href);
for (const f of readdirSync(here).filter((f) => f.endsWith('.json')).sort()) {
    const name = basename(f, '.json');
    const out = jsonToRc(readFileSync(resolve(here, f), 'utf8'), here);
    const bytes = out.bytes ?? out;
    writeFileSync(resolve(here, '../../documents', name + '.rc'), Buffer.from(bytes));
    console.log(`documents/${name}.rc: ${bytes.length} bytes`);
}
