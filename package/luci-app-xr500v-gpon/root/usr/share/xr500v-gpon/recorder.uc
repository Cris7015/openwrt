// GPON fiber health recorder (luci-app-xr500v-gpon).
//
// Every `interval` seconds it samples read-only sources (omci.agent status,
// the xmac sysfs BIP/GEM counters and, when present, the lab debugfs
// resync counters), keeps 1-min/5-min/30-min rings in RAM, turns changes
// into events, and publishes ubus `xr500v.gpon`. Kernel log lines from the
// xPON driver arrive through a `logread -f` instance of the init script
// that appends them to KLOG_FILE; they are read incrementally on each tick.
// The rings are saved to the overlay every `persist_hours` and on stop;
// events are appended as they happen, with a daily write budget.
//
// `ucode -L <dir> recorder.uc --replay fixture.json` runs the same logic
// on recorded samples, without uloop or ubus (tests).
'use strict';

import * as fs from 'fs';
import * as uloop from 'uloop';
import * as ubus from 'ubus';
import { cursor } from 'uci';
import {
	normalize_status, parse_bip, thresholds, evaluate, detect, aggregate, ring_push
} from 'lib';

const CLOCK_MIN = 1704067200;	// 2024-01-01: older means the clock is not synced yet
const XMAC = '/sys/bus/platform/drivers/airoha-xpon/1fb60000.xmac';
const DBG = '/sys/kernel/debug/1fb60000.xmac';
const STATE_DIR = '/etc/xr500v-gpon';
const SNAPSHOT = STATE_DIR + '/history.json.gz';
const EVENTS_FILE = STATE_DIR + '/events.jsonl';
const KLOG_FILE = '/tmp/xr500v-gpon/klog.log';
const KLOG_MAX = 65536;
// ring -> [bucket seconds, capacity, pending list]
const RINGS = { m5: [ 300, 2016, 'p5' ], m30: [ 1800, 1440, 'p30' ] };
const M1_CAP = 1440;

let cfg = { interval: 60, persist_hours: 6, events_max: 500, event_writes_per_day: 100 };
let th = thresholds(null);
let sink = null;	// daemon only: event -> overlay

let st = {
	m1: [], m5: [], m30: [], p5: [], p30: [], events: [],
	last: null, info: null, serial: null,
	o5_since: null, deact: 0, last_deact_t: null, skipped: 0
};

function num(text) {
	if (text == null)
		return null;
	let s = trim('' + text);
	if (s === '')
		return null;
	let n = +s;
	return (n != n) ? null : n;
}

function add_event(t, ev) {
	let e = { t, type: ev.type, severity: ev.severity, msg: ev.msg };
	ring_push(st.events, e, cfg.events_max);
	if (sink)
		sink(e);
}

// Closes the 5/30-min buckets the sample leaves behind, then queues it.
function bucket(sample) {
	for (let ring, spec in RINGS) {
		let step = spec[0], pend = st[spec[2]];
		if (length(pend) && pend[0].t - pend[0].t % step != sample.t - sample.t % step) {
			ring_push(st[ring], aggregate(pend, step)[0], spec[1]);
			st[spec[2]] = pend = [];
		}
		push(pend, sample);
	}
}

function ingest(raw) {
	if (!(raw?.t >= CLOCK_MIN)) {
		st.skipped++;
		return;
	}
	let s = normalize_status(raw.status), bip = parse_bip(raw.bip), prev = st.last;
	let sample = {
		t: raw.t, state: s.state,
		rx_nw: s.rx_nw, tx_nw: s.tx_nw, temp_mc: s.temp_mc,
		bias_ua: s.bias_ua, volt_uv: s.volt_uv, alarms: s.alarms,
		bip_total: bip?.total, bip_session: bip?.session_total,
		gem_delin: num(raw.gem_delin),
		sfifo_resync: num(raw.sfifo_resync), sfifo_glitch: num(raw.sfifo_glitch),
		bip: 0
	};
	// A counter that went backwards was reset (driver reload): count from 0.
	if (sample.bip_total != null && prev?.bip_total != null)
		sample.bip = sample.bip_total >= prev.bip_total
			? sample.bip_total - prev.bip_total : sample.bip_total;
	if (sample.state != 5)
		st.o5_since = null;
	else if (st.o5_since == null)
		st.o5_since = sample.t;

	for (let ev in detect(prev, sample, sample.bip, th))
		add_event(sample.t, ev);
	ring_push(st.m1, sample, M1_CAP);
	bucket(sample);
	st.last = sample;
	st.info = s;
	if (raw.status?.serial)
		st.serial = raw.status.serial;
}

