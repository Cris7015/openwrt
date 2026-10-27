// Replay tests for recorder.uc. Run on the router:
// scripts/gpon-panel-test.sh <pkgdir> tests/recorder_test.uc
'use strict';

import { ok, eq, done } from 'assert';
import * as fs from 'fs';

const BASE = '/tmp/xr500v-gpon-test';
const T0 = 1800000000;	// aligned to 300 s and 1800 s buckets
const DEACT = '[  100.000000] airoha-xpon 1fb60000.xmac: restarting GPON after Deactivate_ONU-ID from the OLT';

function status(over) {
	let st = {
		state: 5, onu_id: 1, telemetry_valid: 255,
		bosa_rx_power_nw: 8400, bosa_tx_power_nw: 1480000,
		bosa_temperature_mc: 50776, bosa_bias_ua: 17142,
		bosa_voltage_uv: 3258500, bosa_alarms: 0,
		fec_downstream: 2, fec_upstream: 2,
		olt_profile_effective_name: 'huawei',
		olt: { vendor_id: 'HWTC', version: '10' },
		serial: 'TEST00000000'
	};
	for (let k in over) st[k] = over[k];
	return st;
}

function bipline(total) {
	return `active=1 session=1 total=${total} session_total=${total} last=0 pending_rei=0`;
}

function sample(t, over) {
	let s = { t, status: status({}), bip: bipline(0), gem_delin: '15\n',
		sfifo_resync: '0\n', sfifo_glitch: '0\n' };
	for (let k in over) s[k] = over[k];
	return s;
}

// Writes the fixture, runs the replay, returns { rc, out }.
function replay(name, fixture) {
	let path = `${BASE}/${name}.json`;
	fs.writefile(path, sprintf('%J', fixture));
	let p = fs.popen(`ucode -L ${BASE}/lib ${BASE}/lib/recorder.uc --replay ${path} 2>/dev/null`);
	let text = p.read('all');
	let rc = p.close();
	let out = null;
	try { out = json(text); } catch (e) { }
	return { rc, out };
}

function types(events) {
	return map(events || [], (e) => e.type);
}

// recorder_replay_basic: 6 h at O5, BIP only in the first 5 h.
let samples = [];
for (let i = 0; i < 360; i++)
	push(samples, sample(T0 + 60 * i, { bip: bipline(i < 300 ? int(i / 10) : 29) }));
let r = replay('basic', { samples });
eq('recorder_replay_basic_rc', r.rc, 0);
eq('recorder_replay_basic_m1', r.out?.m1_len, 360);
eq('recorder_replay_basic_m5', r.out?.m5_len, 72);
eq('recorder_replay_basic_health', r.out?.health, 'ok');
eq('recorder_replay_basic_bip_total', r.out?.counters?.bip_total, 29);

// recorder_replay_deactivate: a Deactivate in the kernel log at the end.
samples = [];
for (let i = 0; i < 10; i++)
	push(samples, sample(T0 + 60 * i));
r = replay('deact', { samples, klog: [ { t: T0 + 540, line: DEACT } ] });
eq('recorder_replay_deactivate_events', length(filter(types(r.out?.events), (t) => t == 'deactivate')), 1);
eq('recorder_replay_deactivate_counter', r.out?.counters?.deactivate, 1);
eq('recorder_replay_deactivate_health', r.out?.health, 'crit');

// recorder_replay_skips_invalid_clock: first sample before NTP sync.
samples = [ sample(1000, { status: status({ state: 1 }) }) ];
for (let i = 1; i < 5; i++)
	push(samples, sample(T0 + 60 * i));
r = replay('clock', { samples });
eq('recorder_replay_skips_invalid_clock_skipped', r.out?.skipped, 1);
eq('recorder_replay_skips_invalid_clock_m1', r.out?.m1_len, 4);
ok('recorder_replay_skips_invalid_clock_no_1970_event',
	length(filter(r.out?.events || [], (e) => e.t < 1704067200)) == 0);

// recorder_replay_survives_missing_status: omci.agent did not answer.
r = replay('nostatus', { samples: [ sample(T0), sample(T0 + 60, { status: null }) ] });
eq('recorder_replay_survives_missing_status_rc', r.rc, 0);
eq('recorder_replay_survives_missing_status_m1', r.out?.m1_len, 2);
eq('recorder_replay_survives_missing_status_state', r.out?.last?.state, null);
eq('recorder_replay_survives_missing_status_health', r.out?.health, 'crit');

// recorder_replay_missing_status_keeps_o5: one unanswered call at O5 is not
// a link drop: no o5 events, o5_since kept.
r = replay('gap', { samples: [ sample(T0), sample(T0 + 60, { status: null }),
	sample(T0 + 120) ] });
eq('recorder_replay_missing_status_keeps_o5_events', types(r.out?.events), []);
eq('recorder_replay_missing_status_keeps_o5_since', r.out?.o5_since, T0);

// recorder_replay_missing_status_then_drop: a drop behind a gap is still seen.
r = replay('gapdrop', { samples: [ sample(T0), sample(T0 + 60, { status: null }),
	sample(T0 + 120, { status: status({ state: 1 }) }), sample(T0 + 180) ] });
eq('recorder_replay_missing_status_then_drop_events', types(r.out?.events), [ 'o5_down', 'o5_up' ]);
eq('recorder_replay_missing_status_then_drop_since', r.out?.o5_since, T0 + 180);

// recorder_replay_without_debugfs: lab counters absent.
samples = [];
for (let i = 0; i < 3; i++) {
	let s = sample(T0 + 60 * i);
	delete s.sfifo_resync;
	delete s.sfifo_glitch;
	push(samples, s);
}
r = replay('nodebugfs', { samples });
eq('recorder_replay_without_debugfs_resync', r.out?.counters?.sfifo_resync, null);
eq('recorder_replay_without_debugfs_glitch', r.out?.counters?.sfifo_glitch, null);
ok('recorder_replay_without_debugfs_no_resync_event', !('resync' in types(r.out?.events)));

// load_snapshot_tolerates_corrupt_file: power cut during a snapshot write.
fs.writefile(`${BASE}/bad.json.gz`, '\x1f\x8b\x08\x00garbage');
r = replay('corrupt', { snapshot: `${BASE}/bad.json.gz`,
	samples: [ sample(T0), sample(T0 + 60), sample(T0 + 120) ] });
eq('load_snapshot_tolerates_corrupt_file_rc', r.rc, 0);
eq('load_snapshot_tolerates_corrupt_file_m1', r.out?.m1_len, 3);

// snapshot_roundtrip: a saved snapshot is restored before new samples.
fs.unlink(`${BASE}/good.json.gz`);
r = replay('save', { save: `${BASE}/good.json.gz`, samples: [ sample(T0), sample(T0 + 60) ] });
ok('snapshot_roundtrip_saved', fs.stat(`${BASE}/good.json.gz`) != null);
r = replay('load', { snapshot: `${BASE}/good.json.gz`, samples: [ sample(T0 + 120) ] });
eq('snapshot_roundtrip_m1', r.out?.m1_len, 3);

done();
