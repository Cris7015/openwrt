// Replay tests for recorder.uc. Run on the router:
// scripts/gpon-panel-test.sh <pkgdir> tests/recorder_test.uc
'use strict';

import { ok, eq, done } from 'assert';
import * as fs from 'fs';

const BASE = '/tmp/xr500v-gpon-test';
const T0 = 1800000000;	// aligned to 300 s and 1800 s buckets
const DEACT = '[  100.000000] airoha-xpon 1fb60000.xmac: restarting GPON after Deactivate_ONU-ID from the OLT';
const DEACT2 = '[  150.000000] airoha-xpon 1fb60000.xmac: restarting GPON after Deactivate_ONU-ID from the OLT';
const LINKUP = '[  100.000000] airoha-xpon 1fb60000.xmac: GPON netdev link ready';

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
eq('recorder_replay_basic_no_gem', r.out?.gem, null);

// recorder_replay_gem_mib: per-GEM totals become rates; a total that goes
// backwards (new session) gives no rate, and "enabled=0" lists no port.
function gemmib(rx, tx) {
	return `enabled=1 ready=1 epoch=2 type=gem\n` +
		`gem=257 rx_frames=10 rx_bytes=${rx} tx_frames=5 tx_bytes=${tx}\n` +
		`gem=4095 rx_frames=0 rx_bytes=0 tx_frames=0 tx_bytes=0\n`;
}
r = replay('gem', { samples: [
	sample(T0, { gem_mib: gemmib(1000, 500) }),
	sample(T0 + 60, { gem_mib: gemmib(751000, 15500) })
] });
eq('recorder_replay_gem_ports', length(r.out?.gem?.ports), 2);
eq('recorder_replay_gem_id', r.out?.gem?.ports?.[0]?.id, 257);
eq('recorder_replay_gem_rx_bytes', r.out?.gem?.ports?.[0]?.rx_bytes, 751000);
eq('recorder_replay_gem_rx_bps', r.out?.gem?.ports?.[0]?.rx_bps, 100000);
eq('recorder_replay_gem_tx_bps', r.out?.gem?.ports?.[0]?.tx_bps, 2000);
r = replay('gem_reset', { samples: [
	sample(T0, { gem_mib: gemmib(751000, 15500) }),
	sample(T0 + 60, { gem_mib: gemmib(1000, 500) })
] });
eq('recorder_replay_gem_reset_no_rate', r.out?.gem?.ports?.[0]?.rx_bps, null);
r = replay('gem_off', { samples: [ sample(T0, { gem_mib: 'enabled=0\n' }) ] });
eq('recorder_replay_gem_off_enabled', r.out?.gem?.enabled, false);
eq('recorder_replay_gem_off_ports', length(r.out?.gem?.ports), 0);

// recorder_replay_deactivate: a Deactivate in the kernel log at the end.
samples = [];
for (let i = 0; i < 10; i++)
	push(samples, sample(T0 + 60 * i));
// Kernel lines carry their own time: [100 s] after a boot at T0 + 440.
r = replay('deact', { boot: T0 + 440, samples, klog: [ { t: T0 + 540, line: DEACT } ] });
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
eq('recorder_replay_survives_missing_status_health', r.out?.health, 'unknown');

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

// recorder_klog_skips_lines_before_start: lines already in klog.log when the
// daemon starts were counted from dmesg; only lines appended later are read.
fs.writefile(`${BASE}/klog.log`, DEACT + '\n');
r = replay('klogstart', { boot: T0, klog_file: `${BASE}/klog.log`,
	klog_append: [ { t: T0 + 120, line: DEACT2 } ],
	samples: [ sample(T0), sample(T0 + 60), sample(T0 + 180) ] });
eq('recorder_klog_skips_lines_before_start_counter', r.out?.counters?.deactivate, 1);
eq('recorder_klog_skips_lines_before_start_events',
	length(filter(types(r.out?.events), (t) => t == 'deactivate')), 1);

// recorder_o5_since_from_kernel_survives_clock_step: the link-ready line is
// read before NTP (wrong clock); the time in O5 uses the clock at query time.
r = replay('kstep', { boot: T0 - 1000, klog: [ { t: 1800, line: LINKUP } ],
	samples: [ sample(T0, { up: 1000 }) ] });
