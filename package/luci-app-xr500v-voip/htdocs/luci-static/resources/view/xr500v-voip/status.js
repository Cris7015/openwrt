'use strict';
'require view';
'require rpc';
'require poll';
'require dom';

var getStatus = rpc.declare({ object: 'xr500v.voip', method: 'status', expect: {} });

function text(value) { return value == null ? '-' : String(value); }
function yesno(value) { return value == null ? _('Unknown') : value ? _('Yes') : _('No'); }
function bytes(value) {
	if (value == null) return '-';
	return value >= 1048576 ? (value / 1048576).toFixed(1) + ' MiB' :
		value >= 1024 ? (value / 1024).toFixed(1) + ' KiB' : value + ' B';
}
function row(title, value) {
	return E('tr', {}, [E('td', { 'class': 'td left', 'width': '42%' }, title), E('td', { 'class': 'td left' }, text(value))]);
}
function section(title, rows) {
	return E('div', { 'class': 'cbi-section' }, [E('h3', {}, title), E('table', { 'class': 'table' }, rows)]);
}
function portState(line) {
	if (!line.enabled) return _('Disabled');
	if (line.configured_mode === 'shared' && line.hook === 'unknown') return _('Shared with Phone 1');
	if (line.ringing) return _('Ringing');
	return line.hook === 'on_hook' ? _('On hook') : line.hook === 'off_hook' ? _('Off hook') : _('Unavailable');
}
function account(line) {
	// A shared port has no account of its own: it rings on Phone 1's.
	if (line.configured_mode === 'shared') return _('Shared with Phone 1');
	return line.account_mode === 'local' ? _('Local SIP (no registration)') :
		line.account_mode === 'registration' ? _('Configured; registration not verified') :
		line.account_mode === 'unconfigured' ? _('No account') : _('Unknown');
}
function ports(lines) {
	var fields = [
		[_('Port state'), portState],
		[_('Enabled in configuration'), l => yesno(l.enabled)],
		[_('Baresip process running'), l => l.configured_mode === 'shared' ? _('Shared with Phone 1') : yesno(l.sip_running)],
		[_('Call manager running'), l => l.configured_mode === 'shared' ? _('Shared with Phone 1') : yesno(l.manager_running)],
		[_('SIP account'), account],
		[_('Configured network'), l => text(l.configured_network)],
		[_('Selected network ready'), l => l.network_managed ? yesno(l.network_ready) : _('Legacy configuration')],
		[_('Outgoing dialing'), l => l.configured_mode === 'shared' ? _('Dialed on the Phone 1 line') :
			l.dial_mode === 'keypad' ? _('Telephone keypad') : l.dial_mode === 'hotline' ? _('Fixed destination') : l.dial_mode === 'none' ? _('Incoming calls only') : _('Legacy configuration')],
		[_('Idle line feed'), l => l.idle_feed === 'active' ? _('Active (instant dial tone, audible hiss)') : _('Standby (silent, recommended)')],
		[_('Audio stream open'), l => yesno(l.audio_open)],
		[_('SLIC channel'), l => l.ec == null ? '-' : 'EC' + l.ec],
		[_('RX / TX slot'), l => text(l.rx_slot) + ' / ' + text(l.tx_slot)],
		[_('RX / TX DMA'), l => text(l.rx_dma) + ' / ' + text(l.tx_dma)],
		[_('Capture / playback queue'), l => bytes(l.capture_bytes) + ' / ' + bytes(l.playback_bytes)],
		[_('Audio received / sent'), l => bytes(l.read_bytes) + ' / ' + bytes(l.write_bytes)],
		[_('Capture drops'), l => text(l.capture_drops)],
		[_('Short TX frames'), l => text(l.tx_short_frames)]
	];
	var head = E('tr', { 'class': 'tr table-titles' }, [E('th', { 'class': 'th left' }, _('Parameter'))].concat(
		lines.map(l => E('th', { 'class': 'th left', 'data-phone': l.id }, _('Phone %s').format(l.id)))));
	return section(_('FXS ports'), [head].concat(fields.map(f =>
		E('tr', {}, [E('td', { 'class': 'td left', 'width': '42%' }, f[0])].concat(
			lines.map(l => E('td', { 'class': 'td left' }, f[1](l))))))));
}
function content(data) {
	if (!data || !data.available)
		return E('p', {}, _('Telephony status is not available on this device.'));
	return E([], [
		section(_('Service and drivers'), [
			row(_('Service enabled in configuration'), yesno(data.service_enabled)),
			row(_('Running / expected processes'), text(data.processes_running) + ' / ' + text(data.expected_processes)),
			row(_('PCM driver present'), yesno(data.drivers.pcm)),
			row(_('SLIC driver present'), yesno(data.drivers.slic)),
			row(_('PCM engine running'), yesno(data.pcm.running)),
			row(_('WAN connection available'), yesno(data.wan_up))
		]),
		data.wan_up === false ? E('p', {}, _('No WAN connection. This page and local SIP calls can work without fiber.')) : '',
		ports(data.lines || []),
		section(_('Audio: saved configuration'), [
			row(_('Echo cancellation'), yesno(data.audio.echo_cancel)),
			row(_('Mild noise suppression'), yesno(data.audio.noise_suppress)),
			row(_('Playback level'), data.audio.playback_percent == null ? '-' : data.audio.playback_percent + '%')
		]),
		E('p', {}, _('A running SIP process does not confirm provider registration. On-hook state does not prove a phone is connected, and an open audio stream does not by itself confirm an established call.')),
		E('p', {}, _('Counters belong to the audio stream and may reset when it closes. This view is read-only.'))
	]);
}
return view.extend({
	load: function() { return getStatus(); },
	render: function(data) {
		this.node = E('div', { 'id': 'voip-status-content' }, [content(data)]);
		this.error = E('p', { 'id': 'voip-status-error', 'class': 'alert-message warning', 'hidden': true, 'role': 'alert' });
		this.updated = E('span', {}, '');
		this.button = E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-action', 'click': this.refresh.bind(this) }, _('Refresh'));
		this.updated.textContent = _('Last query: ') + new Date().toLocaleTimeString();
		poll.add(() => document.hidden ? Promise.resolve() : this.refresh(), 10);
		return E('div', { 'id': 'voip-status-page' }, [
			E('h2', {}, _('Telephony status')),
			E('p', {}, _('Local FXS port and telephony service status. Updated every 10 seconds.')),
			this.error, this.node,
			E('div', { 'class': 'cbi-page-actions' }, [this.updated, ' ', this.button])
		]);
	},
	refresh: function() {
		if (this.busy) return Promise.resolve();
		this.busy = true; this.button.disabled = true;
		return getStatus().then(data => {
			dom.content(this.node, content(data));
			this.error.hidden = true;
			this.updated.textContent = _('Last query: ') + new Date().toLocaleTimeString();
		}).catch(() => {
			this.error.textContent = _('Unable to refresh. The last successful reading is still shown; telephony was not changed.');
			this.error.hidden = false;
		}).finally(() => { this.busy = false; this.button.disabled = false; });
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
