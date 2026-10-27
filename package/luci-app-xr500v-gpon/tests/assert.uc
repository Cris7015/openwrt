// Minimal assertions for the router-side ucode tests (not installed).
'use strict';

let failed = 0, passed = 0;

function report(name, good, detail) {
	if (good) {
		passed++;
	} else {
		failed++;
		print(`FAIL ${name}${detail ? ': ' + detail : ''}\n`);
	}
}

export function ok(name, cond) {
	report(name, !!cond, null);
};

export function eq(name, got, want) {
	// Arrays and objects compare by content, not by reference.
	let same = (type(want) in [ 'array', 'object' ])
		? sprintf('%J', got) === sprintf('%J', want) : got === want;
	report(name, same, `got ${sprintf('%J', got)}, want ${sprintf('%J', want)}`);
};

export function near(name, got, want, tol) {
	let d = got - want;	// no math module on the router: no abs()
	report(name, type(got) in ['int', 'double'] && (d < 0 ? -d : d) <= tol,
		`got ${sprintf('%J', got)}, want ${want} ± ${tol}`);
};

export function done() {
	print(`${passed} passed, ${failed} failed\n`);
	exit(failed ? 1 : 0);
};
