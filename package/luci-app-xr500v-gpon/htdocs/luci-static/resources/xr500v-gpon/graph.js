'use strict';
'require baseclass';
'require xr500v-gpon.analysis as analysis';

/*
 * History graph drawn like the LuCI realtime graphs: white SVG box, filled
 * polyline, light grid, legend table below.
 *
 * render(points, opts): points come from the xr500v.gpon history method;
 * opts = { field: 'rx'|'temp'|'bip', unit, label, color,
 *          lines: [ { value, label, color } ], height, span, step, now,
 *          events, onMarker(t) }
 * The returned node carries `chart` for the cursor: cursor(nodes, onMove)
 * moves one vertical line through every graph at once, by mouse or touch.
 */

var NS = 'http://www.w3.org/2000/svg';
var TEXT_STYLE = 'fill:#eee;font-size:9pt;font-family:sans-serif;text-shadow:1px 1px 1px #000';
var TICKS = [ 3600, 10800, 21600, 86400, 172800, 604800 ];
var MARK = { crit: '#f44336', warn: '#ff9800', info: '#9e9e9e' };
var RANK = { crit: 3, warn: 2, info: 1 };

function svg(tag, attrs, text) {
	var el = document.createElementNS(NS, tag);

	for (var k in attrs)
		el.setAttribute(k, attrs[k]);

	if (text != null)
		el.appendChild(document.createTextNode(text));

	return el;
}

/* Real minus sign, as in the overview section */
function num(v, digits) {
	return (v < 0 ? '−' : '') + ('%.' + digits + 'f').format(Math.abs(v));
}

/* Integer range whose span divides by 4, so the 25/50/75 % grid lines fall on whole numbers */
function range(data, field, lines) {
	var lo = Infinity, hi = -Infinity;

	data.forEach(function(d) { lo = Math.min(lo, d.min); hi = Math.max(hi, d.max); });

	if (field == 'bip')
		return [ 0, Math.max(4, Math.ceil(hi / 4) * 4) ];

	(lines || []).forEach(function(l) { lo = Math.min(lo, l.value); hi = Math.max(hi, l.value); });

	if (!isFinite(lo))
		return [ 0, 4 ];

	var pad = Math.max(1, (hi - lo) * 0.1);

	lo = Math.floor(lo - pad);

	return [ lo, lo + Math.ceil((Math.ceil(hi + pad) - lo) / 4) * 4 ];
}

function tickLabel(t, step) {
	var d = new Date(t * 1000);

	return (step < 86400)
		? '%02d:%02d'.format(d.getHours(), d.getMinutes())
		: '%02d-%02d'.format(d.getMonth() + 1, d.getDate());
}

function legend(data, opts, digits) {
	var cells = [], sum = 0, n = 0, lo = Infinity, hi = -Infinity, fmt;

	fmt = function(v) {
		return isFinite(v) ? num(v, digits) + (opts.unit ? ' ' + opts.unit : '') : '—';
	};

	data.forEach(function(d) {
		sum += (opts.field == 'bip') ? d.avg : d.avg * d.n;
		n += d.n;
		lo = Math.min(lo, d.min);
		hi = Math.max(hi, d.max);
	});

	var cur = data.length ? data[data.length - 1].avg : NaN;

	if (opts.field == 'bip')
		cells = [ _('Current:'), fmt(cur), _('Total:'), fmt(data.length ? sum : NaN), _('Peak:'), fmt(hi) ];
	else
		cells = [ _('Current:'), fmt(cur), _('Average:'), fmt(n ? sum / n : NaN),
		          _('Minimum:'), fmt(lo), _('Maximum:'), fmt(hi) ];

	var row = [ E('td', { 'class': 'td right top' },
		E('strong', { 'style': 'border-bottom:2px solid ' + opts.color }, [ opts.label ])) ];

	for (var i = 0; i < cells.length; i += 2)
		row.push(
			E('td', { 'class': 'td right top' }, E('strong', {}, [ cells[i] ])),
			E('td', { 'class': 'td' }, [ cells[i + 1] ]));

	return E('table', { 'class': 'table', 'style': 'width:100%;table-layout:fixed' },
		E('tr', { 'class': 'tr' }, row));
}