function on_klog(t, line) {
	if (index(line, 'Deactivate_ONU-ID') >= 0) {
		st.deact++;
		st.last_deact_t = t;
		add_event(t, { type: 'deactivate', severity: 'crit',
			msg: 'OLT sent Deactivate_ONU-ID; GPON restarting' });
	} else if (index(line, 'GPON netdev link ready') >= 0) {
		st.o5_since = t;
	}
}

function bip_last_hour(now) {
	let sum = 0;
	for (let s in st.m1)
		if (s.t > now - 3600)
			sum += s.bip;
	return sum;
}

function assess(now) {
	return evaluate(st.last, {
		now, bip_last_min: st.last?.bip, bip_last_hour: bip_last_hour(now),
		last_deact_t: st.last_deact_t
	}, th);
}

function summary(now) {
	let l = st.last, i = st.info || {}, a = assess(now);
	return {
		now, stale: !l || now - l.t > 3 * cfg.interval,
		health: a.health, alerts: a.alerts,
		state: l?.state, o5_since: st.o5_since,
		olt: { vendor: i.olt_vendor, version: i.olt_version, profile: i.olt_profile },
		onu_id: i.onu_id, fec: { ds: i.fec_ds, us: i.fec_us },
		optics: { rx_nw: l?.rx_nw, tx_nw: l?.tx_nw, temp_mc: l?.temp_mc,
			bias_ua: l?.bias_ua, volt_uv: l?.volt_uv, alarms: l?.alarms },
		counters: { bip_total: l?.bip_total, bip_session: l?.bip_session,
			gem_delin: l?.gem_delin, sfifo_resync: l?.sfifo_resync,
			sfifo_glitch: l?.sfifo_glitch, deactivate: st.deact },
		serial_hint: st.serial ? substr(st.serial, 0, 4) + '…' : null
	};
}

function history(window) {
	if (window == '7d' || window == '30d') {
		let ring = (window == '7d') ? 'm5' : 'm30', spec = RINGS[ring];
		let points = slice(st[ring]);
		if (length(st[spec[2]]))
			push(points, aggregate(st[spec[2]], spec[0])[0]);
		return { window, step: spec[0], points };
	}
	return { window: '24h', step: 60, points: map(st.m1, (s) => ({
		t: s.t, n: 1,
		rx_min: s.rx_nw, rx_avg: s.rx_nw, rx_max: s.rx_nw,
		temp_min: s.temp_mc, temp_avg: s.temp_mc, temp_max: s.temp_mc,
		bip: s.bip
	})) };
}

function events_list(limit) {
	let out = [], n = length(st.events);
	if (!(limit > 0) || limit > n)
		limit = n;
	for (let i = n - 1; i >= n - limit; i--)
		push(out, st.events[i]);
	return out;
}

function save_snapshot(path) {
	let raw = '/tmp/xr500v-gpon-snapshot.json';
	fs.writefile(raw, sprintf('%J', {
		version: 1, saved: time(),
		m1: st.m1, m5: st.m5, m30: st.m30, p5: st.p5, p30: st.p30
	}));
	let rc = system([ '/bin/sh', '-c', 'gzip -c "$1" > "$2.tmp" && mv "$2.tmp" "$2"',
		'snapshot', raw, path ]);
	fs.unlink(raw);
	return rc == 0;
}

// A missing, truncated or otherwise unreadable snapshot starts empty rings.
function load_snapshot(path) {
	if (!fs.stat(path))
		return false;
	let p = fs.popen(sprintf('gzip -dc %s 2>/dev/null', path));
	let text = p ? p.read('all') : null;
	if (p)
		p.close();
	let data = null;
	try { data = json(text); } catch (e) { data = null; }
	if (type(data) != 'object' || type(data.m1) != 'array') {
		warn(`xr500v-gpon: ignoring unreadable snapshot ${path}\n`);
		return false;
	}
	for (let k in [ 'm1', 'm5', 'm30', 'p5', 'p30' ])
		if (type(data[k]) == 'array')
			st[k] = data[k];
	return true;
}

let writes = { day: null, n: 0 };

function append_event(e) {
	let day = int(e.t / 86400);
	if (writes.day != day) {
		writes.day = day;
		writes.n = 0;
	}
	if (writes.n >= cfg.event_writes_per_day)
		return;	// stays in RAM; the next persist() rewrites the file
	writes.n++;
	let f = fs.open(EVENTS_FILE, 'a');
	if (f) {
		f.write(sprintf('%J\n', e));
		f.close();
	}
}

function load_events() {
	let text = fs.readfile(EVENTS_FILE);
	for (let line in split(text || '', '\n')) {
		if (!length(line))
			continue;
		try { ring_push(st.events, json(line), cfg.events_max); } catch (e) { }
	}
}

function persist() {
	let ok = save_snapshot(SNAPSHOT);
	let tmp = EVENTS_FILE + '.tmp';
	fs.writefile(tmp, join('', map(st.events, (e) => sprintf('%J\n', e))));
	fs.rename(tmp, EVENTS_FILE);
	return ok;
}

