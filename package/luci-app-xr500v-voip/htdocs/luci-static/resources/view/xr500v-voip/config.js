'use strict';
'require view';
'require form';
'require rpc';
'require ui';
'require dom';

var readConfig = rpc.declare({ object: 'xr500v.voip', method: 'config_get', expect: {} });

var feedValues = { standby: _('Standby'), active: _('Active') };
var callerIdValues = { off: _('Off'), v23: _('V.23 FSK (ETSI)'), bell202: _('Bell 202 FSK') };

// Order matters: it is the parameter order of config_apply.
var fields = [
	{ key: 'service_enabled', tab: 'general', type: 'flag', name: _('Enable telephony') },
	{ key: 'phone1_enabled', tab: 'general', type: 'flag', name: _('Enable Phone 1') },
	{ key: 'phone2_enabled', tab: 'general', type: 'flag', name: _('Enable Phone 2') },
	{ key: 'echo_cancel', tab: 'audio', type: 'flag', name: _('Echo cancellation') },
	{ key: 'noise_suppress', tab: 'audio', type: 'flag', name: _('Noise suppression') },
	{ key: 'playback_percent', tab: 'audio', type: 'number', min: 25, max: 100, unit: '%',
		name: _('Playback level'), description: _('Default: 50 %.') },
	{ key: 'phone1_idle_feed', tab: 'line', type: 'list', values: feedValues, name: _('Phone 1 idle feed') },
	{ key: 'phone2_idle_feed', tab: 'line', type: 'list', values: feedValues, name: _('Phone 2 idle feed'),
		description: _('Standby is silent; the dial tone follows a short delay. Active gives an immediate dial tone, with an audible hiss while idle.') },
	{ key: 'call_waiting', tab: 'calls', type: 'flag', name: _('Call waiting'),
		description: _('A second call during a conversation is announced with a tone; the R key (hook flash) answers it.') },
	{ key: 'ring_on_ms', tab: 'calls', type: 'number', min: 300, max: 10000, unit: 'ms', name: _('Ring on'),
		description: _('Default: 1000 ms.') },
	{ key: 'ring_off_ms', tab: 'calls', type: 'number', min: 300, max: 10000, unit: 'ms', name: _('Ring off'),
		description: _('Default: 4000 ms.') },
	{ key: 'ring_prefeed_ms', tab: 'calls', type: 'number', min: 0, max: 5000, unit: 'ms', name: _('Ring pre-feed'),
		description: _('Line power-up before the first ring. Default: 1500 ms.') },
	{ key: 'flash_min_ms', tab: 'calls', type: 'number', min: 40, max: 1500, unit: 'ms', name: _('Hook flash minimum'),
		description: _('Shorter breaks are ignored. Default: 80 ms.') },
	{ key: 'flash_max_ms', tab: 'calls', type: 'number', min: 40, max: 1500, unit: 'ms', name: _('Hook flash maximum'),
		description: _('Longer breaks hang up. Default: 750 ms.') },
	{ key: 'emergency_numbers', tab: 'calls', type: 'text', name: _('Emergency numbers'),
		description: _('Dialed as soon as they are complete. Digits, * and #, separated by spaces. Empty by default.') },
	{ key: 'caller_id', tab: 'calls', type: 'list', values: callerIdValues, name: _('Caller ID'),
		description: _('Experimental: sent on hook before the first ring. Not every telephone decodes it.') }
];
var byKey = {};
fields.forEach(f => { byKey[f.key] = f; });

var applyConfig = rpc.declare({
	object: 'xr500v.voip', method: 'config_apply', expect: {},
	params: ['revision', 'confirm'].concat(fields.map(f => f.key))
});

var errors = {
	line_busy: _('A port is off hook, ringing or in a call. Hang up both phones and try again.'),
	cannot_verify_idle: _('Unable to verify that both ports are idle. Nothing was changed.'),
	cannot_verify_service: _('Unable to query the telephony service. Nothing was changed.'),
	configuration_changed: _('The configuration changed since this page was loaded. Reload it and try again.'),
	confirmation_required: _('The service restart was not confirmed.'),
	invalid_volume: _('Playback level: enter a value from 25 to 100.'),
	invalid_option: _('One of the values is invalid.'),
	invalid_ring_on_ms: _('Ring on: enter a value from 300 to 10000 ms.'),
	invalid_ring_off_ms: _('Ring off: enter a value from 300 to 10000 ms.'),
	invalid_ring_prefeed_ms: _('Ring pre-feed: enter a value from 0 to 5000 ms.'),
	invalid_flash_min_ms: _('Hook flash minimum: enter a value from 40 to 1500 ms.'),
	invalid_flash_max_ms: _('Hook flash maximum: enter a value from 40 to 1500 ms.'),
	invalid_flash_window: _('The hook flash minimum must be lower than the maximum.'),
	invalid_emergency_numbers: _('Emergency numbers: use digits, * and #, separated by spaces.'),
	backup_failed: _('Unable to back up the configuration. Nothing was changed.'),
	save_failed: _('Unable to save the configuration.'),
	restart_failed: _('The configuration was saved, but telephony did not restart. Check the status page.'),
	configuration_unavailable: _('Unable to read the current configuration.'),
	unsupported_board: _('These settings are only available on the Archer XR500v.'),
	busy: _('Another change is being applied.')
};

function toForm(f, value) {
	if (f.type === 'flag') return value ? '1' : '0';
	return value == null ? '' : String(value);
}

