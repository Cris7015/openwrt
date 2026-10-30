'use strict';
'require view';
'require poll';
'require rpc';
'require dom';
'require ui';
'require uci';
'require form';
'require xr500v-gpon.graph as graph';
'require xr500v-gpon.analysis as analysis';
'require xr500v-gpon.heatmap as heatmap';

var callSummary = rpc.declare({
	object: 'xr500v.gpon',
	method: 'summary'
});

var callHistory = rpc.declare({
	object: 'xr500v.gpon',
	method: 'history',
	params: [ 'window' ]
});

var callEvents = rpc.declare({
	object: 'xr500v.gpon',
	method: 'events',
	params: [ 'limit' ],
	expect: { events: [] }
});

var callAround = rpc.declare({
	object: 'xr500v.gpon',
	method: 'around',
	params: [ 't' ]
});

var callEyeScan = rpc.declare({
	object: 'xr500v.gpon',
	method: 'eye_scan'
});

var EYE_ERRORS = {
	no_module: _('The lab module xr500v_eye.ko is not installed.'),
	not_aligned: _('The eye sampler did not realign. Try again.'),
	realign_failed: _('The eye sampler did not accept the command.'),
	scan_failed: _('The scan did not complete.')
};

var EVENTS_SHOWN = 100;	/* rows in the Events tab */
var EVENTS_KEPT = 500;	/* the recorder's whole list, for the graphs */

var TIP_STYLE = 'position:fixed;z-index:1000;display:none;pointer-events:none;max-width:320px;' +
	'padding:6px 8px;border-radius:4px;background:rgba(32,32,32,.92);color:#fff;' +
	'font-size:12px;line-height:1.45;box-shadow:0 2px 6px rgba(0,0,0,.4)';

var WINDOWS = [
	[ '24h', _('24 hours'), 86400 ],
	[ '7d', _('7 days'), 604800 ],
	[ '30d', _('30 days'), 2592000 ]
];

var SOURCES = {
	saved: _('saved 1-minute detail'),
	'1min': _('1-minute history'),
	'5min': _('5-minute averages'),
	none: _('no data')
};

var ALERTS = {
	not_o5: _('Link not operational (not in O5)'),
	rx_crit: _('Received power below the critical threshold'),
	rx_warn: _('Received power below the warning threshold'),
	rx_high: _('Received power above the overload warning'),
	temp_warn: _('Laser temperature above the warning threshold'),
	bip_hour: _('BIP errors in the last hour'),
	deactivate_recent: _('Deactivate from the OLT in the last 10 minutes')
};

function dbmText(nw) {
	if (!(nw > 0))
		return null;

	var v = 10 * Math.log10(nw / 1e6);

	return (v < 0 ? '−' : '+') + '%.1f dBm'.format(Math.abs(v));
}

function scaled(v, div, format) {
	return (v != null) ? format.format(v / div) : null;
}

var GEM_MIB_ENABLE = '/sys/bus/platform/drivers/airoha-xpon/1fb60000.xmac/gpon_gem_mib_enable';

function volume(bytes, frames) {
	if (bytes == null)
		return '—';

	return '%1024.2mB'.format(bytes) + ((frames != null) ? ' · ' + _('%d frames').format(frames) : '');
}

function rate(bps) {
	return (bps != null) ? '%.2mbit/s'.format(bps) : '—';
}

function datetime(t) {
	var d = new Date(t * 1000);

	return '%04d-%02d-%02d %02d:%02d:%02d'.format(d.getFullYear(), d.getMonth() + 1,
		d.getDate(), d.getHours(), d.getMinutes(), d.getSeconds());
}

function tag(severity) {
	if (severity == 'crit')
		return E('span', { 'class': 'label', 'style': 'background-color:#f44336;color:#fff' }, [ _('critical') ]);

	if (severity == 'warn')
		return E('span', { 'class': 'label warning' }, [ _('warning') ]);

	return E('span', { 'class': 'label' }, [ _('info') ]);
}

