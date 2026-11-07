// GPON fiber health: the pure logic shared by the recorder daemon and the
// tests. No I/O here. Optical powers are in nW (omci.agent telemetry),
// temperatures in m°C. The router's ucode has no math module, so log10 and
// 10^x are computed below.
'use strict';

const LN10 = 2.302585092994046;

// Alert ids and their severity.
const SEVERITY = {
	not_o5: 'crit',
	rx_crit: 'crit',
	rx_warn: 'warn',
	rx_high: 'warn',
	temp_warn: 'warn',
	bip_hour: 'warn',
	deactivate_recent: 'crit'
};

// Event type -> [severity, message].
const EVENTS = {
	o5_up: [ 'info', 'Link operational (O5)' ],
	o5_down: [ 'crit', 'Link lost: left O5' ],
	bip_burst: [ 'warn', 'BIP error burst' ],
	rx_crit: [ 'crit', 'Received power below the critical threshold' ],
	rx_warn: [ 'warn', 'Received power below the warning threshold' ],
	rx_high: [ 'warn', 'Received power above the overload warning' ],
	temp_warn: [ 'warn', 'Laser temperature above the warning threshold' ],
	resync: [ 'warn', 'Grant FIFO resync' ],
	bosa_alarm: [ 'crit', 'Optical module alarm' ]
};

const DEFAULTS = {
	rx_warn_dbm: -25,
	rx_crit_dbm: -27,
	rx_high_dbm: -9,
	temp_warn_c: 70,
	bip_burst_per_min: 10
};

// omci.agent status field for each optics value of a sample.
const OPTICS = {
	rx_nw: 'bosa_rx_power_nw',
	tx_nw: 'bosa_tx_power_nw',
	temp_mc: 'bosa_temperature_mc',
	bias_ua: 'bosa_bias_ua',
	volt_uv: 'bosa_voltage_uv',
	alarms: 'bosa_alarms'
};

const DEACT_RECENT_S = 600;

function absv(x) {
	return x < 0 ? -x : x;
}

function is_num(x) {
	return type(x) in [ 'int', 'double' ];
}

// Natural log for x > 0: scale into [1, 10), then 2 atanh((x-1)/(x+1)).
function ln(x) {
	let k = 0;
	while (x >= 10) { x /= 10; k++; }
	while (x < 1) { x *= 10; k--; }
	let y = (x - 1) / (x + 1), y2 = y * y, term = y, sum = 0;
	for (let n = 1; n < 400; n += 2) {
		sum += term / n;
		term *= y2;
		if (term < 1e-16)
			break;
	}
	return 2 * sum + k * LN10;
}

// e^x: fold whole multiples of ln 10 out, Taylor series for the rest.
function exp(x) {
	let k = 0;
	while (x > 1) { x -= LN10; k++; }
	while (x < -1) { x += LN10; k--; }
	let sum = 1, term = 1;
	for (let i = 1; i < 60; i++) {
		term *= x / i;
		sum += term;
		if (absv(term) < 1e-17)
			break;
	}
	for (; k > 0; k--) sum *= 10;
	for (; k < 0; k++) sum /= 10;
	return sum;
}

export function nw_to_dbm(nw) {
	if (!is_num(nw) || nw <= 0)
		return null;
	return 10 * ln(nw / 1e6) / LN10;
};

export function dbm_to_nw(dbm) {
	return int(1e6 * exp(dbm / 10.0 * LN10) + 0.5);	// int / int truncates in ucode
};

// "active=1 session=1 total=0 session_total=0 last=0 pending_rei=0"
export function parse_bip(text) {
	if (type(text) != 'string')
		return null;
	let r = {};
	for (let kv in split(trim(text), /[ \t\n]+/)) {
		let m = match(kv, /^([a-z_]+)=(-?[0-9]+)$/);
		if (m)
			r[m[1]] = int(m[2]);
	}
	return ('total' in r && 'session' in r) ? r : null;
};