return baseclass.extend({
	render: function(points, opts) {
		var view = document.querySelector('#view'),
		    W = opts.width || ((view && view.offsetWidth > 100) ? view.offsetWidth - 2 : 800),
		    H = opts.height || 200,
		    all = analysis.series(points, opts.field),
		    t1 = opts.now || (all.length ? all[all.length - 1].t : Date.now() / 1000),
		    t0 = t1 - opts.span,
		    /* the rings are capped by count: after downtime they reach back
		     * before the window, which must not count in the legend or scale */
		    data = analysis.inWindow(all, t0, t1 + 1),
		    digits = (opts.field == 'bip') ? 0 : 1,
		    r = range(data, opts.field, opts.lines),
		    root = svg('svg', { 'width': '100%', 'height': '100%', 'version': '1.1' });

		var x = function(t) { return (t - t0) / opts.span * W; };
		var y = function(v) { return H - (v - r[0]) / (r[1] - r[0]) * H; };

		/* time grid aligned to local hours/days */
		var tick = TICKS.filter(function(s) { return opts.span / s <= 8; })[0] || TICKS[TICKS.length - 1],
		    tz = new Date().getTimezoneOffset() * 60;

		for (var t = Math.ceil((t0 - tz) / tick) * tick + tz; t < t1; t += tick) {
			root.appendChild(svg('line', { 'x1': x(t), 'y1': 0, 'x2': x(t), 'y2': '100%', 'style': 'stroke:black;stroke-width:0.1' }));

			if (x(t) < W - 45)
				root.appendChild(svg('text', { 'x': x(t) + 5, 'y': 15, 'style': TEXT_STYLE }, tickLabel(t, tick)));
		}

		/* data: one filled shape per run without gaps, bars for counts */
		var gap = (opts.step || 60) * 2.5, run = [];

		var flush = function() {
			if (run.length) {
				var pts = [ x(run[0].t) + ',' + H ];

				run.forEach(function(d) { pts.push(x(d.t) + ',' + y(d.avg)); });
				pts.push(x(run[run.length - 1].t) + ',' + H);
				root.appendChild(svg('polyline', { 'points': pts.join(' '),
					'style': 'fill:%s;fill-opacity:0.4;stroke:%s;stroke-width:1'.format(opts.color, opts.color) }));
			}

			run = [];
		};

		data.forEach(function(d, i) {
			if (opts.field == 'bip') {
				if (d.avg > 0)
					root.appendChild(svg('rect', { 'x': x(d.t), 'y': y(d.avg),
						'width': Math.max(2, (opts.step || 60) / opts.span * W), 'height': H - y(d.avg),
						'style': 'fill:%s;fill-opacity:0.8'.format(opts.color) }));
				return;
			}

			if (run.length && d.t - run[run.length - 1].t > gap)
				flush();

			run.push(d);
		});

		flush();

		/* value grid */
		[ 0.25, 0.5, 0.75 ].forEach(function(f) {
			var v = r[0] + (r[1] - r[0]) * f;

			root.appendChild(svg('line', { 'x1': 0, 'y1': y(v), 'x2': '100%', 'y2': y(v), 'style': 'stroke:black;stroke-width:0.1' }));
			root.appendChild(svg('text', { 'x': 20, 'y': y(v) - 3, 'style': TEXT_STYLE }, num(v, digits)));
		});

		/* threshold lines */
		(opts.lines || []).forEach(function(l) {
			root.appendChild(svg('line', { 'x1': 0, 'y1': y(l.value), 'x2': '100%', 'y2': y(l.value),
				'style': 'stroke:%s;stroke-width:1;stroke-dasharray:6,4'.format(l.color) }));
			root.appendChild(svg('text', { 'x': W - 10, 'y': y(l.value) - 4, 'text-anchor': 'end', 'style': TEXT_STYLE },
				'%s %s (%s)'.format(num(l.value, digits), opts.unit, l.label)));
		});

		/* events: one marker per marker-wide strip (8 px), the most severe
		 * one wins, so markers never overlap and a flapping link cannot bury
		 * the graph */
		var cols = {};

		(opts.events || []).forEach(function(e) {
			var bin, c;

			if (e.t < t0 || e.t > t1)
				return;
			bin = Math.floor(x(e.t) / 8);
			c = cols[bin];
			if (!c || (RANK[e.severity] || 0) > (RANK[c.severity] || 0))
				cols[bin] = e;
		});
		Object.keys(cols).forEach(function(bin) {
			var e = cols[bin], px = bin * 8 + 4, color = MARK[e.severity] || MARK.info,
			    mark = svg('rect', { 'x': px - 4, 'y': 0, 'width': 8, 'height': 8,
				'style': 'fill:%s;cursor:pointer'.format(color) });

			root.appendChild(svg('line', { 'x1': px, 'y1': 0, 'x2': px, 'y2': '100%',
				'style': 'stroke:%s;stroke-width:1;stroke-opacity:0.45;pointer-events:none'.format(color) }));
			if (opts.onMarker)
				mark.addEventListener('click', function() { opts.onMarker(e.t); });
			root.appendChild(mark);
		});

		if (!data.length)
			root.appendChild(svg('text', { 'x': W / 2, 'y': H / 2, 'text-anchor': 'middle', 'style': TEXT_STYLE }, _('No data yet')));

		/* the cursor line sits under the pointer: it must not take the clicks
		 * meant for the event markers */
		var line = svg('line', { 'x1': 0, 'y1': 0, 'x2': 0, 'y2': '100%', 'style': 'stroke:#333;stroke-width:1;display:none;pointer-events:none' }),
		    box = E('div', { 'style': 'width:100%%;height:%dpx;border:1px solid #000;background:#fff'.format(H) }, root),
		    node = E('div', {}, [ box, legend(data, opts, digits) ]);

		root.appendChild(line);
		node.chart = {
			t0: t0, t1: t1, step: opts.step || 60, box: box,
			/* the drawing is W pixels wide from the SVG's left edge */
			timeAt: function(clientX) {
				return t0 + (clientX - root.getBoundingClientRect().left) / W * opts.span;
			},
			setCursor: function(t) {
				if (t == null || t < t0 || t > t1) {
					line.style.display = 'none';
					return;
				}
				line.setAttribute('x1', x(t));
				line.setAttribute('x2', x(t));
				line.style.display = '';
			}
		};

		return node;
	},

	/* onMove(t, clientX, clientY) with t = null when the pointer leaves */
	cursor: function(nodes, onMove) {
		var charts = nodes.filter(function(n) { return n && n.chart; }).map(function(n) { return n.chart; });

		var move = function(chart, cx, cy) {
			var t = chart.timeAt(cx);

			if (t < chart.t0 || t > chart.t1)
				t = null;
			charts.forEach(function(c) { c.setCursor(t); });
			onMove(t, cx, cy);
		};

		var touch = function(chart) {
			return function(ev) {
				if (ev.touches && ev.touches[0])
					move(chart, ev.touches[0].clientX, ev.touches[0].clientY);
			};
		};

		charts.forEach(function(c) {
			c.box.addEventListener('mousemove', function(ev) { move(c, ev.clientX, ev.clientY); });
			c.box.addEventListener('mouseleave', function() {
				charts.forEach(function(o) { o.setCursor(null); });
				onMove(null);
			});
			/* a tap shows the same box; it stays until the next touch */
			c.box.addEventListener('touchstart', touch(c), { passive: true });
			c.box.addEventListener('touchmove', touch(c), { passive: true });
		});
	}
});
