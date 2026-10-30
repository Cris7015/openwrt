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
import { eye_scan } from 'eye';

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
// ±30 min around critical events, kept so "what happened before" still has
// 1-minute detail once the event is older than the 1-minute ring (24 h).
const WIN_HALF = 1800;
const WIN_MAX_SPAN = 7200;	// close events share one window, up to 2 h
const WIN_KEEP = 20;
const WINDOWS_FILE = STATE_DIR + '/windows.json.gz';

let cfg = { interval: 60, persist_hours: 6, events_max: 500, event_writes_per_day: 100, windows_per_day: 10 };
let th = thresholds(null);
let sink = null;	// daemon only: event -> overlay

let st = {
	m1: [], m5: [], m30: [], p5: [], p30: [], events: [],
	last: null, known: null, info: null, serial: null,
	// moments kept as uptime when known (immune to NTP steps), else wall time
	o5_up: null, o5_t: null, deact: 0, deact_up: null, deact_t: null,
	// in O5 at least once since boot; until then an unused fiber port
	// (WAN over Ethernet, say) raises no alert and no event
	ever_o5: false, snap_boot: null,
	windows: [], win_pending: null, win_dirty: false,
	skipped: 0,
	// latest gpon_gem_mib reading; rates against the previous one
	gem: null
};

let klog_path = KLOG_FILE;
// set by the daemon only: replays never write /etc
let snapshot_path = null, events_path = null, windows_path = null;
let win_writes = { day: null, n: 0 };

function num(text) {
	if (text == null)
		return null;
	let s = trim('' + text);
	if (s === '')
		return null;
	let n = +s;
	return (n != n) ? null : n;
}

function fields(line) {
	let f = {};
	for (let kv in split(line, ' ')) {
		let p = split(kv, '=', 2);
		if (length(p) == 2)
			f[p[0]] = p[1];
	}
	return f;
}

// gpon_gem_mib: "enabled=1 ready=1 epoch=3 type=gem", then one line per GEM
// port with running totals. A total that went backwards belongs to a new
// session (MAC reset): no rate for that reading.
function parse_gem_mib(text, t, prev) {
	if (text == null)
		return null;
	let lines = split(trim(text), '\n'), head = fields(lines[0]);
	let gem = { t, enabled: head.enabled == '1', ready: head.ready == '1',
		type: head.type, ports: [] };
	for (let i = 1; i < length(lines); i++) {
		let f = fields(lines[i]), id = num(f.gem);
		if (id == null)
			continue;
		let port = { id, rx_frames: num(f.rx_frames), rx_bytes: num(f.rx_bytes),
			tx_frames: num(f.tx_frames), tx_bytes: num(f.tx_bytes) };
		let old = (prev && t > prev.t) ? filter(prev.ports, (o) => o.id == id)[0] : null;
		if (old && port.rx_bytes >= old.rx_bytes && port.tx_bytes >= old.tx_bytes) {
			port.rx_bps = (port.rx_bytes - old.rx_bytes) * 8 / (t - prev.t);
			port.tx_bps = (port.tx_bytes - old.tx_bytes) * 8 / (t - prev.t);
		}
		push(gem.ports, port);
	}
	return gem;
}

function uptime() {
	let s = num(split(fs.readfile('/proc/uptime') || '', ' ')[0]);
	return (s != null) ? int(s) : null;
}

function boot_time() {
	return time() - (uptime() || 0);
}

let boot_of = boot_time;	// replay: the fixture's boot time

function boot_id() {
	return trim(fs.readfile('/proc/sys/kernel/random/boot_id') || '');
}

let boot_id_of = boot_id;	// replay: the fixture's

// Wall-clock time of a moment, converted with the clock as it is now.
function wall(up, t) {
	return (up != null) ? boot_of() + up : t;
}

// "[ 6747.947031] airoha-xpon ..." -> seconds since boot, or null
function kernel_up(line) {
	let m = match(line, /\[ *([0-9]+)\.[0-9]+\]/);
	return m ? int(m[1]) : null;
}

// gzip a JSON value to path through a synced temporary file.
function write_gz(path, value) {
	let raw = '/tmp/xr500v-gpon-write.json';
	fs.writefile(raw, sprintf('%J', value));
	let rc = system([ '/bin/sh', '-c', 'gzip -c "$1" > "$2.tmp" && sync && mv "$2.tmp" "$2"',
		'write_gz', raw, path ]);
	fs.unlink(raw);
	return rc == 0;
}