eq('recorder_o5_since_from_kernel_survives_clock_step', r.out?.o5_since, T0 - 900);

// recorder_o5_since_from_sample_survives_clock_step: first O5 sample taken
// with a stale clock (a day behind), then NTP steps it.
r = replay('sstep', { boot: T0 - 3600,
	samples: [ sample(T0 - 86400, { up: 200 }), sample(T0, { up: 3600 }) ] });
eq('recorder_o5_since_from_sample_survives_clock_step', r.out?.o5_since, T0 - 3400);

// recorder_crit_events_bypass_write_budget: a flood of warnings must not keep
// a link drop off the flash.
fs.unlink(`${BASE}/events.jsonl`);
samples = [];
for (let i = 0; i < 6; i++)
	push(samples, sample(T0 + 60 * i, { sfifo_resync: `${i}\n` }));
push(samples, sample(T0 + 360, { status: status({ state: 1 }) }));
r = replay('budget', { events_file: `${BASE}/events.jsonl`, writes_per_day: 2, samples });
let written = filter(split(fs.readfile(`${BASE}/events.jsonl`) || '', '\n'), length);
eq('recorder_crit_events_bypass_write_budget_warn',
	length(filter(written, (l) => index(l, '"resync"') >= 0)), 2);
eq('recorder_crit_events_bypass_write_budget_crit',
	length(filter(written, (l) => index(l, '"o5_down"') >= 0)), 1);

// recorder_nofiber_quiet: never in O5 and no light (e.g. WAN over Ethernet).
const NOLIGHT = status({ state: 1, bosa_rx_power_nw: 0 });
samples = [];
for (let i = 0; i < 20; i++)
	push(samples, sample(T0 + 60 * i, { status: NOLIGHT }));
r = replay('nofiber', { samples });
eq('recorder_nofiber_quiet_events', types(r.out?.events), []);
eq('recorder_nofiber_quiet_health', r.out?.health, 'nofiber');
eq('recorder_nofiber_quiet_ever_o5', r.out?.ever_o5, false);

// recorder_first_o5_ends_nofiber: the first O5 ends the quiet state; a later
// drop is a real fault.
samples = [];
for (let i = 0; i < 5; i++)
	push(samples, sample(T0 + 60 * i, { status: NOLIGHT }));
for (let i = 5; i < 10; i++)
	push(samples, sample(T0 + 60 * i));
push(samples, sample(T0 + 600, { status: status({ state: 1 }) }));
r = replay('firsto5', { samples });
eq('recorder_first_o5_ends_nofiber_events', types(r.out?.events), [ 'o5_up', 'o5_down' ]);
eq('recorder_first_o5_ends_nofiber_ever_o5', r.out?.ever_o5, true);
eq('recorder_first_o5_ends_nofiber_health', r.out?.health, 'crit');

// recorder_restart_does_not_refire: a restart keeps the last known sample, so
// a warning that already fired does not fire again.
const WEAK = status({ bosa_rx_power_nw: 3000 });
fs.unlink(`${BASE}/warn.json.gz`);
r = replay('warn1', { save: `${BASE}/warn.json.gz`,
	samples: [ sample(T0, { status: WEAK }), sample(T0 + 60, { status: WEAK }) ] });
eq('recorder_restart_does_not_refire_first', types(r.out?.events), [ 'rx_warn' ]);
r = replay('warn2', { snapshot: `${BASE}/warn.json.gz`, samples: [ sample(T0 + 120, { status: WEAK }) ] });
eq('recorder_restart_does_not_refire', types(r.out?.events), []);

// recorder_budget_survives_restart: today's writes already in the file count
// against the daily budget after a restart.
fs.writefile(`${BASE}/budget2.jsonl`,
	sprintf('%J\n%J\n', { t: T0 - 100, type: 'resync', severity: 'warn', msg: 'x' },
		{ t: T0 - 50, type: 'resync', severity: 'warn', msg: 'x' }));
r = replay('budget2', { events_file: `${BASE}/budget2.jsonl`, writes_per_day: 2,
	samples: [ sample(T0, { sfifo_resync: '0\n' }), sample(T0 + 60, { sfifo_resync: '1\n' }) ] });
