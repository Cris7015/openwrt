// Lab eye scan of the PON PHY through the xr500v_eye module (debugfs
// xr500v-eye). The eye sampler does not disturb the data path, but moving
// its phase past the eye edge slips its comparison by a bit. So each half
// is swept outwards from the centre, after a realign (phase out to -48 with
// the monitor off, back with it on) that leaves the centre clean.
'use strict';

import * as fs from 'fs';

const EYE = '/sys/kernel/debug/xr500v-eye';
const STEP = 4, DUR = 4096, TRIES = 8;

function put(file, text) {
	return fs.writefile(`${EYE}/${file}`, text) != null;
}

function points() {
	let out = [];
	for (let line in split(trim(fs.readfile(`${EYE}/result`) || ''), '\n')) {
		let p = map(split(line, ' '), (x) => +x);
		if (length(p) == 3)
			push(out, p);
	}
	return out;
}

function loaded() {
	if (fs.access(`${EYE}/scanx`))
		return true;
	let ko = `/lib/modules/${trim(fs.readfile('/proc/sys/kernel/osrelease'))}/xr500v_eye.ko`;
	if (!fs.access(ko))
		return false;
	system([ 'insmod', ko ]);
	return !!fs.access(`${EYE}/scanx`);
}

export function eye_scan() {
	if (!loaded())
		return { ok: false, error: 'no_module' };
	let grid = {}, tries = [];
	for (let half in [ 1, 2 ]) {
		let n, centre;
		for (n = 1; n <= TRIES; n++) {
			if (!put('realign', '-48'))
				return { ok: false, error: 'realign_failed', tries };
			centre = points()[0]?.[2];
			if (centre === 0)
				break;
		}
		push(tries, n);
		if (centre !== 0)
			return { ok: false, error: 'not_aligned', tries };
		if (!put('scanx', `${STEP} ${DUR} ${half}`))
			return { ok: false, error: 'scan_failed', tries };
		// half 1 sweeps phase codes 64..127 (0..+63), half 2 codes 0..63
		for (let p in points())
			if ((half == 1) == (p[0] >= 64))
				grid[p[0] * 128 + p[1]] = p;
	}
	return {
		ok: true, t: time(), step: STEP, max: 4 * DUR, tries,
		points: sort(values(grid), (a, b) => a[0] - b[0] || a[1] - b[1])
	};
}