// The JSON value in a gzip file, or null if missing, truncated or unreadable.
function read_gz(path) {
	if (!path || !fs.stat(path))
		return null;
	let p = fs.popen(sprintf('gzip -dc %s 2>/dev/null', path));
	let text = p ? p.read('all') : null;
	if (p)
		p.close();
	let data = null;
	try { data = json(text); } catch (e) { data = null; }
	return data;
}

// A window still open goes to the file as far as it has got (a power cut
// keeps that much); its final capture replaces it.
function save_windows() {
	let all = slice(st.windows), p = st.win_pending;
	st.win_dirty = false;
	if (p)
		push(all, { t0: p.t0, t1: p.t1, saved: time(), partial: true,
			points: filter(st.m1, (s) => s.t >= p.t0 && s.t <= p.t1) });
	return windows_path ? write_gz(windows_path, { version: 1, windows: all }) : false;
}

// Closes the pending window with the 1-minute samples it covers; writes the
// file within the daily budget, otherwise the next persist() does.
function capture_window(now, write) {
	let p = st.win_pending, day = int(now / 86400);
	st.win_pending = null;
	ring_push(st.windows, { t0: p.t0, t1: p.t1, saved: now,
		points: filter(st.m1, (s) => s.t >= p.t0 && s.t <= p.t1) }, WIN_KEEP);
	st.win_dirty = true;
	if (win_writes.day != day)
		win_writes = { day, n: 0 };
	if (write && win_writes.n < cfg.windows_per_day) {
		win_writes.n++;
		save_windows();
	}
}

// A critical event at t opens a window, or stretches the pending one.
function open_window(t) {
	let p = st.win_pending;
	if (!st.ever_o5)
		return;		// an unused fiber port writes nothing
	if (p && t >= p.t0 && t <= p.t1) {
		p.t1 = (t + WIN_HALF < p.t0 + WIN_MAX_SPAN) ? t + WIN_HALF : p.t0 + WIN_MAX_SPAN;
		return;
	}
	if (p)
		capture_window(t, true);
	st.win_pending = { t0: t - WIN_HALF, t1: t + WIN_HALF };
}

function add_event(t, ev) {
	let e = { t, type: ev.type, severity: ev.severity, msg: ev.msg };
	for (let k in [ 'n', 'flags' ])	/* the browser words the event with these */
		if (ev[k] != null)
			e[k] = ev[k];
	ring_push(st.events, e, cfg.events_max);
	if (sink)
		sink(e);
	if (e.severity == 'crit')
		open_window(t);
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
	st.gem = parse_gem_mib(raw.gem_mib, raw.t, st.gem);
	// A counter that went backwards was reset (driver reload): count from 0.
	if (sample.bip_total != null && prev?.bip_total != null)
		sample.bip = sample.bip_total >= prev.bip_total
			? sample.bip_total - prev.bip_total : sample.bip_total;
	// Unknown state (no answer from omci.agent) keeps the link timeline;
	// transitions compare against the last sample the agent answered.
	if (sample.state == 5) {
		st.ever_o5 = true;
		if (st.o5_up == null && st.o5_t == null) {
			st.o5_up = raw.up;
			st.o5_t = sample.t;
		}
	} else if (sample.state != null) {
		st.o5_up = st.o5_t = null;
	}

	if (st.ever_o5)
		for (let ev in detect(st.known, sample, sample.bip, th))
			add_event(sample.t, ev);
	if (sample.state != null)
		st.known = sample;
	ring_push(st.m1, sample, M1_CAP);
	bucket(sample);
	// Due, or the clock stepped back before it: capture what there is.
	let wp = st.win_pending;
	if (wp && (sample.t >= wp.t1 || sample.t < wp.t0))
		capture_window(sample.t, true);
	st.last = sample;
	st.info = s;
	if (raw.status?.serial)
		st.serial = raw.status.serial;
}

// t: wall clock when the line was read, used if it has no kernel timestamp.
function on_klog(line, t) {
	let up = kernel_up(line);
	if (index(line, 'Deactivate_ONU-ID') >= 0) {
		st.deact++;
		st.deact_up = up;
		st.deact_t = t;
		add_event(wall(up, t), { type: 'deactivate', severity: 'crit',
			msg: 'OLT sent Deactivate_ONU-ID; GPON restarting' });
	} else if (index(line, 'GPON netdev link ready') >= 0) {
		st.o5_up = up;
		st.o5_t = t;
		st.ever_o5 = true;
	}
}