// gpon_counters: one "name=value" per line; null without the file.
export function parse_counters(text) {
	if (type(text) != 'string')
		return null;
	let r = {};
	for (let kv in split(trim(text), /[ \t\n]+/)) {
		let m = match(kv, /^([a-z0-9_]+)=([0-9]+)$/);
		if (m)
			r[m[1]] = int(m[2]);
	}
	return length(r) ? r : null;
};

// gpon_pon_id: "count=N class=C+ source=olt id=O011906 id_hex=... tol=356".
// Null until the OLT sends one, which it does only if the operator set it up.
export function parse_pon_id(text) {
	if (type(text) != 'string')
		return null;
	let r = {};
	for (let kv in split(trim(text), /[ \t\n]+/)) {
		let m = match(kv, /^([a-z_]+)=(.+)$/);
		if (m)
			r[m[1]] = m[2];
	}
	if (!(int(r.count) > 0) || !r.id_hex)
		return null;
	let tol = int(r.tol);
	return { id: r.id, id_hex: r.id_hex, class: r.class,
		tol_dbm: (tol < 65535) ? tol / 10.0 - 30 : null };
};

// The code of a failed SIP registration (0 if it has none), or null. With the
// WAN down every registration fails, and the WAN is what is wrong then.
export function sip_failed(voip) {
	if (!voip?.wan_up)
		return null;
	for (let line in voip.lines || [])
		for (let reg in line.registrations || [])
			if (reg.state == 'failed')
				return reg.code || 0;
	return null;
};

export function normalize_status(st) {
	let s = {
		state: null, onu_id: null, olt_vendor: null, olt_version: null,
		olt_profile: null, fec_ds: null, fec_us: null
	};
	for (let k in OPTICS)
		s[k] = null;
	if (type(st) != 'object')
		return s;
	s.state = st.state;
	s.onu_id = st.onu_id;
	s.olt_vendor = st.olt?.vendor_id;
	s.olt_version = st.olt?.version;
	s.olt_profile = st.olt_profile_effective_name;
	s.fec_ds = st.fec_downstream;
	s.fec_us = st.fec_upstream;
	if (st.telemetry_valid != 0)
		for (let k, src in OPTICS)
			s[k] = st[src];
	return s;
};

export function thresholds(sec) {
	let th = {};
	for (let k, def in DEFAULTS) {
		let raw = (type(sec) == 'object') ? sec[k] : null;
		let n = (raw == null || raw === '') ? def : +raw;
		th[k] = (n != n) ? def : n;	// NaN check
	}
	// 0 would turn every minute with one BIP error into a "burst".
	if (th.bip_burst_per_min < 1)
		th.bip_burst_per_min = DEFAULTS.bip_burst_per_min;
	return th;
};

// The received-power alert of one sample, or null.
function rx_alert(rx, th) {
	if (rx == null)
		return null;
	let dbm = nw_to_dbm(rx);
	if (dbm == null || dbm <= th.rx_crit_dbm)
		return 'rx_crit';
	if (dbm <= th.rx_warn_dbm)
		return 'rx_warn';
	if (dbm >= th.rx_high_dbm)
		return 'rx_high';
	return null;
}

function hot(sample, th) {
	return sample?.temp_mc != null && sample.temp_mc >= th.temp_warn_c * 1000;
}

