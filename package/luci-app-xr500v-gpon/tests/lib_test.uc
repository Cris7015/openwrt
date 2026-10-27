// Tests for lib.uc. Run on the router: scripts/gpon-panel-test.sh <pkgdir> tests/lib_test.uc
'use strict';

import { ok, eq, near, done } from 'assert';
import {
	nw_to_dbm, dbm_to_nw, parse_bip, normalize_status, thresholds,
	evaluate, detect, aggregate, ring_push
} from 'lib';

const th = thresholds(null);
const now = 1800000000;

function has(alerts, id) {
	return length(filter(alerts, (a) => a.id == id)) > 0;
}

function types(events) {
	return map(events, (e) => e.type);
}

// dBm conversions (GPON class B+ range).
near('nw_to_dbm_rx', nw_to_dbm(8400), -20.76, 0.01);
near('nw_to_dbm_tx', nw_to_dbm(1480000), 1.70, 0.01);
eq('nw_to_dbm_zero', nw_to_dbm(0), null);
eq('nw_to_dbm_null', nw_to_dbm(null), null);
near('dbm_to_nw_crit', dbm_to_nw(-27), 1995, 2);
near('dbm_to_nw_warn', dbm_to_nw(-25), 3162, 2);

// BIP counter line from sysfs.
let bip = parse_bip('active=1 session=1 total=0 session_total=0 last=0 pending_rei=0');
eq('parse_bip_session', bip?.session, 1);
eq('parse_bip_total', bip?.total, 0);
eq('parse_bip_garbage', parse_bip('x'), null);

// Thresholds defaults.
eq('thresholds_rx_warn', th.rx_warn_dbm, -25);
eq('thresholds_rx_crit', th.rx_crit_dbm, -27);
eq('thresholds_rx_high', th.rx_high_dbm, -9);
eq('thresholds_temp', th.temp_warn_c, 70);
eq('thresholds_bip_burst', th.bip_burst_per_min, 10);
eq('thresholds_uci_override', thresholds({ rx_warn_dbm: '-24' }).rx_warn_dbm, -24);

// evaluate()
function sample(over) {
	let s = { state: 5, rx_nw: 8400, temp_mc: 50776, alarms: 0 };
	for (let k in over) s[k] = over[k];
	return s;
}

let r = evaluate(sample({}), {}, th);
eq('evaluate_ok_health', r.health, 'ok');
eq('evaluate_ok_alerts', length(r.alerts), 0);

r = evaluate(sample({ rx_nw: 3000 }), {}, th);
eq('evaluate_rx_warn_health', r.health, 'warn');
ok('evaluate_rx_warn_id', has(r.alerts, 'rx_warn'));

r = evaluate(sample({ rx_nw: 1500 }), {}, th);
eq('evaluate_rx_crit_health', r.health, 'crit');
ok('evaluate_rx_crit_id', has(r.alerts, 'rx_crit'));

r = evaluate(sample({ rx_nw: 130000 }), {}, th);
eq('evaluate_rx_high_health', r.health, 'warn');
ok('evaluate_rx_high_id', has(r.alerts, 'rx_high'));

r = evaluate(sample({ temp_mc: 71000 }), {}, th);
eq('evaluate_temp_health', r.health, 'warn');
ok('evaluate_temp_id', has(r.alerts, 'temp_warn'));

r = evaluate(sample({}), { now: now, last_deact_t: now - 300 }, th);
eq('evaluate_deact_recent_health', r.health, 'crit');
ok('evaluate_deact_recent_id', has(r.alerts, 'deactivate_recent'));
r = evaluate(sample({}), { now: now, last_deact_t: now - 700 }, th);
ok('evaluate_deact_old', !has(r.alerts, 'deactivate_recent'));

r = evaluate(sample({}), { bip_last_hour: 3 }, th);
ok('evaluate_bip_hour', has(r.alerts, 'bip_hour'));

let s = normalize_status({ state: 1, telemetry_valid: 0, bosa_rx_power_nw: 8400 });
eq('normalize_no_telemetry_rx', s.rx_nw, null);
r = evaluate(s, {}, th);
eq('evaluate_not_o5_without_telemetry_health', r.health, 'crit');
ok('evaluate_not_o5_without_telemetry_id', has(r.alerts, 'not_o5'));

s = normalize_status(null);
eq('normalize_null_state', s.state, null);

s = normalize_status({ state: 5, onu_id: 1, telemetry_valid: 255, bosa_rx_power_nw: 8400,
	bosa_tx_power_nw: 1480000, bosa_temperature_mc: 50776, bosa_bias_ua: 17142,
	bosa_voltage_uv: 3258500, bosa_alarms: 0, fec_downstream: 2, fec_upstream: 2,
	olt_profile_effective_name: 'huawei', olt: { vendor_id: 'HWTC', version: '10' } });
eq('normalize_rx', s.rx_nw, 8400);
eq('normalize_olt_vendor', s.olt_vendor, 'HWTC');
eq('normalize_olt_profile', s.olt_profile, 'huawei');
eq('normalize_fec_ds', s.fec_ds, 2);

// detect()
eq('detect_o5_up', types(detect(sample({ state: 1 }), sample({}), 0, th))[0], 'o5_up');
eq('detect_o5_down', types(detect(sample({}), sample({ state: 1 }), 0, th))[0], 'o5_down');
ok('detect_bip_burst', 'bip_burst' in types(detect(sample({}), sample({}), 12, th)));
eq('detect_rx_warn_enter', types(detect(sample({}), sample({ rx_nw: 3000 }), 0, th)), ['rx_warn']);
eq('detect_rx_warn_stay', length(detect(sample({ rx_nw: 3000 }), sample({ rx_nw: 2900 }), 0, th)), 0);
ok('detect_resync', 'resync' in types(detect(sample({ sfifo_resync: 0 }), sample({ sfifo_resync: 1 }), 0, th)));
ok('detect_no_resync_when_null', !('resync' in types(detect(sample({}), sample({}), 0, th))));
ok('detect_bosa_alarm', 'bosa_alarm' in types(detect(sample({ alarms: 0 }), sample({ alarms: 4 }), 0, th)));

// aggregate()
let samples = [];
for (let i = 0; i < 10; i++)
	push(samples, { t: now + 60 * i, rx_nw: 8000 + 100 * i, temp_mc: 50000, bip: 1 });
let b = aggregate(samples, 300);
eq('aggregate_buckets', length(b), 2);
eq('aggregate_n', b[0].n, 5);
eq('aggregate_rx_min', b[0].rx_min, 8000);
eq('aggregate_rx_max', b[0].rx_max, 8400);
near('aggregate_rx_avg', b[0].rx_avg, 8200, 0.001);
eq('aggregate_bip_sum', b[0].bip, 5);
b = aggregate([ { t: now, rx_nw: 8000 }, { t: now + 60, rx_nw: 8001 } ], 300);
near('aggregate_avg_fractional', b[0].rx_avg, 8000.5, 0.001);	// int/int truncation guard
b = aggregate([ { t: now, rx_nw: null }, { t: now + 60, rx_nw: 100 } ], 300);
eq('aggregate_null_rx_n', b[0].n, 2);
eq('aggregate_null_rx_min', b[0].rx_min, 100);
near('aggregate_null_rx_avg', b[0].rx_avg, 100, 0.001);

// ring_push()
let ring = [1, 2, 3];
ring_push(ring, 4, 3);
eq('ring_push_cap', ring, [2, 3, 4]);

done();