function table(head, rows, empty) {
	var t = E('table', { 'class': 'table' }, [
		E('tr', { 'class': 'tr table-titles' }, head.map(function(h) {
			return E('th', { 'class': 'th' }, [ h ]);
		}))
	]);

	if (!rows.length)
		rows = [ [ E('em', {}, [ empty ]) ] ];

	rows.forEach(function(r) {
		t.appendChild(E('tr', { 'class': 'tr' }, r.map(function(c, i) {
			return E('td', { 'class': 'td left', 'colspan': (r.length == 1) ? head.length : null }, [ c ]);
		})));
	});

	return t;
}

/* "−20.8 dBm", or "−20.8 dBm (−21.0 … −20.7)" for an averaged point */
function triple(v, unit, spread) {
	var f = function(x) { return (x < 0 ? '−' : '') + Math.abs(x).toFixed(1); };

	if (!v)
		return '—';
	return f(v.avg) + ' ' + unit + ((spread && v.min != null) ? ' (%s … %s)'.format(f(v.min), f(v.max)) : '');
}

function threshold(name, def) {
	var v = parseFloat(uci.get('xr500v-gpon', 'thresholds', name));

	return isNaN(v) ? def : v;
}

return view.extend({
	window: '24h',
	seq: 0,		/* bumped by every window change: older replies are dropped */
	pseq: 0,	/* the same for the Patterns tab */

	load: function() {
		return Promise.all([
			L.resolveDefault(callSummary(), {}),
			L.resolveDefault(callHistory(this.window), {}),
			L.resolveDefault(callEvents(EVENTS_KEPT), []),
			uci.load('xr500v-gpon')
		]);
	},

	/* The cursor's box: values and events of the point under the cursor */
	showTip: function(t, cx, cy) {
		/* events within a marker's width (8 px) of the cursor count as under
		 * it: a marker covers its event's whole 8 px strip */
		var slack = (this.charts && this.charts[0]) ? 8 * this.charts[0].chart.secPerPx : 0,
		    tip = (t != null) ? analysis.tooltip(this.points, this.tipEvents, t, this.step, slack) : null,
		    spread = this.step > 60, rows, w, h;

		this.lastTip = (t != null) ? { t: t, cx: cx, cy: cy } : null;
		if (!tip) {
			this.tip.style.display = 'none';
			return;
		}

		rows = [
			E('strong', {}, [ analysis.localTime(tip.t, true) ]),
			E('div', {}, [ _('Received power') + ': ' + triple(tip.rx, 'dBm', spread) ]),
			E('div', {}, [ _('Temperature') + ': ' + triple(tip.temp, '°C', spread) ]),
			E('div', {}, [ _('BIP errors') + ': ' + ((tip.bip != null) ? tip.bip : '—') ])
		];
		tip.events.forEach(function(e) {
			rows.push(E('div', { 'style': 'margin-top:3px' }, [
				E('span', { 'style': 'color:' + ((e.severity == 'crit') ? '#ff8a80' : (e.severity == 'warn') ? '#ffd54f' : '#b0bec5') },
					[ '● ' ]),
				analysis.eventText(e)
			]));
		});
		if (tip.more)
			rows.push(E('div', {}, [ _('+%d more').format(tip.more) ]));

		this.tipOwner = 'graph';
		this.placeTip(rows, cx, cy);
	},

	/* Shows the box next to the pointer, inside the window */
	placeTip: function(rows, cx, cy) {
		var w, h;

		dom.content(this.tip, rows);
		this.tip.style.display = 'block';
		w = this.tip.offsetWidth;
		h = this.tip.offsetHeight;
		this.tip.style.left = ((cx + 14 + w > window.innerWidth - 8) ? Math.max(8, cx - 14 - w) : cx + 14) + 'px';
		this.tip.style.top = ((cy + 14 + h > window.innerHeight - 8) ? Math.max(8, cy - 14 - h) : cy + 14) + 'px';
	},

	/* The Patterns map's box: one hour of one day */
	showCell: function(info, cx, cy) {
		var c, rows;

		if (!info) {
			this.tip.style.display = 'none';
			return;
		}
		c = info.cell;
		rows = [ E('strong', {}, [ '%s · %02d:00–%02d:00'.format(info.label, info.hour, (info.hour + 1) % 24) ]) ];
		if (!c.seen && !c.crit && !c.warn && !c.info) {
			rows.push(E('div', {}, [ _('No data') ]));
		} else {
			rows.push(E('div', {}, [ _('BIP errors') + ': ' + c.bip ]));
			if (c.crit)
				rows.push(E('div', {}, [ _('Critical events: %d').format(c.crit) ]));
			if (c.warn)
				rows.push(E('div', {}, [ _('Warnings: %d').format(c.warn) ]));
			if (c.info)
				rows.push(E('div', {}, [ _('Other events: %d').format(c.info) ]));
			if (!c.crit && !c.warn && !c.info)
				rows.push(E('div', {}, [ _('No events') ]));
		}
		this.tipOwner = 'cell';
		this.placeTip(rows, cx, cy);
	},

	/* Last 30 days, fetched when the tab is opened: the 30-day history is
	 * the largest reply and most visits never look at it. */
	loadPatterns: function() {
		var seq = ++this.pseq;

		dom.content(this.patterns, E('em', {}, [ _('Loading…') ]));
		return Promise.all([
			L.resolveDefault(callSummary(), {}),
			L.resolveDefault(callHistory('30d'), {}),
			L.resolveDefault(callEvents(EVENTS_KEPT), [])
		]).then(L.bind(function(res) {
			var now = res[0].now || Math.floor(Date.now() / 1000),
			    map = analysis.heatmap(res[1].points || [], res[2], now, 30),
			    any = map.some(function(d) {
				return d.hours.some(function(c) { return c.seen || c.crit || c.warn || c.info; });
			    });

			if (seq != this.pseq)
				return;
			dom.content(this.patterns, any ? heatmap.render(map, now, L.bind(this.showCell, this))
				: E('em', {}, [ _('No data yet') ]));
		}, this));
	},

	/* zoom: an `around` reply, drawn instead of the chosen window */
	renderGraphs: function(history, summary, zoom) {
		/* no fiber: the note goes above whatever history there still is (a
		 * reboot during an outage keeps the restored rings) */
		var note = (!zoom && summary.health == 'nofiber') ? E('div', { 'class': 'cbi-section-descr' }, [
			E('strong', {}, [ _('No fiber') ]), ' — ',
			_('the GPON port has not reached O5 since the router started and receives no light: no alerts or events until it does.')
		]) : '';

		if (note && !(history.points || []).length) {
			this.points = [];
			this.tipEvents = [];
			return note;
		}

		var win = WINDOWS.filter(function(w) { return w[0] == history.window; })[0] || WINDOWS[0],
		    step = zoom ? zoom.step : (history.step || 60),
		    events = zoom ? (zoom.events || []) : (this.evData || []),
		    common = zoom ? { span: zoom.t1 - zoom.t0, step: step, now: zoom.t1, points: zoom.points }
			: { span: win[2], step: step, now: summary.now, points: history.points },
		    per = (step >= 1800) ? _('per 30 minutes') : (step >= 300) ? _('per 5 minutes') : _('per minute'),
		    point = (step >= 1800) ? _('30-minute averages') : (step >= 300) ? _('5-minute averages') : _('one sample per minute'),
		    marker = L.bind(this.openAround, this);

		var draw = function(opts) {
			return graph.render(common.points, Object.assign({ span: common.span, step: common.step,
				now: common.now, events: events, onMarker: marker }, opts));
		};

		var charts = [
			draw({ field: 'rx', unit: 'dBm', label: _('Received power'), color: '#0069d6', height: 220, lines: [
				{ value: threshold('rx_warn_dbm', -25), label: _('warning'), color: '#ff9800' },
				{ value: threshold('rx_crit_dbm', -27), label: _('sensitivity limit'), color: '#f44336' }
			] }),
			draw({ field: 'temp', unit: '°C', label: _('Temperature'), color: '#ff6600', height: 160 }),
			draw({ field: 'bip', unit: '', label: _('BIP errors'), color: '#ff0000', height: 120 })
		];

		this.points = common.points || [];
		this.step = step;
		this.tipEvents = events;
		this.charts = charts;
		if (this.tipOwner != 'cell')	/* a Patterns box is not ours to close */
			this.tip.style.display = 'none';
		graph.cursor(charts, L.bind(this.showTip, this));

		return E('div', {}, [
			note,
			zoom ? E('div', { 'class': 'cbi-section-descr' }, [
				E('button', { 'class': 'cbi-button', 'click': ui.createHandlerFn(this, 'setWindow', this.window) },
					[ _('← Back to %s').format(win[1]) ]),
				' ', _('30 minutes before and after %s').format(analysis.localTime(zoom.t0 + (zoom.t1 - zoom.t0) / 2, true))
			]) : '',
			E('h3', {}, [ _('Received power (dBm)') ]), charts[0],
			E('h3', {}, [ _('Laser temperature (°C)') ]), charts[1],
			E('h3', {}, [ _('BIP errors %s').format(per) ]), charts[2],
			E('div', { 'class': 'right' }, E('small', {}, [
				zoom ? _('(±30 minutes, %s)').format(SOURCES[zoom.source] || zoom.source)
					: _('(%s window, %s)').format(win[1], point)
			]))
		]);
	},

	/* Downloads what the graphs show (the window, or the zoom) as CSV */
	exportCsv: function() {
		/* the span the graphs draw: after downtime the rings hold older points */
		var c = this.charts && this.charts[0] && this.charts[0].chart,
		    d = new Date(),
		    name = 'xr500v-gpon-%s-%04d%02d%02d-%02d%02d.csv'.format(this.zoom ? 'event' : this.window,
			d.getFullYear(), d.getMonth() + 1, d.getDate(), d.getHours(), d.getMinutes()),
		    url = URL.createObjectURL(new Blob([ analysis.csv(this.points, this.tipEvents, this.step,
			c ? c.t0 : null, c ? c.t1 : null) ], { type: 'text/csv' })),
		    link = E('a', { 'href': url, 'download': name, 'style': 'display:none' });

		document.body.appendChild(link);
		link.click();
		document.body.removeChild(link);
		window.setTimeout(function() { URL.revokeObjectURL(url); }, 10000);
	},

	/* The graphs from 30 minutes before t to 30 after, at the best detail
	 * the recorder still has; t is the event's time. */
	openAround: function(t) {
		var seq = ++this.seq, tab = document.querySelector('.cbi-tabmenu li[data-tab="graphs"] a');

		if (tab)
			tab.click();
		return L.resolveDefault(callAround(t), null).then(L.bind(function(res) {
			if (seq != this.seq || !res)
				return;
			this.zoom = res;
			dom.content(this.graphs, this.renderGraphs({ window: this.window }, {}, res));
			this.graphs.scrollIntoView({ block: 'start' });
		}, this));
	},

	renderCounters: function(s) {
		var op = L.isObject(s.optics) ? s.optics : {},
		    c = L.isObject(s.counters) ? s.counters : {},
		    olt = L.isObject(s.olt) ? s.olt : {},
		    fec = L.isObject(s.fec) ? s.fec : {},
		    AGENT = 'omci.agent status',
		    rows = [
			[ _('PON state'), (s.state != null) ? 'O' + s.state : null, AGENT ],
			[ _('In O5 since'), !s.o5_since ? null : (s.now >= s.o5_since)
				? '%s (%t)'.format(datetime(s.o5_since), s.now - s.o5_since) : datetime(s.o5_since), _('kernel log') ],
			[ _('ONU ID'), s.onu_id, AGENT ],
			[ _('OLT vendor / version'), olt.vendor ? '%s / %s'.format(olt.vendor, olt.version || '—') : null, AGENT ],
			[ _('OMCI profile'), olt.profile, AGENT ],
			[ _('FEC downstream / upstream'), (fec.ds != null || fec.us != null) ? '%s / %s'.format(fec.ds != null ? fec.ds : '—', fec.us != null ? fec.us : '—') : null, AGENT ],
			[ _('Serial number'), s.serial_hint, AGENT ],
			[ _('Received power'), dbmText(op.rx_nw), AGENT ],
			[ _('Transmitted power'), dbmText(op.tx_nw), AGENT ],
			[ _('Laser bias current'), scaled(op.bias_ua, 1000, '%.1f mA'), AGENT ],
			[ _('Laser temperature'), scaled(op.temp_mc, 1000, '%.1f °C'), AGENT ],
			[ _('Optical module supply'), scaled(op.volt_uv, 1e6, '%.3f V'), AGENT ],
			[ _('Optical module alarms'), (op.alarms != null) ? '0x%02x'.format(op.alarms) : null, AGENT ],
			[ _('BIP errors since boot'), c.bip_total, 'sysfs gpon_bip' ],
			[ _('BIP errors this session'), c.bip_session, 'sysfs gpon_bip' ],
			[ _('GEM delineation losses'), c.gem_delin, 'sysfs gpon_gem_delin' ],
			[ _('Grant FIFO resyncs'), c.sfifo_resync, 'debugfs gpon_sfifo_resync_count' ],
			[ _('Grant FIFO glitches'), c.sfifo_glitch, 'debugfs gpon_sfifo_glitch_count' ],
			[ _('Deactivate from the OLT'), c.deactivate, _('kernel log') ]
		];

		/* lab debugfs counters are optional: no row when absent */
		rows = rows.filter(function(r) {
			return r[1] != null || r[2].indexOf('debugfs') != 0;
		});

		return [ table([ _('Value'), _('Current'), _('Source') ], rows.map(function(r) {
			return [ r[0], (r[1] != null) ? String(r[1]) : '—', E('code', {}, [ r[2] ]) ];
		}), ''), this.renderGem(s) ];
	},

	renderGem: function(s) {
		var g = L.isObject(s.gem) ? s.gem : null,
		    title = E('h3', {}, [ _('GEM ports') ]);

		if (!g || !g.enabled)
			return E([], [ title, E('div', { 'class': 'cbi-section-descr' }, [
				_('Per-port traffic counters are off. To turn them on until the next reboot:'), ' ',
				E('code', {}, [ 'echo 1 > ' + GEM_MIB_ENABLE ])
			]) ]);

		return E([], [ title,
			E('div', { 'class': 'cbi-section-descr' }, [
				_('GEM payload since the session started. Rates are averages over the last minute.')
			]),
			table([ _('GEM port'), _('Received'), _('Sent'), _('Receive rate'), _('Send rate') ],
				(g.ports || []).map(function(p) {
					return [ (p.id == 4095) ? _('%d (multicast)').format(p.id) : String(p.id),
						volume(p.rx_bytes, p.rx_frames), volume(p.tx_bytes, p.tx_frames),
						rate(p.rx_bps), rate(p.tx_bps) ];
				}), g.ready ? _('No GEM ports configured') : _('Counters not ready yet'))
		]);
	},

	renderEye: function(res) {
		if (!L.isObject(res))
			return E('em', {}, [ _('No answer from the monitor.') ]);
		if (!res.ok)
			return E('div', { 'class': 'alert-message warning' }, [ EYE_ERRORS[res.error] || String(res.error) ]);

		var step = res.step, max = res.max, cnt = {}, hs = [], vs = [], h, v;

		(res.points || []).forEach(function(p) { cnt[p[0] * 128 + p[1]] = p[2]; });
		for (h = 0; h < 128; h += step)
			hs.push(h);
		for (v = 128 - step; v >= 0; v -= step)
			vs.push(v);

		/* the clean span along the threshold-0 row and the phase-0 column */
		function span(list, key) {
			var z = list.filter(function(x) { return cnt[key(x)] === 0; });
			return z.length ? [ z[0] - 64, z[z.length - 1] - 64 ] : null;
		}
		var hOpen = span(hs, function(x) { return x * 128 + 64; }),
		    vOpen = span(vs.slice().reverse(), function(x) { return 64 * 128 + x; }),
		    LABEL = 'padding:0 4px 0 0;font-size:10px;line-height:8px;text-align:right;white-space:nowrap;overflow:visible',
		    CELL = 'width:8px;height:8px;padding:0;border:0';

		var rows = vs.map(function(v) {
			return E('tr', {}, [ E('td', { 'style': LABEL }, [ ((v - 64) % 16 == 0) ? '%+d'.format(v - 64) : '' ]) ]
				.concat(hs.map(function(h) {
					var c = cnt[h * 128 + v];
					var bg = (c == null) ? 'transparent' : (c == 0) ? 'rgba(76,175,80,.55)'
						: 'rgba(244,67,54,%.2f)'.format(0.15 + 0.85 * Math.min(1, c / max));

					return E('td', { 'style': CELL + ';background:' + bg,
						'title': (c != null) ? _('phase %+d, threshold %+d: %d').format(h - 64, v - 64, c) : '' });
				})));
		});
		var axis = E('tr', {}, [ E('td') ].concat(hs.map(function(h) {
			return E('td', { 'style': 'padding:0;font-size:10px;white-space:nowrap;overflow:visible' },
				[ ((h - 64) % 32 == 0) ? '%+d'.format(h - 64) : '' ]);
		})));

		return E([], [
			E('p', {}, [
				hOpen ? _('At threshold 0 the eye is clean from phase %+d to %+d (%d of 128 codes).').format(hOpen[0], hOpen[1], hOpen[1] - hOpen[0] + step) : _('No clean point at threshold 0.'),
				' ',
				vOpen ? _('At phase 0, from threshold %+d to %+d.').format(vOpen[0], vOpen[1]) : ''
			]),
			E('table', { 'style': 'border-collapse:collapse;margin:8px 0' }, rows.concat([ axis ])),
			E('div', { 'class': 'cbi-section-descr' }, [
				_('Rows: decision threshold. Columns: sampling phase. Green: no disagreement; red: disagreements, darker for more. Measured %s.').format(datetime(res.t))
			])
		]);
	},

	measureEye: function(ev) {
		dom.content(this.eye, [ E('em', { 'class': 'spinning' }, [ _('Measuring…') ]) ]);

		return callEyeScan().then(L.bind(function(res) {
			dom.content(this.eye, this.renderEye(res));
		}, this)).catch(L.bind(function(err) {
			dom.content(this.eye, E('div', { 'class': 'alert-message warning' }, [ String(err) ]));
		}, this));
	},

	renderEvents: function(events) {
		return table([ _('Time'), _('Severity'), _('Event'), '' ], (events || []).slice(0, EVENTS_SHOWN).map(L.bind(function(e) {
			return [ datetime(e.t), tag(e.severity), analysis.eventText(e),
				E('button', { 'class': 'cbi-button', 'title': _('Graphs from 30 minutes before to 30 after'),
					'click': ui.createHandlerFn(this, 'openAround', e.t) }, [ _('±30 min') ]) ];
		}, this)), _('No events recorded yet'));
	},

	renderAlerts: function(s) {
		return table([ _('Severity'), _('Active alert') ], (s.alerts || []).map(function(a) {
			return [ tag(a.severity), ALERTS[a.id] || a.id ];
		}), (s.health == 'nofiber') ? _('No fiber') : (s.stale || s.health == 'unknown') ? _('No data') : _('No active alerts'));
	},

	setWindow: function(id, ev) {
		var seq = ++this.seq;

		this.window = id;
		this.zoom = null;

		this.buttons.forEach(function(b) {
			b.classList.toggle('cbi-button-action', b.getAttribute('data-window') == id);
		});

		return Promise.all([
			L.resolveDefault(callSummary(), {}),
			L.resolveDefault(callHistory(id), {})
		]).then(L.bind(function(res) {
			if (seq == this.seq)
				dom.content(this.graphs, this.renderGraphs(res[1], res[0]));
		}, this));
	},

	refresh: function() {
		var seq = this.seq;

		return Promise.all([
			L.resolveDefault(callSummary(), {}),
			L.resolveDefault(callHistory(this.window), {}),
			L.resolveDefault(callEvents(EVENTS_KEPT), [])
		]).then(L.bind(function(res) {
			var keep = (this.tip.style.display != 'none' && this.tipOwner == 'graph') ? this.lastTip : null,
			    c;

			this.evData = res[2];
			/* a window chosen meanwhile has its own, newer reply; a zoom stays */
			if (seq == this.seq && !this.zoom) {
				dom.content(this.graphs, this.renderGraphs(res[1], res[0]));
				/* the minute's redraw keeps an open box and its cursor, with the
				 * new values, while that moment is still in the window */
				c = this.charts && this.charts[0] && this.charts[0].chart;
				if (keep && c && keep.t >= c.t0 && keep.t <= c.t1) {
					this.charts.forEach(function(n) { n.chart.setCursor(keep.t); });
					this.showTip(keep.t, keep.cx, keep.cy);
				}
			}
			dom.content(this.counters, this.renderCounters(res[0]));
			dom.content(this.events, this.renderEvents(res[2]));
			dom.content(this.alerts, this.renderAlerts(res[0]));
		}, this));
	},

	render: function(data) {
		var summary = data[0], history = data[1], events = data[2],
		    m, s, o;

		m = new form.Map('xr500v-gpon');

		s = m.section(form.NamedSection, 'thresholds', 'thresholds', _('Thresholds'),
			_('Alerts and graph lines use these limits. Changes apply to the next sample.'));

		o = s.option(form.Value, 'rx_warn_dbm', _('Received power warning (dBm)'));
		o.datatype = 'float';
		o.placeholder = '-25';

		o = s.option(form.Value, 'rx_crit_dbm', _('Received power critical (dBm)'),
			_('Class B+ receiver sensitivity is −27 dBm.'));
		o.datatype = 'float';
		o.placeholder = '-27';

		o = s.option(form.Value, 'rx_high_dbm', _('Received power overload warning (dBm)'));
		o.datatype = 'float';
		o.placeholder = '-9';

		o = s.option(form.Value, 'temp_warn_c', _('Laser temperature warning (°C)'));
		o.datatype = 'float';
		o.placeholder = '70';

		o = s.option(form.Value, 'bip_burst_per_min', _('BIP burst (errors per minute)'));
		o.datatype = 'uinteger';
		o.placeholder = '10';

		this.buttons = WINDOWS.map(L.bind(function(w) {
			return E('button', {
				'class': 'cbi-button' + (w[0] == this.window ? ' cbi-button-action' : ''),
				'data-window': w[0],
				'click': ui.createHandlerFn(this, 'setWindow', w[0])
			}, [ w[1] ]);
		}, this));

		this.tip = E('div', { 'style': TIP_STYLE });
		this.patterns = E('div', {}, [ E('em', {}, [ _('Loading…') ]) ]);
		this.evData = events;
		this.graphs = E('div', {}, this.renderGraphs(history, summary));
		this.counters = E('div', {}, this.renderCounters(summary));
		this.events = E('div', {}, this.renderEvents(events));
		this.alerts = E('div', {}, this.renderAlerts(summary));
		this.eye = E('div', {}, [ E('em', {}, [ _('Not measured yet.') ]) ]);

		return m.render().then(L.bind(function(mapEl) {
			var tabs = E('div', {}, [
				E('div', { 'data-tab': 'graphs', 'data-tab-title': _('Graphs') }, [
					E('div', { 'class': 'cbi-section' }, [
						E('div', {}, [ E('strong', {}, [ _('Window:') ]), ' ' ].concat(this.buttons, [ ' ',
							E('button', { 'class': 'cbi-button', 'title': _('Download what the graphs show, for a spreadsheet'),
								'click': ui.createHandlerFn(this, 'exportCsv') }, [ _('Export CSV') ]) ])),
						this.graphs
					])
				]),
				E('div', { 'data-tab': 'patterns', 'data-tab-title': _('Patterns') }, [
					E('div', { 'class': 'cbi-section' }, [
						E('div', { 'class': 'cbi-section-descr' }, [
							_('Errors and events per hour over the last 30 days. A problem that comes back at the same time of day shows up as a column.')
						]),
						this.patterns
					])
				]),
				E('div', { 'data-tab': 'counters', 'data-tab-title': _('Counters') }, [
					E('div', { 'class': 'cbi-section' }, this.counters)
				]),
				E('div', { 'data-tab': 'events', 'data-tab-title': _('Events') }, [
					E('div', { 'class': 'cbi-section' }, this.events)
				]),
				E('div', { 'data-tab': 'eye', 'data-tab-title': _('Eye') }, [
					E('div', { 'class': 'cbi-section' }, [
						E('div', { 'class': 'cbi-section-descr' }, [
							_('Lab measurement of the received signal with the PHY eye sampler: each cell counts the bits where a sample at that phase and threshold disagrees with the data. The data path is not touched; the sweep takes a few seconds.')
						]),
						E('button', { 'class': 'cbi-button cbi-button-action',
							'click': ui.createHandlerFn(this, 'measureEye') }, [ _('Measure the eye') ]),
						this.eye
					])
				]),
				E('div', { 'data-tab': 'alerts', 'data-tab-title': _('Alerts') }, [
					E('div', { 'class': 'cbi-section' }, [ E('h3', {}, [ _('Active alerts') ]), this.alerts ]),
					mapEl
				])
			]);

			var root = E('div', {}, [
				E('h2', {}, [ _('Fiber') ]),
				E('div', { 'class': 'cbi-map-descr' }, [
					_('Health of the GPON link, sampled every minute by the xr500v-gpon monitor. History survives reboots.')
				]),
				tabs,
				this.tip
			]);

			/* a tap outside the graphs and the map closes the box a tap opened
			 * (the map cell's own touchstart runs first and opens it) */
			document.addEventListener('touchstart', L.bind(function(ev) {
				if (this.graphs.contains(ev.target) || this.patterns.contains(ev.target))
					return;
				this.tip.style.display = 'none';
				(this.charts || []).forEach(function(c) { c.chart.setCursor(null); });
			}, this), { passive: true });

			/* the tab menu is inserted before the group: it needs a parent */
			ui.tabs.initTabGroup(tabs.childNodes);

			/* LuCI reopens the last tab: load the map now if that is this one */
			var ptab = root.querySelector('.cbi-tabmenu li[data-tab="patterns"] a'),
			    ppane = tabs.querySelector('div[data-tab="patterns"]');

			if (ptab)
				ptab.addEventListener('click', L.bind(this.loadPatterns, this));
			if (ppane && ppane.getAttribute('data-tab-active') == 'true')
				this.loadPatterns();
			poll.add(L.bind(this.refresh, this), 60);

			return root;
		}, this));
	}
});
