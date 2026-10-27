'use strict';
'require baseclass';

/*
 * Day x hour map for the Patterns tab: one row per day, one cell per hour.
 * render(map, now, onCell): map comes from analysis.heatmap(); onCell(info,
 * clientX, clientY) gets { day, label, hour, cell } on hover or tap, and
 * null when the pointer leaves the map.
 */

var CELL = 'width:15px;height:15px;padding:0;border:1px solid rgba(128,128,128,.25)';

function color(c, maxBip, future) {
	if (future)
		return 'background:transparent;border-color:transparent';
	if (c.crit)
		return 'background:rgba(244,67,54,%.2f)'.format(0.75 + 0.25 * Math.min(1, c.crit / 3));	/* above any orange */
	if (c.bip || c.warn) {
		var f = maxBip ? Math.log(1 + c.bip) / Math.log(1 + maxBip) : 0;

		return 'background:rgba(255,152,0,%.2f)'.format(Math.max(c.warn ? 0.35 : 0.15, 0.15 + 0.85 * f));
	}
	if (!c.seen)
		return 'background:repeating-linear-gradient(45deg,rgba(128,128,128,.35) 0 2px,transparent 2px 5px)';
	return 'background:rgba(128,128,128,.10)';
}

function swatch(style, text) {
	return E('span', { 'style': 'margin-right:14px;white-space:nowrap' }, [
		E('span', { 'style': 'display:inline-block;vertical-align:middle;margin-right:4px;' + CELL + ';' + style }),
		text
	]);
}

return baseclass.extend({
	render: function(map, now, onCell) {
		var maxBip = 0, head = [ E('th', { 'style': 'padding:0 6px 0 0' }) ], rows = [], table;

		map.forEach(function(d) {
			d.hours.forEach(function(c) { maxBip = Math.max(maxBip, c.bip); });
		});

		for (var h = 0; h < 24; h++)
			head.push(E('th', { 'style': 'padding:0;font-weight:normal;font-size:10px;text-align:left' },
				[ (h % 6 == 0) ? String(h) : '' ]));

		map.forEach(function(d) {
			var start = new Date(d.day + 'T00:00:00').getTime() / 1000,
			    cells = [ E('th', { 'style': 'padding:0 6px 0 0;font-weight:normal;font-size:11px;text-align:right;white-space:nowrap' }, [ d.label ]) ];

			d.hours.forEach(function(c, h) {
				var td = E('td', { 'style': CELL + ';' + color(c, maxBip, start + h * 3600 > now) }),
				    info = { day: d.day, label: d.label, hour: h, cell: c };

				td.addEventListener('mousemove', function(ev) { onCell(info, ev.clientX, ev.clientY); });
				td.addEventListener('touchstart', function(ev) {
					if (ev.touches && ev.touches[0])
						onCell(info, ev.touches[0].clientX, ev.touches[0].clientY);
				}, { passive: true });
				cells.push(td);
			});
			rows.push(E('tr', {}, cells));
		});

		table = E('table', { 'style': 'border-collapse:collapse;margin:8px 0' }, [ E('tr', {}, head) ].concat(rows));
		table.addEventListener('mouseleave', function() { onCell(null); });

		return E('div', {}, [
			table,
			E('div', { 'style': 'font-size:12px;margin-top:6px' }, [
				swatch(color({ crit: 1 }, 0, false), _('critical event')),
				swatch(color({ bip: 1, warn: 1 }, 1, false), _('BIP errors or warnings')),
				swatch(color({ seen: true }, 0, false), _('no errors')),
				swatch(color({ seen: false }, 0, false), _('no data'))
			])
		]);
	}
});
