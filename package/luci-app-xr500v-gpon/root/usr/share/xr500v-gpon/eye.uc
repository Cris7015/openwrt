// Lab eye scan of the PON PHY through the xr500v_eye module (debugfs
// xr500v-eye). The eye sampler does not disturb the data path, but moving
// its phase past the eye edge slips its comparison by a bit. So each half
// is swept outwards from the centre, after a realign (phase out to -48 with
// the monitor off, back with it on) that leaves the centre clean. A slip can
// still happen mid-sweep, but it only adds disagreements (the neighbouring
// bit matches half the time), never a clean cell: each half is swept a few
// times and every cell keeps its lowest count.
'use strict';

import * as fs from 'fs';

const EYE = '/sys/kernel/debug/xr500v-eye';
const STEP = 4, DUR = 4096, TRIES = 16, PASSES = 3;

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

// The margin kept in the history: clean width along threshold 0 and clean
// height along phase 0, in codes, and the clean cells of the whole map.
export function eye_metrics(r, t) {
	if (!r?.ok)
		return { t, err: r?.error || 'unknown' };
	let w = 0, h = 0, a = 0;
	for (let p in r.points) {
		if (p[2] != 0)
			continue;
		a++;
		if (p[1] == 64)
			w += r.step;
		if (p[0] == 64)
			h += r.step;
	}
	return { t: r.t, w, h, a, n: length(r.points), tries: r.tries };
}

export function eye_scan() {
	if (!loaded())
		return { ok: false, error: 'no_module' };
	let grid = {}, tries = [];
	for (let half in [ 1, 2 ]) {
		let swept = 0;
		for (let pass = 0; pass < PASSES; pass++) {
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
				continue;
			if (!put('scanx', `${STEP} ${DUR} ${half}`))
				return { ok: false, error: 'scan_failed', tries };
			swept++;
			// half 1 sweeps phase codes 64..127 (0..+63), half 2 codes 0..63
			for (let p in points()) {
				let k = p[0] * 128 + p[1];
				if ((half == 1) == (p[0] >= 64) && (grid[k] == null || p[2] < grid[k][2]))
					grid[k] = p;
			}
		}
		if (!swept)
			return { ok: false, error: 'not_aligned', tries };
	}
	return {
		ok: true, t: time(), step: STEP, max: 4 * DUR, tries,
		points: sort(values(grid), (a, b) => a[0] - b[0] || a[1] - b[1])
	};
}