function fromForm(f, raw) {
	if (f.type === 'flag') return raw === '1';
	if (f.type === 'number') return Number(raw);
	return raw == null ? '' : String(raw);
}

function describe(f, value) {
	if (f.type === 'flag') return value ? _('Yes') : _('No');
	if (f.type === 'list') return f.values[value] || value;
	if (f.type === 'number') return '%d %s'.format(value, f.unit);
	return value === '' ? _('None') : value;
}

return view.extend({
	load: function() { return readConfig(); },
	render: function(data) {
		this.current = data;
		this.editing = false;
		this.host = E('div', { 'id': 'voip-config-page' });
		return this.draw().then(() => this.host);
	},
	draw: function() {
		var values = {};
		fields.forEach(f => { values[f.key] = toForm(f, this.current.settings[f.key]); });
		var m = this.map = new form.JSONMap({ settings: values }, _('Telephony'),
			_('Service, audio and call settings. SIP accounts are configured on the Accounts page.'));
		m.readonly = !this.editing || !L.hasViewPermission();
		var s = m.section(form.NamedSection, 'settings', 'settings');
		s.anonymous = true;
		s.addremove = false;
		s.tab('general', _('General'));
		s.tab('audio', _('Audio'));
		s.tab('calls', _('Calls'));
		s.tab('line', _('Line feed'));
		fields.forEach(f => {
			var o = s.taboption(f.tab, f.type === 'flag' ? form.Flag : f.type === 'list' ? form.ListValue : form.Value,
				f.key, f.name, f.description);
			o.rmempty = false;
			if (f.type === 'list')
				Object.keys(f.values).forEach(v => o.value(v, f.values[v]));
			if (f.type === 'number') {
				o.datatype = 'and(uinteger,range(%d,%d))'.format(f.min, f.max);
				o.placeholder = f.unit;
			}
			if (f.key === 'flash_max_ms')
				o.validate = function(section_id, value) {
					var min = Number(this.section.formvalue(section_id, 'flash_min_ms'));
					return Number(value) > min ? true : _('Must be higher than the minimum.');
				};
			if (f.key === 'emergency_numbers')
				o.validate = function(section_id, value) {
					return /^[0-9*# ,;]*$/.test(value) && value.length < 256 ? true :
						_('Use digits, * and #, separated by spaces.');
				};
		});
		return m.render().then(node => {
			var buttons = [];
			if (this.editing) {
				buttons.push(E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-neutral', 'click': () => { this.editing = false; return this.draw(); } }, _('Cancel')));
				buttons.push(' ', E('button', { 'id': 'voip-save-apply', 'type': 'button', 'class': 'cbi-button cbi-button-apply', 'click': ui.createHandlerFn(this, 'prepareApply') }, _('Save & Apply')));
			} else if (L.hasViewPermission()) {
				buttons.push(E('button', { 'id': 'voip-edit', 'type': 'button', 'class': 'cbi-button cbi-button-action', 'click': () => { this.editing = true; return this.draw(); } }, _('Edit')));
			}
			buttons.push(' ', E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-neutral', 'click': ui.createHandlerFn(this, 'reloadConfig') }, _('Reload')));
			dom.content(this.host, [
				node,
				E('p', { 'class': 'cbi-section-descr' }, _('Applying restarts the telephony service only. Both phones must be on hook.')),
				E('div', { 'class': 'cbi-page-actions' }, buttons)
			]);
		});
	},
	reloadConfig: function() {
		return readConfig().then(data => { this.current = data; this.editing = false; return this.draw(); });
	},
	prepareApply: function() {
		return this.map.save(null, true).then(() => {
			var v = {};
			fields.forEach(f => { v[f.key] = fromForm(f, this.map.data.get('json', 'settings', f.key)); });
			var changed = fields.filter(f => v[f.key] !== this.current.settings[f.key]);
			if (!changed.length) { ui.addNotification(null, E('p', {}, _('No changes.')), 'info'); return; }
			ui.showModal(_('Apply changes'), [
				E('ul', {}, changed.map(f => E('li', {}, '%s: %s'.format(f.name, describe(f, v[f.key]))))),
				E('p', {}, _('The telephony service will restart. A call in progress would be dropped.')),
				E('div', { 'class': 'right' }, [
					E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-neutral', 'click': ui.hideModal }, _('Cancel')), ' ',
					E('button', { 'id': 'voip-confirm-apply', 'type': 'button', 'class': 'cbi-button cbi-button-apply', 'click': ui.createHandlerFn(this, 'applyValues', v) }, _('Apply'))
				])
			]);
		}).catch(() => {
			ui.addNotification(null, E('p', {}, _('Some values are invalid. Correct the highlighted fields.')), 'warning');
		});
	},
	applyValues: function(v) {
		var args = [this.current.revision, true].concat(fields.map(f => v[f.key]));
		return applyConfig.apply(null, args).then(result => {
			ui.hideModal();
			if (!result || !result.ok) {
				ui.addNotification(null, E('p', {}, errors[result?.error] || _('The changes could not be applied. Check the status page.')), 'error');
				return;
			}
			ui.addNotification(null, E('p', {}, result.changed ? _('Changes applied.') : _('No changes.')), 'info');
			this.current = result.config;
			this.editing = false;
			return this.draw();
		}).catch(() => {
			ui.hideModal();
			ui.addNotification(null, E('p', {}, _('No response from the router. Reload the page to see the current configuration.')), 'error');
		});
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
