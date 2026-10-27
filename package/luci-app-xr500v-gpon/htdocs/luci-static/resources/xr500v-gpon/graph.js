'use strict';
'require baseclass';

/*
 * History graph drawn like the LuCI realtime graphs: white SVG box, filled
 * polyline, light grid, legend table below.
 *
 * render(points, opts): points come from the xr500v.gpon history method;
 * opts = { field: 'rx'|'temp'|'bip', unit, label, color,
 *          lines: [ { value, label, color } ], height, span, step, now }
 */

var NS = 'http://www.w3.org/2000/svg';
var TEXT_STYLE = 'fill:#eee;font-size:9pt;font-family:sans-serif;text-shadow:1px 1px 1px #000';
var TICKS = [ 3600, 10800, 21600, 86400, 172800, 604800 ];

function dbm(nw) {
	return (nw > 0) ? 10 * Math.log10(nw / 1e6) : null;
}

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

/* [ { t, n, avg, min, max } ] in display units, points without data skipped */
function series(points, field) {
	var out = [];

	(points || []).forEach(function(p) {
		var v;

		if (field == 'rx')
			v = { avg: dbm(p.rx_avg), min: dbm(p.rx_min), max: dbm(p.rx_max) };
		else if (field == 'temp')
			v = (p.temp_avg != null) ? { avg: p.temp_avg / 1000, min: p.temp_min / 1000, max: p.temp_max / 1000 } : {};
		else
			v = (p.bip != null) ? { avg: p.bip, min: p.bip, max: p.bip } : {};

		if (v.avg != null)
			out.push({ t: p.t, n: p.n || 1, avg: v.avg, min: v.min, max: v.max });
	});

	return out;
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
		    data = series(points, opts.field),
		    digits = (opts.field == 'bip') ? 0 : 1,
		    r = range(data, opts.field, opts.lines),
		    t1 = opts.now || (data.length ? data[data.length - 1].t : Date.now() / 1000),
		    t0 = t1 - opts.span,
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
			if (d.t < t0)
				return;

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

		if (!data.length)
			root.appendChild(svg('text', { 'x': W / 2, 'y': H / 2, 'text-anchor': 'middle', 'style': TEXT_STYLE }, _('No data yet')));

		return E('div', {}, [
			E('div', { 'style': 'width:100%%;height:%dpx;border:1px solid #000;background:#fff'.format(H) }, root),
			legend(data, opts, digits)
		]);
	}
});
