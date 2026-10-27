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
		var tip = (t != null) ? analysis.tooltip(this.points, this.evData, t, this.step) : null,
		    spread = this.step > 60, rows, w, h;

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

		dom.content(this.tip, rows);
		this.tip.style.display = 'block';
		w = this.tip.offsetWidth;
		h = this.tip.offsetHeight;
		this.tip.style.left = ((cx + 14 + w > window.innerWidth - 8) ? Math.max(8, cx - 14 - w) : cx + 14) + 'px';
		this.tip.style.top = ((cy + 14 + h > window.innerHeight - 8) ? Math.max(8, cy - 14 - h) : cy + 14) + 'px';
	},

	renderGraphs: function(history, summary) {
		var win = WINDOWS.filter(function(w) { return w[0] == history.window; })[0] || WINDOWS[0],
		    step = history.step || 60,
		    common = { span: win[2], step: step, now: summary.now, points: history.points },
		    per = (step >= 1800) ? _('per 30 minutes') : (step >= 300) ? _('per 5 minutes') : _('per minute'),
		    point = (step >= 1800) ? _('30-minute averages') : (step >= 300) ? _('5-minute averages') : _('one sample per minute');

		var draw = function(opts) {
			return graph.render(common.points, Object.assign({ span: common.span, step: common.step, now: common.now }, opts));
		};

		var charts = [
			draw({ field: 'rx', unit: 'dBm', label: _('Received power'), color: '#0069d6', height: 220, lines: [
				{ value: threshold('rx_warn_dbm', -25), label: _('warning'), color: '#ff9800' },
				{ value: threshold('rx_crit_dbm', -27), label: _('sensitivity limit'), color: '#f44336' }
			] }),
			draw({ field: 'temp', unit: '°C', label: _('Temperature'), color: '#ff6600', height: 160 }),
			draw({ field: 'bip', unit: '', label: _('BIP errors'), color: '#ff0000', height: 120 })
		];

		this.points = history.points || [];
		this.step = step;
		this.charts = charts;
		this.tip.style.display = 'none';
		graph.cursor(charts, L.bind(this.showTip, this));

		return E('div', {}, [
			E('h3', {}, [ _('Received power (dBm)') ]), charts[0],
			E('h3', {}, [ _('Laser temperature (°C)') ]), charts[1],
			E('h3', {}, [ _('BIP errors %s').format(per) ]), charts[2],
			E('div', { 'class': 'right' }, E('small', {}, [
				_('(%s window, %s)').format(win[1], point)
			]))
		]);
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

		return table([ _('Value'), _('Current'), _('Source') ], rows.map(function(r) {
			return [ r[0], (r[1] != null) ? String(r[1]) : '—', E('code', {}, [ r[2] ]) ];
		}), '');
	},

	renderEvents: function(events) {
		return table([ _('Time'), _('Severity'), _('Event') ], (events || []).slice(0, EVENTS_SHOWN).map(function(e) {
			return [ datetime(e.t), tag(e.severity), analysis.eventText(e) ];
		}), _('No events recorded yet'));
	},

	renderAlerts: function(s) {
		return table([ _('Severity'), _('Active alert') ], (s.alerts || []).map(function(a) {
			return [ tag(a.severity), ALERTS[a.id] || a.id ];
		}), (s.stale || s.health == 'unknown') ? _('No data') : _('No active alerts'));
	},

	setWindow: function(id, ev) {
		var seq = ++this.seq;

		this.window = id;

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
			this.evData = res[2];
			/* a window chosen meanwhile has its own, newer reply */
			if (seq == this.seq)
				dom.content(this.graphs, this.renderGraphs(res[1], res[0]));
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
		this.evData = events;
		this.graphs = E('div', {}, this.renderGraphs(history, summary));
		this.counters = E('div', {}, this.renderCounters(summary));
		this.events = E('div', {}, this.renderEvents(events));
		this.alerts = E('div', {}, this.renderAlerts(summary));

		return m.render().then(L.bind(function(mapEl) {
			var tabs = E('div', {}, [
				E('div', { 'data-tab': 'graphs', 'data-tab-title': _('Graphs') }, [
					E('div', { 'class': 'cbi-section' }, [
						E('div', {}, [ E('strong', {}, [ _('Window:') ]), ' ' ].concat(this.buttons)),
						this.graphs
					])
				]),
				E('div', { 'data-tab': 'counters', 'data-tab-title': _('Counters') }, [
					E('div', { 'class': 'cbi-section' }, this.counters)
				]),
				E('div', { 'data-tab': 'events', 'data-tab-title': _('Events') }, [
					E('div', { 'class': 'cbi-section' }, this.events)
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

			/* a tap outside the graphs closes the box a tap opened */
			document.addEventListener('touchstart', L.bind(function(ev) {
				if (this.graphs.contains(ev.target))
					return;
				this.tip.style.display = 'none';
				(this.charts || []).forEach(function(c) { c.chart.setCursor(null); });
			}, this), { passive: true });

			/* the tab menu is inserted before the group: it needs a parent */
			ui.tabs.initTabGroup(tabs.childNodes);
			poll.add(L.bind(this.refresh, this), 60);

			return root;
		}, this));
	}
});
