// Node harness for the panel's pure LuCI modules (not installed).
// node tests/js/run.js            runs every *_test.js next to this file
// LuCI modules are evaluated with stubs: baseclass.extend returns its
// argument, _() returns the string untranslated.
'use strict';

const fs = require('fs');
const path = require('path');

const RES = path.join(__dirname, '../../htdocs/luci-static/resources');

const stubs = {
	baseclass: { extend: (o) => o },
	L: { isObject: (v) => v != null && typeof v == 'object' && !Array.isArray(v) },
	_: (s) => s
};

function load(rel) {
	const src = fs.readFileSync(path.join(RES, rel), 'utf8');
	return new Function('baseclass', 'L', '_', src)(stubs.baseclass, stubs.L, stubs._);
}

let passed = 0, failed = 0;

function report(name, good, detail) {
	if (good)
		passed++;
	else {
		failed++;
		console.log(`FAIL ${name}${detail ? ': ' + detail : ''}`);
	}
}

function eq(name, got, want) {
	const a = JSON.stringify(got), b = JSON.stringify(want);
	report(name, a === b, `got ${a}, want ${b}`);
}

function ok(name, cond) {
	report(name, !!cond);
}

for (const f of fs.readdirSync(__dirname).filter((f) => f.endsWith('_test.js')).sort()) {
	try {
		require(path.join(__dirname, f))({ load, eq, ok });
	} catch (e) {
		report(f, false, e.stack.split('\n').slice(0, 2).join(' '));
	}
}

console.log(`${passed} passed, ${failed} failed (TZ=${process.env.TZ || 'system'})`);
process.exit(failed ? 1 : 0);