export function evaluate(sample, ctx, th) {
	ctx = ctx || {};
	let ids = [];
	// A null state means omci.agent did not answer: the link and optics
	// alerts cannot be judged, the counter-based ones still can.
	let known = sample?.state != null;
	// Never in O5 since boot and no light: the fiber port is not in use
	// (WAN over Ethernet, say), not a fault. Only an explicit false: callers
	// that do not track it keep the plain O5 judgement.
	if (known && ctx.ever_o5 === false && sample.state != 5 && !(sample.rx_nw > 0))
		return { alerts: [], health: 'nofiber' };
	if (known) {
		if (sample.state != 5)
			push(ids, 'not_o5');
		let rx = rx_alert(sample.rx_nw, th);
		if (rx)
			push(ids, rx);
		if (hot(sample, th))
			push(ids, 'temp_warn');
	}
	if (ctx.bip_last_hour > 0)
		push(ids, 'bip_hour');
	if (ctx.last_deact_t != null && ctx.now != null &&
	    ctx.now - ctx.last_deact_t < DEACT_RECENT_S)
		push(ids, 'deactivate_recent');

	let alerts = map(ids, (id) => ({ id, severity: SEVERITY[id] }));
	let health = known ? 'ok' : 'unknown';
	for (let a in alerts) {
		if (a.severity == 'crit')
			return { alerts, health: 'crit' };
		health = 'warn';
	}
	return { alerts, health };
};

// extra: fields the browser needs to word the event itself (n, flags).
function event(type, msg, extra) {
	let e = { type, severity: EVENTS[type][0], msg: msg || EVENTS[type][1] };
	for (let k, v in extra)
		e[k] = v;
	return e;
}

// Events between two consecutive samples. Alerts only fire on entering.
export function detect(prev, cur, bip_delta, th) {
	let out = [];
	// A null state means omci.agent did not answer: not a transition.
	if (prev?.state != null && cur?.state != null) {
		if (cur.state == 5 && prev.state != 5)
			push(out, event('o5_up'));
		else if (prev.state == 5 && cur.state != 5)
			push(out, event('o5_down'));
	}
	if (bip_delta != null && bip_delta > th.bip_burst_per_min)
		push(out, event('bip_burst', `${bip_delta} BIP errors in one minute`, { n: bip_delta }));
	let was = rx_alert(prev?.rx_nw, th), is = rx_alert(cur?.rx_nw, th);
	if (is && is != was)
		push(out, event(is));
	if (hot(cur, th) && !hot(prev, th))
		push(out, event('temp_warn'));
	if (prev?.sfifo_resync != null && cur?.sfifo_resync != null &&
	    cur.sfifo_resync > prev.sfifo_resync)
		push(out, event('resync'));
	if (cur?.alarms > 0 && !(prev?.alarms > 0))
		push(out, event('bosa_alarm', `Optical module alarm (flags ${cur.alarms})`, { flags: cur.alarms }));
	return out;
};

function add(b, key, v) {
	if (v == null)
		return;
	if (b[key + '_c'] == 0 || v < b[key + '_min'])
		b[key + '_min'] = v;
	if (b[key + '_c'] == 0 || v > b[key + '_max'])
		b[key + '_max'] = v;
	b[key + '_s'] += v;
	b[key + '_c']++;
}

// Buckets of `step` seconds with min/avg/max of RX and temperature and
// the BIP sum. Samples must be in time order.
export function aggregate(samples, step) {
	let out = [], cur = null;
	for (let s in samples) {
		let t = s.t - s.t % step;
		if (!cur || cur.t != t) {
			cur = {
				t, n: 0, bip: 0,
				rx_min: null, rx_max: null, rx_s: 0, rx_c: 0,
				temp_min: null, temp_max: null, temp_s: 0, temp_c: 0
			};
			push(out, cur);
		}
		cur.n++;
		add(cur, 'rx', s.rx_nw);
		add(cur, 'temp', s.temp_mc);
		if (s.bip != null)
			cur.bip += s.bip;
	}
	return map(out, (b) => ({
		t: b.t, n: b.n,
		rx_min: b.rx_min, rx_avg: b.rx_c ? b.rx_s * 1.0 / b.rx_c : null, rx_max: b.rx_max,
		temp_min: b.temp_min, temp_avg: b.temp_c ? b.temp_s * 1.0 / b.temp_c : null, temp_max: b.temp_max,
		bip: b.bip
	}));
};

export function ring_push(arr, item, cap) {
	push(arr, item);
	while (length(arr) > cap)
		shift(arr);
	return arr;
};
