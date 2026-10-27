'use strict';
'require baseclass';
'require rpc';

var callSummary = rpc.declare({
	object: 'xr500v.gpon',
	method: 'summary'
});

var healthColor = { ok: '#4caf50', warn: '#ff9800', crit: '#f44336' };

function dbm(nw) {
	return (nw > 0) ? 10 * Math.log10(nw / 1e6) : null;
}

function fmt(format, value) {
	return (value != null) ? format.format(value) : '—';
}

/* Signed, with a real minus to match the range text (String.format has no "+" flag) */
function dbmText(v) {
	return (v != null) ? (v < 0 ? '−' : '+') + '%.1f dBm'.format(Math.abs(v)) : '—';
}

function dot(health) {
	return E('span', {
		'style': 'display:inline-block;width:9px;height:9px;border-radius:50%%;margin-right:6px;vertical-align:middle;background:%s'
			.format(healthColor[health] || '#9e9e9e')
	});
}

function rxbar(rx) {
	var pc = (rx != null) ? Math.min(100, Math.max(0, (rx + 27) / 19 * 100)) : 0;

	return E('div', {
		'class': 'cbi-progressbar',
		'title': (rx != null) ? _('%s · healthy range −27 … −8 dBm').format(dbmText(rx)) : '—'
	}, E('div', { 'style': 'width:%.2f%%'.format(pc) }));
}

function linkState(s) {
	if (s.state == 5)
		return [
			dot(s.health),
			_('O5 · Operational'),
			(s.o5_since && s.now >= s.o5_since) ? ' · ' + _('up %s').format('%t'.format(s.now - s.o5_since)) : ''
		];

	/* null: omci.agent did not answer this sample */
	if (s.state == null)
		return [ dot(s.health), _('No data') ];

	return [ dot('crit'), _('O%d · Not operational').format(s.state) ];
}

function olt(s) {
	var o = L.isObject(s.olt) ? s.olt : {};

	return [
		o.vendor || '—',
		o.profile ? _('OMCI profile "%s"').format(o.profile) : null,
		(s.onu_id != null) ? _('ONU ID %d').format(s.onu_id) : null
	].filter(function(v) { return v != null }).join(' · ');
}

function transmit(op) {
	return [
		dbmText(dbm(op.tx_nw)),
		_('laser %s').format(fmt('%.0f mA', (op.bias_ua != null) ? op.bias_ua / 1000 : null)),
		fmt('%.1f °C', (op.temp_mc != null) ? op.temp_mc / 1000 : null)
	].join(' · ');
}

function errors(c) {
	var parts = [
		_('BIP %s').format(fmt('%d', c.bip_total)),
		_('Deactivate %s').format(fmt('%d', c.deactivate))
	];

	if (c.sfifo_resync != null)
		parts.push(_('resyncs %d').format(c.sfifo_resync));

	parts.push(_('%s GEM delineation losses').format(fmt('%d', c.gem_delin)));

	return parts.join(' · ');
}

return baseclass.extend({
	title: _('Fiber (GPON)'),

	load: function() {
		return L.resolveDefault(callSummary(), null);
	},

	render: function(s) {
		var table = E('table', { 'class': 'table' }),
		    rows;

		if (!L.isObject(s) || s.stale)
			rows = [ _('Link state'), _('No data') ];
		else
			rows = [
				_('Link state'),        linkState(s),
				_('OLT'),               olt(s),
				_('Received power'),    rxbar(dbm(L.isObject(s.optics) ? s.optics.rx_nw : null)),
				_('Transmitted power'), transmit(L.isObject(s.optics) ? s.optics : {}),
				_('Errors since boot'), errors(L.isObject(s.counters) ? s.counters : {})
			];

		for (var i = 0; i < rows.length; i += 2) {
			table.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, [ rows[i] ]),
				/* a bare string would be parsed as HTML: the OLT vendor id comes
				 * from the network */
				E('td', { 'class': 'td left' }, (typeof rows[i + 1] == 'string') ? [ rows[i + 1] ] : rows[i + 1])
			]));
		}

		table.appendChild(E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'colspan': 2 }, [
				E('a', { 'href': L.url('admin/network/omci/fiber') }, [ _('Details and graphs →') ])
			])
		]));

		return table;
	}
});