// From the newest sample back: the overview asks for the summary every 5 s
// and the ring holds 24 h, so walking all of it cost ~30 ms each time.
function bip_last_hour(now) {
	let sum = 0;
	for (let i = length(st.m1) - 1; i >= 0 && st.m1[i].t > now - 3600; i--)
		sum += st.m1[i].bip;
	return sum;
}

function assess(now) {
	return evaluate(st.last, {
		now, bip_last_min: st.last?.bip, bip_last_hour: bip_last_hour(now),
		last_deact_t: wall(st.deact_up, st.deact_t), ever_o5: st.ever_o5
	}, th);
}

function summary(now) {
	let l = st.last, i = st.info || {}, a = assess(now);
	return {
		now, stale: !l || now - l.t > 3 * cfg.interval,
		health: a.health, alerts: a.alerts, ever_o5: st.ever_o5,
		state: l?.state, o5_since: wall(st.o5_up, st.o5_t),
		olt: { vendor: i.olt_vendor, version: i.olt_version, profile: i.olt_profile },
		onu_id: i.onu_id, fec: { ds: i.fec_ds, us: i.fec_us },
		optics: { rx_nw: l?.rx_nw, tx_nw: l?.tx_nw, temp_mc: l?.temp_mc,
			bias_ua: l?.bias_ua, volt_uv: l?.volt_uv, alarms: l?.alarms },
		counters: { bip_total: l?.bip_total, bip_session: l?.bip_session,
			gem_delin: l?.gem_delin, sfifo_resync: l?.sfifo_resync,
			sfifo_glitch: l?.sfifo_glitch, deactivate: st.deact },
		gem: st.gem ? { enabled: st.gem.enabled, ready: st.gem.ready,
			type: st.gem.type, ports: st.gem.ports } : null,
		serial_hint: st.serial ? substr(st.serial, 0, 4) + '…' : null
	};
}