function load_config() {
	let uci = cursor();
	let m = uci.get_all('xr500v-gpon', 'main') || {};
	for (let k in [ 'interval', 'persist_hours', 'events_max', 'event_writes_per_day' ]) {
		let n = num(m[k]);
		if (n > 0)
			cfg[k] = n;
	}
	th = thresholds(uci.get_all('xr500v-gpon', 'thresholds'));
}

function boot_time() {
	return time() - int(num(split(fs.readfile('/proc/uptime') || '0', ' ')[0]) || 0);
}

// "[ 6747.947031] airoha-xpon ..." -> wall clock seconds
function kernel_time(line, boot) {
	let m = match(line, /\[ *([0-9]+)\.[0-9]+\]/);
	return m ? boot + int(m[1]) : time();
}

// Deactivates and the last O5 since boot, from one dmesg read at start.
function startup_scan() {
	let boot = boot_time(), p = fs.popen('dmesg');
	if (!p)
		return;
	for (let line = p.read('line'); length(line); line = p.read('line')) {
		if (index(line, 'Deactivate_ONU-ID') >= 0) {
			st.deact++;
			st.last_deact_t = kernel_time(line, boot);
		} else if (index(line, 'GPON netdev link ready') >= 0) {
			st.o5_since = kernel_time(line, boot);
		}
	}
	p.close();
}

let klog_off = 0;

// New complete lines appended by the logread instance since the last tick.
function read_klog() {
	let size = fs.stat(KLOG_FILE)?.size;
	if (size == null)
		return;
	if (size < klog_off)
		klog_off = 0;	// truncated
	let f = fs.open(KLOG_FILE, 'r');
	if (!f)
		return;
	f.seek(klog_off);
	let text = f.read('all') || '';
	f.close();
	let nl = rindex(text, '\n');
	if (nl < 0)
		return;
	klog_off += nl + 1;
	let boot = boot_time();
	for (let line in split(substr(text, 0, nl), '\n'))
		if (length(line))
			on_klog(kernel_time(line, boot), line);
	if (klog_off > KLOG_MAX) {
		fs.writefile(KLOG_FILE, '');
		klog_off = 0;
	}
}

function collect(bus) {
	let status = null;
	try {
		status = bus.call('omci.agent', 'status', { device: 0 })?.status;
	} catch (e) {
		status = null;
	}
	return {
		t: time(), status,
		bip: fs.readfile(XMAC + '/gpon_bip'),
		gem_delin: fs.readfile(XMAC + '/gpon_gem_delin'),
		sfifo_resync: fs.readfile(DBG + '/gpon_sfifo_resync_count'),
		sfifo_glitch: fs.readfile(DBG + '/gpon_sfifo_glitch_count')
	};
}

function replay(path) {
	let fx = json(fs.readfile(path));
	if (fx.snapshot)
		load_snapshot(fx.snapshot);
	let klog = fx.klog || [], k = 0;
	for (let raw in fx.samples || []) {
		for (; k < length(klog) && klog[k].t <= raw.t; k++)
			on_klog(klog[k].t, klog[k].line);
		ingest(raw);
	}
	for (; k < length(klog); k++)
		on_klog(klog[k].t, klog[k].line);
	if (fx.save)
		save_snapshot(fx.save);
	let now = st.last ? st.last.t : time();
	print(sprintf('%J\n', {
		events: st.events, m1_len: length(st.m1),
		m5_len: length(history('7d').points), health: assess(now).health,
		counters: summary(now).counters, skipped: st.skipped, last: st.last
	}));
}

function daemon() {
	load_config();
	fs.mkdir(STATE_DIR);
	load_snapshot(SNAPSHOT);
	load_events();
	sink = append_event;
	startup_scan();
	uloop.init();
	let bus = ubus.connect();
	bus.publish('xr500v.gpon', {
		summary: { call: () => summary(time()) },
		history: { args: { window: '' }, call: (req) => history(req.args.window) },
		events: { args: { limit: 0 }, call: (req) => ({ events: events_list(req.args.limit) }) },
		persist: { call: () => ({ ok: persist() }) },
		reload: { call: () => { load_config(); return { ok: true }; } }
	});
	let tick = () => {
		read_klog();
		ingest(collect(bus));
	};
	let sampler, saver;
	sampler = uloop.timer(cfg.interval * 1000, () => {
		tick();
		sampler.set(cfg.interval * 1000);
	});
	saver = uloop.timer(cfg.persist_hours * 3600000, () => {
		persist();
		saver.set(cfg.persist_hours * 3600000);
	});
	tick();
	uloop.run();
}

if (ARGV[0] == '--replay')
	replay(ARGV[1]);
else
	daemon();
