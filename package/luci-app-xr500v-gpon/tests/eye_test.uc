// eye_metrics() tests. Run on the router:
// scripts/gpon-panel-test.sh <pkgdir> tests/eye_test.uc
'use strict';

import { eq, done } from 'assert';
import { eye_metrics } from 'eye';

// A 3x3 corner of a step-4 map: phase codes 60/64/68, thresholds 60/64/68.
let pts = [];
for (let h in [ 60, 64, 68 ])
	for (let v in [ 60, 64, 68 ])
		push(pts, [ h, v, (h == 68 && v == 68) ? 900 : 0 ]);
let m = eye_metrics({ ok: true, t: 1800000000, step: 4, tries: [ 1, 2 ], points: pts }, 1);

eq('eye_metrics_t', m.t, 1800000000);
eq('eye_metrics_width', m.w, 12);	// row v=64: three clean cells x 4
eq('eye_metrics_height', m.h, 12);	// column h=64: three clean cells x 4
eq('eye_metrics_area', m.a, 8);
eq('eye_metrics_cells', m.n, 9);
eq('eye_metrics_tries', m.tries?.[1], 2);

let f = eye_metrics({ ok: false, error: 'not_aligned' }, 1700000000);
eq('eye_metrics_fail_t', f.t, 1700000000);
eq('eye_metrics_fail_err', f.err, 'not_aligned');
eq('eye_metrics_fail_width', f.w, null);
eq('eye_metrics_null', eye_metrics(null, 5).err, 'unknown');

done();