eq('recorder_budget_survives_restart_event', 'resync' in types(r.out?.events), true);
eq('recorder_budget_survives_restart_file',
	length(filter(split(fs.readfile(`${BASE}/budget2.jsonl`) || '', '\n'), length)), 2);

// --- v2: saved ±30 min windows around critical events ---
// A Deactivate kernel line at `up` seconds after a boot at T0 + 2300.
function deact_at(up) {
	return { t: T0 + 2300 + up, line: sprintf('[%6d.000000] airoha-xpon 1fb60000.xmac: restarting GPON after Deactivate_ONU-ID from the OLT', up) };
}

function every(step, from, to) {
	let s = [];
	for (let t = from; t <= to; t += step)
		push(s, sample(t));
	return s;
}

function gz_windows(path) {
	let p = fs.popen(`gzip -dc ${path} 2>/dev/null`);
	let text = p ? p.read('all') : null;
	if (p)
		p.close();
	let d = null;
	try { d = json(text); } catch (e) { }
	return d?.windows;
}

// recorder_window_saved_after_half_hour
fs.unlink(`${BASE}/win1.json.gz`);
r = replay('win1', { boot: T0 + 2300, windows_file: `${BASE}/win1.json.gz`,
	klog: [ deact_at(100) ], samples: every(60, T0, T0 + 5940) });
eq('recorder_window_saved_after_half_hour_count', length(r.out?.windows), 1);
eq('recorder_window_saved_after_half_hour_range', [ r.out?.windows?.[0]?.t0, r.out?.windows?.[0]?.t1 ], [ T0 + 600, T0 + 4200 ]);
eq('recorder_window_saved_after_half_hour_points', r.out?.windows?.[0]?.n, 61);
eq('recorder_window_saved_after_half_hour_file', length(gz_windows(`${BASE}/win1.json.gz`)), 1);
eq('recorder_window_marks_event', filter(r.out?.listed || [], (e) => e.type == 'deactivate')[0]?.win, true);

// recorder_window_not_before_half_hour
r = replay('win2', { boot: T0 + 2300, klog: [ deact_at(100) ], samples: every(60, T0, T0 + 3000) });
eq('recorder_window_not_before_half_hour', length(r.out?.windows), 0);

// recorder_window_merges_close_events
r = replay('win3', { boot: T0 + 2300, klog: [ deact_at(100), deact_at(700) ], samples: every(60, T0, T0 + 5940) });
eq('recorder_window_merges_close_events_count', length(r.out?.windows), 1);
eq('recorder_window_merges_close_events_range', [ r.out?.windows?.[0]?.t0, r.out?.windows?.[0]?.t1 ], [ T0 + 600, T0 + 4800 ]);

// recorder_window_caps: 25 critical events 2 h apart, 20 windows kept
let kl = [];
for (let i = 0; i < 25; i++)
	push(kl, { t: T0 + 3600 + i * 7200, line: sprintf('[%6d.000000] airoha-xpon 1fb60000.xmac: restarting GPON after Deactivate_ONU-ID from the OLT', 3600 + i * 7200) });
r = replay('win4', { boot: T0, windows_per_day: 30, klog: kl, samples: every(600, T0, T0 + 3600 + 25 * 7200) });
eq('recorder_window_caps_count', length(r.out?.windows), 20);
eq('recorder_window_caps_oldest', r.out?.windows?.[0]?.t0, T0 + 3600 + 5 * 7200 - 1800);

// recorder_window_daily_cap: 3 windows the same day, 2 written
fs.unlink(`${BASE}/win5.json.gz`);
r = replay('win5', { boot: T0, windows_file: `${BASE}/win5.json.gz`, windows_per_day: 2,
	klog: slice(kl, 0, 3), samples: every(600, T0, T0 + 21600) });
eq('recorder_window_daily_cap_ram', length(r.out?.windows), 3);
eq('recorder_window_daily_cap_file', length(gz_windows(`${BASE}/win5.json.gz`)), 2);

