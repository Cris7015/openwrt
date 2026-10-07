'use strict';
'require view';
'require rpc';
'require poll';
'require dom';

var getStatus = rpc.declare({ object: 'xr500v.voip', method: 'status', expect: {} });

var sipReasons = {
	400: _('Bad request'), 401: _('Authentication failed'), 403: _('Forbidden'), 404: _('Account not found'),
	407: _('Authentication failed'), 408: _('Request timeout'), 423: _('Interval too brief'),
	480: _('Temporarily unavailable'), 500: _('Server error'), 502: _('Bad gateway'),
	503: _('Service unavailable'), 504: _('Server timeout')
};

function text(value) { return value == null ? '-' : String(value); }
function yesno(value) { return value == null ? _('Unknown') : value ? _('Yes') : _('No'); }
function bytes(value) {
	if (value == null) return '-';
	return value >= 1048576 ? (value / 1048576).toFixed(1) + ' MiB' :
		value >= 1024 ? (value / 1024).toFixed(1) + ' KiB' : value + ' B';
}
function duration(seconds) {
	if (seconds == null) return null;
	return seconds < 60 ? _('%d s').format(seconds) :
		seconds < 3600 ? _('%d min').format(Math.floor(seconds / 60)) :
		seconds < 86400 ? _('%d h').format(Math.floor(seconds / 3600)) : _('%d d').format(Math.floor(seconds / 86400));
}
function row(title, value) {
	return E('tr', { 'class': 'tr' }, [E('td', { 'class': 'td left', 'width': '33%' }, title), E('td', { 'class': 'td left' }, text(value))]);
}
function section(title, rows) {
	return E('div', { 'class': 'cbi-section' }, [E('h3', {}, title), E('table', { 'class': 'table' }, rows)]);
}

function portState(line) {
	if (!line.enabled) return _('Disabled');
	if (line.configured_mode === 'shared' && line.hook === 'unknown') return _('Shared with Phone 1');
	if (line.ringing) return _('Ringing');
	if (line.hook === 'off_hook') return line.audio_open ? _('In call') : _('Off hook');
	return line.hook === 'on_hook' ? _('Idle') : _('Unavailable');
}

function registrationText(r) {
	var since = duration(r.age);
	switch (r.state) {
	case 'registered':
		return since ? _('Registered (%s)').format(since) : _('Registered');
	case 'registering':
		return _('Registering…');
	case 'unregistering':
		return _('Unregistering…');
	default:
		if (!r.code) return _('Failed: no response from the server');
		return _('Failed: %d %s').format(r.code, sipReasons[r.code] || '');
	}
}

function registration(line) {
	if (!line.enabled) return '-';
	// A shared port has no account of its own: it rings on Phone 1's.
	if (line.configured_mode === 'shared') return _('Shared with Phone 1');
	// The configured mode decides while the port is not running; the
	// generated accounts (custom files, legacy setups) once it is.
	if (line.configured_mode === 'local') return _('Not required (local SIP)');
	if (line.sip_running !== true) {
		if (line.network_managed && line.network_ready === false)
			return _('Waiting for %s').format(line.configured_network);
		return _('SIP service stopped');
	}
	if (line.account_mode === 'unconfigured') return _('No account');
	if (line.account_mode === 'local') return _('Not required (local SIP)');
	var regs = line.registrations;
	if (!regs || !regs.length) return _('Waiting for the first attempt');
	if (regs.length === 1) return registrationText(regs[0]);
	return E([], regs.map((r, i) => E('div', {}, _('Account %d: %s').format(r.account || i + 1, registrationText(r)))));
}

function service(line) {
	if (line.configured_mode === 'shared') return _('Shared with Phone 1');
	if (line.sip_running == null && line.manager_running == null) return _('Unknown');
	return line.sip_running && line.manager_running ? _('Running') : _('Stopped');
}

function dialing(line) {
	if (line.configured_mode === 'shared') return _('Through Phone 1');
	return line.dial_mode === 'keypad' ? _('Keypad') : line.dial_mode === 'hotline' ? _('Hotline') :
		line.dial_mode === 'none' ? _('Incoming only') : _('Default');
}

function table(title, lines, fields) {
	var head = E('tr', { 'class': 'tr table-titles' }, [E('th', { 'class': 'th left' }, '')].concat(
		lines.map(l => E('th', { 'class': 'th left', 'data-phone': l.id }, _('Phone %d').format(l.id)))));
	return section(title, [head].concat(fields.map(f =>
		E('tr', { 'class': 'tr' }, [E('td', { 'class': 'td left', 'width': '33%' }, f[0])].concat(
			lines.map(l => E('td', { 'class': 'td left' }, f[1](l))))))));
}

function content(data) {
	if (!data || !data.available)
		return E('p', {}, _('Telephony status is not available on this device.'));
	var lines = data.lines || [];
	return E([], [
		section(_('Service'), [
			row(_('Telephony'), data.service_enabled ? _('Enabled') : _('Disabled')),
			row(_('Processes'), _('%s of %s running').format(text(data.processes_running), text(data.expected_processes))),
			row(_('Drivers'), data.drivers.pcm && data.drivers.slic ? _('Loaded') : _('Missing')),
			row(_('WAN'), data.wan_up == null ? _('Unknown') : data.wan_up ? _('Connected') : _('Disconnected'))
		]),
		table(_('Phones'), lines, [
			[_('State'), portState],
			[_('Registration'), registration],
			[_('SIP service'), service],
			[_('Network'), l => l.network_managed ? '%s (%s)'.format(text(l.configured_network), l.network_ready ? _('up') : _('down')) : text(l.configured_network)],
			[_('Dialing'), dialing],
			[_('Idle feed'), l => l.idle_feed === 'active' ? _('Active') : _('Standby')]
		]),
		table(_('Diagnostics'), lines, [
			[_('Audio stream'), l => l.audio_open == null ? _('Unknown') : l.audio_open ? _('Open') : _('Closed')],
			[_('SLIC channel'), l => l.ec == null ? '-' : 'EC' + l.ec],
			[_('RX / TX slot'), l => text(l.rx_slot) + ' / ' + text(l.tx_slot)],
			[_('RX / TX DMA'), l => text(l.rx_dma) + ' / ' + text(l.tx_dma)],
			[_('Capture / playback queue'), l => bytes(l.capture_bytes) + ' / ' + bytes(l.playback_bytes)],
			[_('Audio received / sent'), l => bytes(l.read_bytes) + ' / ' + bytes(l.write_bytes)],
			[_('Capture drops'), l => text(l.capture_drops)],
			[_('Short TX frames'), l => text(l.tx_short_frames)]
		])
	]);
}

return view.extend({
	load: function() { return getStatus(); },
	render: function(data) {
		this.node = E('div', { 'id': 'voip-status-content' }, [content(data)]);
		this.error = E('p', { 'id': 'voip-status-error', 'class': 'alert-message warning', 'hidden': true, 'role': 'alert' });
		poll.add(() => document.hidden ? Promise.resolve() : this.refresh(), 10);
		return E('div', { 'id': 'voip-status-page' }, [
			E('h2', {}, _('Telephony')),
			E('div', { 'class': 'cbi-map-descr' }, _('Phone ports and SIP registration, refreshed every 10 seconds.')),
			this.error, this.node
		]);
	},
	refresh: function() {
		if (this.busy) return Promise.resolve();
		this.busy = true;
		return getStatus().then(data => {
			dom.content(this.node, content(data));
			this.error.hidden = true;
		}).catch(() => {
			this.error.textContent = _('Unable to refresh the status.');
			this.error.hidden = false;
		}).finally(() => { this.busy = false; });
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