function m1_point(s) {
	return {
		t: s.t, n: 1,
		rx_min: s.rx_nw, rx_avg: s.rx_nw, rx_max: s.rx_nw,
		temp_min: s.temp_mc, temp_avg: s.temp_mc, temp_max: s.temp_mc,
		bip: s.bip
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
	return { window: '24h', step: 60, points: map(st.m1, m1_point) };
}

function covered(t) {
	for (let w in st.windows)
		if (t >= w.t0 && t <= w.t1)
			return true;
	return false;
}

// What happened around t: a saved window that holds it, else the 1-minute
// ring, else the 5-minute ring; ranges include both ends.
function around(t) {
	let r0 = t - WIN_HALF, r1 = t + WIN_HALF, res = null, pts;
	for (let w in st.windows)
		if (t >= w.t0 && t <= w.t1)
			res = { t0: w.t0, t1: w.t1, step: 60, source: 'saved', points: map(w.points, m1_point) };
	if (!res) {
		pts = filter(st.m1, (s) => s.t >= r0 && s.t <= r1);
		if (length(pts))
			res = { t0: r0, t1: r1, step: 60, source: '1min', points: map(pts, m1_point) };
	}
	if (!res) {
		pts = filter(history('7d').points, (q) => q.t >= r0 && q.t <= r1);
		res = length(pts) ? { t0: r0, t1: r1, step: RINGS.m5[0], source: '5min', points: pts }
			: { t0: r0, t1: r1, step: 60, source: 'none', points: [] };
	}
	res.events = filter(st.events, (e) => e.t >= res.t0 && e.t <= res.t1);
	return res;
}

function events_list(limit) {
	let out = [], n = length(st.events);
	if (!(limit > 0) || limit > n)
		limit = n;
	for (let i = n - 1; i >= n - limit; i--)
		push(out, covered(st.events[i].t) ? { ...st.events[i], win: true } : st.events[i]);
	return out;
}

function save_snapshot(path) {
	return write_gz(path, {
		version: 1, saved: time(), boot_id: boot_id_of(),
		m1: st.m1, m5: st.m5, m30: st.m30, p5: st.p5, p30: st.p30
	});
}

// A missing, truncated or otherwise unreadable snapshot starts empty rings.
function load_snapshot(path) {
	if (!fs.stat(path))
		return false;
	let data = read_gz(path);
	if (type(data) != 'object' || type(data.m1) != 'array') {
		warn(`xr500v-gpon: ignoring unreadable snapshot ${path}\n`);
		return false;
	}
	for (let k in [ 'm1', 'm5', 'm30', 'p5', 'p30' ])
		if (type(data[k]) == 'array')
			st[k] = data[k];
	st.snap_boot = data.boot_id;
	return true;
}

let writes = { day: null, n: 0, crit: 0 };

// Critical events have their own daily budget, so a flood of warnings cannot
// keep a link drop off the flash.
function append_event(e) {
	let day = int(e.t / 86400), key = (e.severity == 'crit') ? 'crit' : 'n';
	if (writes.day != day) {
		writes.day = day;
		writes.n = writes.crit = 0;
	}
	if (writes[key] >= cfg.event_writes_per_day)
		return;	// stays in RAM; the next persist() rewrites the file
	writes[key]++;
	let f = fs.open(events_path, 'a');
	if (f) {
		f.write(sprintf('%J\n', e));
		f.close();
	}
}

// Also recounts today's writes, so a restart does not reset the budget.
function load_events(now) {
	let text = fs.readfile(events_path), today = int(now / 86400);
	writes = { day: today, n: 0, crit: 0 };
	for (let line in split(text || '', '\n')) {
		let e = null;
		if (!length(line))
			continue;
		try { e = json(line); } catch (err) { continue; }
		ring_push(st.events, e, cfg.events_max);
		if (int(e?.t / 86400) == today)
			writes[(e.severity == 'crit') ? 'crit' : 'n']++;
	}
}

// A missing or unreadable windows file starts with none.
function load_windows(now) {
	let d = read_gz(windows_path), day = int(now / 86400);
	if (type(d?.windows) == 'array')
		st.windows = filter(d.windows, (w) => type(w) == 'object' && w.t0 <= w.t1 && type(w.points) == 'array');
	win_writes = { day, n: length(filter(st.windows, (w) => int(w.saved / 86400) == day)) };
}

// After a restart: the last answered sample, if recent, so a warning that
// already fired does not fire again and a link that was up counts as O5.
function restore_known(now) {
	for (let i = length(st.m1) - 1; i >= 0; i--) {
		let s = st.m1[i];
		if (s.state == null)
			continue;
		if (now - s.t > 3 * cfg.interval)
			return;
		if (st.snap_boot && st.snap_boot == boot_id_of()) {
			st.known = s;
			if (s.state == 5)
				st.ever_o5 = true;
		} else {
			/* Another boot (after a reboot sysfixtime makes the snapshot look
			 * recent): the link went down with the router. Keep the readings,
			 * so warnings do not fire again, not the link state. */
			st.known = { ...s, state: null };
		}
		return;
	}
}

// closing: the service stops, so a pending window is closed with what it
// has; the 6-hourly save leaves it pending (its capture comes later).
function persist(closing) {
	let ok = snapshot_path ? save_snapshot(snapshot_path) : true;
	if (st.win_pending && closing)
		capture_window(time(), false);
	if (st.win_dirty || st.win_pending)
		save_windows();
	if (!events_path)
		return ok;
	let tmp = events_path + '.tmp';
	/* a full overlay must not truncate the file that is there */
	if (fs.writefile(tmp, join('', map(st.events, (e) => sprintf('%J\n', e)))) != null)
		fs.rename(tmp, events_path);
	return ok;
}

function load_config() {
	let uci = cursor();
	let m = uci.get_all('xr500v-gpon', 'main') || {};
	for (let k in [ 'interval', 'persist_hours', 'events_max', 'event_writes_per_day', 'windows_per_day' ]) {
		let n = num(m[k]);
		if (n > 0)
			cfg[k] = n;
	}
	th = thresholds(uci.get_all('xr500v-gpon', 'thresholds'));
}

// Deactivates and the last O5 since boot, from one dmesg read at start.
function startup_scan() {
	let p = fs.popen('dmesg');
	if (!p)
		return;
	for (let line = p.read('line'); length(line); line = p.read('line')) {
		if (index(line, 'Deactivate_ONU-ID') >= 0) {
			st.deact++;
			st.deact_up = kernel_up(line);
			st.deact_t = null;
		} else if (index(line, 'GPON netdev link ready') >= 0) {
			st.o5_up = kernel_up(line);
			st.o5_t = null;
			st.ever_o5 = true;
		}
	}
	p.close();
}

let klog_off = 0;

// Lines already in the file at start were counted by startup_scan().
function klog_init() {
	klog_off = fs.stat(klog_path)?.size || 0;
}

// New complete lines appended by the logread instance since the last tick.
function read_klog(t) {
	let size = fs.stat(klog_path)?.size;
	if (size == null)
		return;
	if (size < klog_off)
		klog_off = 0;	// truncated
	let f = fs.open(klog_path, 'r');
	if (!f)
		return;
	f.seek(klog_off);
	let text = f.read('all') || '';
	f.close();
	let nl = rindex(text, '\n');
	if (nl < 0)
		return;
	klog_off += nl + 1;
	for (let line in split(substr(text, 0, nl), '\n'))
		if (length(line))
			on_klog(line, t);
	if (klog_off > KLOG_MAX) {
		fs.writefile(klog_path, '');
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
		t: time(), up: uptime(), status,
		bip: fs.readfile(XMAC + '/gpon_bip'),
		gem_delin: fs.readfile(XMAC + '/gpon_gem_delin'),
		gem_mib: fs.readfile(XMAC + '/gpon_gem_mib'),
		sfifo_resync: fs.readfile(DBG + '/gpon_sfifo_resync_count'),
		sfifo_glitch: fs.readfile(DBG + '/gpon_sfifo_glitch_count')
	};
}

function replay(path) {
	let fx = json(fs.readfile(path));
	if (fx.boot != null)
		boot_of = () => fx.boot;
	let bid = (fx.boot_id != null) ? fx.boot_id : 'replay';
	boot_id_of = () => bid;
	let first = length(fx.samples) ? fx.samples[0].t : time();
	if (fx.windows_file)
		windows_path = fx.windows_file;
	if (fx.windows_per_day > 0)
		cfg.windows_per_day = fx.windows_per_day;
	if (fx.events_file) {
		events_path = fx.events_file;
		if (fx.writes_per_day > 0)
			cfg.event_writes_per_day = fx.writes_per_day;
		load_events(first);
		sink = append_event;
	}
	if (fx.klog_file) {
		klog_path = fx.klog_file;
		klog_init();
	}
	if (fx.snapshot && load_snapshot(fx.snapshot))
		restore_known(first);
	let klog = fx.klog || [], k = 0, extra = fx.klog_append || [], x = 0, saves = fx.persist_at || [];
	for (let raw in fx.samples || []) {
		for (; k < length(klog) && klog[k].t <= raw.t; k++)
			on_klog(klog[k].line, klog[k].t);
		if (fx.klog_file) {
			for (; x < length(extra) && extra[x].t <= raw.t; x++) {
				let f = fs.open(klog_path, 'a');
				f.write(extra[x].line + '\n');
				f.close();
			}
			read_klog(raw.t);
		}
		ingest(raw);
		if (raw.t in saves)
			persist();	// the 6-hourly save, at this sample
	}
	for (; k < length(klog); k++)
		on_klog(klog[k].line, klog[k].t);
	if (fx.save)
		save_snapshot(fx.save);
	let now = st.last ? st.last.t : time();
	print(sprintf('%J\n', {
		events: st.events, m1_len: length(st.m1),
		m5_len: length(history('7d').points), health: assess(now).health,
		counters: summary(now).counters, gem: summary(now).gem,
		skipped: st.skipped, last: st.last,
		o5_since: wall(st.o5_up, st.o5_t), ever_o5: st.ever_o5,
		windows: map(st.windows, (w) => ({ t0: w.t0, t1: w.t1, n: length(w.points) })),
		pending: st.win_pending, listed: events_list(0),
		around: (fx.around != null) ? around(fx.around) : null
	}));
}

function daemon() {
	load_config();
	fs.mkdir(STATE_DIR);
	snapshot_path = SNAPSHOT;
	events_path = EVENTS_FILE;
	if (load_snapshot(SNAPSHOT))
		restore_known(time());
	load_events(time());
	windows_path = WINDOWS_FILE;
	load_windows(time());
	sink = append_event;
	klog_init();
	startup_scan();
	uloop.init();
	let bus = ubus.connect();
	// ucode rejects arguments missing from a method's policy, and rpcd adds
	// ubus_rpc_session to every call coming from LuCI.
	let sid = { ubus_rpc_session: '' };
	bus.publish('xr500v.gpon', {
		summary: { args: sid, call: () => summary(time()) },
		history: { args: { ...sid, window: '' }, call: (req) => history(req.args.window) },
		events: { args: { ...sid, limit: 0 }, call: (req) => ({ events: events_list(req.args.limit) }) },
		around: { args: { ...sid, t: 0 }, call: (req) => around(req.args.t) },
		persist: { args: sid, call: () => ({ ok: persist(true) }) },	/* stop_service */
		eye_scan: { args: sid, call: () => eye_scan() },	/* lab: needs xr500v_eye.ko */
		reload: { args: sid, call: () => { load_config(); return { ok: true }; } }
	});
	let tick = () => {
		read_klog(time());
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