// recorder_window_after_clock_step: NTP steps the clock while a window is pending
r = replay('win6', { boot: T0 + 2300, klog: [ deact_at(100) ],
	samples: [ ...every(60, T0, T0 + 3000), sample(T0 + 90000) ] });
eq('recorder_window_after_clock_step_count', length(r.out?.windows), 1);
ok('recorder_window_after_clock_step_range', r.out?.windows?.[0]?.t0 <= r.out?.windows?.[0]?.t1 && r.out?.windows?.[0]?.n <= 61);
eq('recorder_window_after_clock_step_pending', r.out?.pending, null);

// recorder_nofiber_no_window: a Deactivate while the port never reached O5
samples = [];
for (let i = 0; i < 100; i++)
	push(samples, sample(T0 + 60 * i, { status: NOLIGHT }));
r = replay('win7', { boot: T0 + 2300, klog: [ deact_at(100) ], samples });
eq('recorder_nofiber_no_window', length(r.out?.windows), 0);

// recorder_around_fallback: no saved window -> rings, then nothing
r = replay('around1', { samples: every(60, T0, T0 + 5940), around: T0 + 3000 });
eq('recorder_around_fallback_1min', [ r.out?.around?.source, length(r.out?.around?.points) ], [ '1min', 61 ]);
let m5 = [];
for (let i = 0; i < 24; i++)
	push(m5, { t: T0 + 300 * i, n: 5, rx_min: 8400, rx_avg: 8400, rx_max: 8400,
		temp_min: 50000, temp_avg: 50000, temp_max: 50000, bip: 0 });
fs.writefile(`${BASE}/m5only.json`, sprintf('%J', { version: 1, m1: [], m5, m30: [], p5: [], p30: [] }));
system(`gzip -c ${BASE}/m5only.json > ${BASE}/m5only.json.gz`);
r = replay('around2', { snapshot: `${BASE}/m5only.json.gz`, samples: [], around: T0 + 3600 });
eq('recorder_around_fallback_5min', [ r.out?.around?.source, r.out?.around?.step, length(r.out?.around?.points) ], [ '5min', 300, 13 ]);
r = replay('around3', { samples: every(60, T0, T0 + 600), around: T0 - 100000 });
eq('recorder_around_fallback_none', [ r.out?.around?.source, length(r.out?.around?.points) ], [ 'none', 0 ]);

// recorder_periodic_persist_keeps_window_pending (final review, Important 2):
// the 6-hourly save lands 10 min after the event; the window must still end
// 30 min after it, with its full detail.
fs.unlink(`${BASE}/win8.json.gz`);
r = replay('win8', { boot: T0 + 2300, windows_file: `${BASE}/win8.json.gz`, klog: [ deact_at(100) ],
	persist_at: [ T0 + 3000 ], samples: every(60, T0, T0 + 5940) });
eq('recorder_periodic_persist_keeps_window_pending_count', length(r.out?.windows), 1);
eq('recorder_periodic_persist_keeps_window_pending_points', r.out?.windows?.[0]?.n, 61);
eq('recorder_periodic_persist_keeps_window_pending_file', length(gz_windows(`${BASE}/win8.json.gz`)?.[0]?.points), 61);

// recorder_reboot_does_not_fake_drop (final review, Important 3): after a
// reboot the snapshot looks recent (sysfixtime), but its last sample is from
// another boot: the link not being up yet is not a drop.
fs.unlink(`${BASE}/boot.json.gz`);
replay('bootA', { boot_id: 'A', save: `${BASE}/boot.json.gz`, samples: [ sample(T0, { status: WEAK }), sample(T0 + 60, { status: WEAK }) ] });
r = replay('bootB', { boot_id: 'B', snapshot: `${BASE}/boot.json.gz`, samples: [ sample(T0 + 120, { status: status({ state: 1 }) }) ] });
eq('recorder_reboot_does_not_fake_drop_events', types(r.out?.events), []);
eq('recorder_reboot_does_not_fake_drop_ever_o5', r.out?.ever_o5, false);
r = replay('bootC', { boot_id: 'B', snapshot: `${BASE}/boot.json.gz`, samples: [ sample(T0 + 120, { status: WEAK }) ] });
eq('recorder_reboot_does_not_refire', types(r.out?.events), []);

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
