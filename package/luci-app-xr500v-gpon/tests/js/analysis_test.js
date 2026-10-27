// Tests for xr500v-gpon/analysis.js (run through run.js).
'use strict';

module.exports = ({ load, eq, ok }) => {
	const a = load('xr500v-gpon/analysis.js');
	const T = 1800000000;	// 2027-01-15 08:00 UTC

	function pt(t, rx, temp, bip) {
		return { t, n: 1, rx_min: rx, rx_avg: rx, rx_max: rx,
			temp_min: temp, temp_avg: temp, temp_max: temp, bip };
	}

	const pts = [ pt(T, 8400, 50776, 0), pt(T + 60, 8500, 50800, 2), pt(T + 120, null, null, null) ];

	// nearest
	eq('analysis_nearest_empty', a.nearest([], 5), -1);
	eq('analysis_nearest_edges', [ a.nearest(pts, T - 999), a.nearest(pts, T + 9999) ], [ 0, 2 ]);
	eq('analysis_nearest_middle', a.nearest(pts, T + 70), 1);

	// series / inWindow
	eq('analysis_series_skips_null', a.series(pts, 'rx').length, 2);
	eq('analysis_inwindow_half_open', a.inWindow(a.series(pts, 'rx'), T, T + 60).length, 1);

	// tooltip
	const ev = { t: T + 90, type: 'deactivate', severity: 'crit', msg: 'x' };
	const tip = a.tooltip(pts, [ ev ], T + 65, 60);
	eq('analysis_tooltip_bucket', [ tip.t, tip.events.length, tip.events[0].type ], [ T + 60, 1, 'deactivate' ]);
	ok('analysis_tooltip_values', Math.abs(tip.rx.avg - (-20.706)) < 0.01 && Math.abs(tip.temp.avg - 50.8) < 0.001 && tip.bip === 2);
	const many = [];
	for (let i = 0; i < 10; i++)
		many.push({ t: T + 61 + i, type: 'o5_up', severity: 'info', msg: 'x' });
	const capped = a.tooltip(pts, many, T + 60, 60);
	eq('analysis_tooltip_caps_events', [ capped.events.length, capped.more ], [ 3, 7 ]);
	eq('analysis_tooltip_no_data', a.tooltip(pts, [], T + 130, 60).rx, null);
	eq('analysis_tooltip_empty_points', a.tooltip([], [], T, 60), null);

	// Final review, Important 1: the point under the cursor is the one whose
	// interval holds it, and an event near the cursor shows even when it falls
	// in the next interval (a marker covers 8 px).
	const two = [ pt(T, 8400, 50776, 0), pt(T + 60, 8500, 50800, 0), pt(T + 120, 8600, 50800, 0) ];
	eq('analysis_tooltip_floor', a.tooltip(two, [], T + 50, 60, 0).t, T);
	eq('analysis_tooltip_event_second_half',
		a.tooltip(two, [ { t: T + 45, type: 'deactivate', severity: 'crit' } ], T + 45, 60, 0).events.length, 1);
	eq('analysis_tooltip_slack',
		a.tooltip(two, [ { t: T + 100, type: 'deactivate', severity: 'crit' } ], T + 130, 60, 40).events.length, 1);
	eq('analysis_tooltip_before_first', a.tooltip(two, [], T - 30, 60, 0).t, T);

	// eventText
	ok('analysis_eventtext_bip', a.eventText({ type: 'bip_burst', n: 12, msg: 'x' }).indexOf('12') >= 0);
	ok('analysis_eventtext_alarm_flags', a.eventText({ type: 'bosa_alarm', flags: 4, msg: 'x' }).indexOf('0x04') >= 0);
	eq('analysis_eventtext_unknown_falls_back', a.eventText({ type: 'new_kind', msg: 'as recorded' }), 'as recorded');

	// localTime
	const tz = process.env.TZ;
	if (tz === 'UTC')
		eq('analysis_localtime_tz', [ a.localTime(T, true), a.localTime(T, false) ], [ '2027-01-15 08:00', '08:00' ]);
	else if (tz === 'America/Argentina/Mendoza')
		eq('analysis_localtime_tz', [ a.localTime(T, true), a.localTime(T, false) ], [ '2027-01-15 05:00', '05:00' ]);
	else
		ok('analysis_localtime_tz', /^\d{4}-\d\d-\d\d \d\d:\d\d$/.test(a.localTime(T, true)));

	// csv
	const lines = a.csv(pts, [ ev ], 60).trim().split('\n');
	eq('analysis_csv_header_and_rows', lines[0],
		'time,rx_dbm_avg,rx_dbm_min,rx_dbm_max,temp_c_avg,temp_c_min,temp_c_max,bip,events');
	eq('analysis_csv_row_count', lines.length, 4);
	const r1 = lines[2].split(',');
	eq('analysis_csv_values', [ r1[1], r1[4], r1[7], r1[8] ], [ '-20.7', '50.8', '2', 'deactivate' ]);
	eq('analysis_csv_null_cells', lines[3].split(',').slice(1, 8), [ '', '', '', '', '', '', '' ]);
	ok('analysis_csv_quotes', a.csv([ pt(T, 8400, 50000, 0) ], [ { t: T, type: 'a,b', severity: 'info' } ], 60).indexOf('"a,b"') >= 0);

	// heatmap: local midnight splits days; hours are local hours
	const late = new Date(2027, 0, 14, 23, 30).getTime() / 1000;
	const early = new Date(2027, 0, 15, 0, 30).getTime() / 1000;
	const now = new Date(2027, 0, 15, 12, 0).getTime() / 1000;
	const hm = a.heatmap([ { t: late, n: 30, bip: 5 }, { t: early, n: 30, bip: 7 } ],
		[ { t: early + 60, type: 'deactivate', severity: 'crit' } ], now, 2);
	eq('analysis_heatmap_local_midnight', [ hm.length, hm[0].day, hm[0].hours[23].bip, hm[1].day, hm[1].hours[0].bip, hm[1].hours[0].crit ],
		[ 2, '2027-01-14', 5, '2027-01-15', 7, 1 ]);
	eq('analysis_heatmap_seen', [ hm[0].hours[23].seen, hm[0].hours[22].seen ], [ true, false ]);

	// empty inputs
	let threw = null;
	try {
		eq('analysis_empty_series', a.series([], 'rx'), []);
		eq('analysis_empty_csv', a.csv([], [], 60).trim().split('\n').length, 1);
		const e = a.heatmap([], [], now, 30);
		eq('analysis_empty_heatmap', [ e.length, e[29].hours.length, e[0].hours[5].bip ], [ 30, 24, 0 ]);
	} catch (e) {
		threw = e.message;
	}
	eq('analysis_empty_inputs', threw, null);
};
