'use strict';
'require view';
'require form';
'require rpc';
'require ui';
'require dom';

var readConfig = rpc.declare({ object: 'xr500v.voip', method: 'config_get', expect: {} });
var applyConfig = rpc.declare({
	object: 'xr500v.voip', method: 'config_apply', expect: {},
	params: ['revision', 'confirm', 'service_enabled', 'phone1_enabled', 'phone2_enabled',
		'echo_cancel', 'noise_suppress', 'playback_percent', 'phone1_idle_feed', 'phone2_idle_feed']
});
var keys = ['service_enabled', 'phone1_enabled', 'phone2_enabled', 'echo_cancel', 'noise_suppress', 'playback_percent',
	'phone1_idle_feed', 'phone2_idle_feed'];
var feeds = ['phone1_idle_feed', 'phone2_idle_feed'];
var names = {
	service_enabled: _('Enable telephony'), phone1_enabled: _('Enable Phone 1'), phone2_enabled: _('Enable Phone 2'),
	echo_cancel: _('Echo cancellation'), noise_suppress: _('Mild noise suppression'), playback_percent: _('Playback level'),
	phone1_idle_feed: _('Phone 1 idle line feed'), phone2_idle_feed: _('Phone 2 idle line feed')
};
var feedNames = { standby: _('Standby (silent, recommended)'), active: _('Active (instant dial tone, audible hiss)') };
var errors = {
	line_busy: _('A port is off hook, ringing, or has active audio. Hang up both phones before applying.'),
	cannot_verify_idle: _('Unable to verify that both ports are idle. No changes were applied.'),
	cannot_verify_service: _('Unable to query the telephony service. No changes were applied.'),
	configuration_changed: _('The configuration changed since this page was loaded. Reload it before editing.'),
	confirmation_required: _('You must confirm the telephony service restart.'),
	invalid_volume: _('The playback level must be an integer from 25 to 100.'),
	invalid_option: _('One of the options has an invalid value.'),
	backup_failed: _('Unable to create a private backup. No changes were applied.'),
	save_failed: _('Unable to save the configuration.'),
	restart_failed: _('The configuration was saved, but the service restart failed. Check Status before retrying.'),
	configuration_unavailable: _('Unable to read the original configuration.'),
	unsupported_board: _('These settings are only available on the XR500v.'),
	busy: _('Another configuration update is already in progress.')
};

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
		keys.forEach(k => {
			values[k] = k === 'playback_percent' ? String(this.current.settings[k]) :
				feeds.includes(k) ? String(this.current.settings[k] || 'standby') :
				this.current.settings[k] ? '1' : '0';
		});
		var m = this.map = new form.JSONMap({ settings: values }, _('Telephony configuration'),
			_('Persistent local service settings. SIP accounts and electrical SLIC controls are not modified by this page.'));
		m.readonly = !this.editing || !L.hasViewPermission();
		var s = m.section(form.NamedSection, 'settings', 'settings', _('Service and audio'));
		s.anonymous = true;
		s.addremove = false;
		s.tab('general', _('General'));
		s.tab('audio', _('Audio'));
		s.tab('line', _('Line feed'));
		keys.forEach(k => {
			var audio = ['echo_cancel', 'noise_suppress', 'playback_percent'].includes(k);
			var tab = feeds.includes(k) ? 'line' : audio ? 'audio' : 'general';
			var o = s.taboption(tab, k === 'playback_percent' ? form.Value :
				feeds.includes(k) ? form.ListValue : form.Flag, k, names[k]);
			o.rmempty = false;
			if (feeds.includes(k)) {
				o.value('standby', feedNames.standby); o.value('active', feedNames.active);
				o.description = _('Standby parks the line in low power while the handset is on hook: silent, and the dial tone needs the few milliseconds the line takes to come up. Active keeps the line powered, so the dial tone is immediate, but the router\'s DC-DC converter hiss is audible in the earpiece while idle.');
			} else if (k === 'playback_percent') {
				o.datatype = 'and(uinteger,range(25,100))';
				o.description = _('Digital playback level (25–100%). The validated baseline uses 50%.');
			} else if (k === 'echo_cancel') o.description = _('Audio echo cancellation; does not change electrical settings.');
			else if (k === 'noise_suppress') o.description = _('Uses the mild noise suppression already included in the service.');
		});
		return m.render().then(node => {
			var buttons = [];
			if (this.editing) {
				buttons.push(E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-neutral', 'click': () => { this.editing = false; return this.draw(); } }, _('Cancel editing')));
				buttons.push(' ', E('button', { 'id': 'voip-save-apply', 'type': 'button', 'class': 'cbi-button cbi-button-apply', 'click': ui.createHandlerFn(this, 'prepareApply') }, _('Save & Apply')));
			} else if (L.hasViewPermission()) {
				buttons.push(E('button', { 'id': 'voip-edit', 'type': 'button', 'class': 'cbi-button cbi-button-action', 'click': () => { this.editing = true; return this.draw(); } }, _('Edit')));
			}
			buttons.push(' ', E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-neutral', 'click': ui.createHandlerFn(this, 'reloadConfig') }, _('Reload')));
			dom.content(this.host, [
				E('p', { 'id': 'voip-config-mode' }, this.editing ? _('Edit mode. Changes are not applied until you confirm.') : _('Read-only mode. Click Edit to change the values.')),
				node,
				E('p', {}, _('Applying changes restarts only telephony. Port state is checked and a private backup is saved first. A call arriving during the change may be interrupted.')),
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
			keys.forEach(k => {
				var raw = this.map.data.get('json', 'settings', k);
				v[k] = k === 'playback_percent' ? Number(raw) : feeds.includes(k) ? String(raw) : raw === '1';
			});
			var changed = keys.filter(k => v[k] !== this.current.settings[k]);
			if (!changed.length) { ui.addNotification(null, E('p', {}, _('No changes. No service was restarted.')), 'info'); return; }
			ui.showModal(_('Apply telephony configuration'), [
				E('p', {}, _('These changes will be saved and only the telephony service will restart:')),
				E('ul', {}, changed.map(k => E('li', {}, names[k] + ': ' + (k === 'playback_percent' ? v[k] + ' %' :
					feeds.includes(k) ? feedNames[v[k]] : v[k] ? _('Yes') : _('No'))))),
				E('p', {}, _('Hang up both phones. Applying is blocked if a port is busy or its state cannot be verified. A call arriving during the operation may be interrupted.')),
				E('div', { 'class': 'right' }, [
					E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-neutral', 'click': ui.hideModal }, _('Cancel')), ' ',
					E('button', { 'id': 'voip-confirm-apply', 'type': 'button', 'class': 'cbi-button cbi-button-apply', 'click': ui.createHandlerFn(this, 'applyValues', v) }, _('Confirm & Apply'))
				])
			]);
		});
	},
	applyValues: function(v) {
		return applyConfig(this.current.revision, true, v.service_enabled, v.phone1_enabled, v.phone2_enabled,
			v.echo_cancel, v.noise_suppress, v.playback_percent, v.phone1_idle_feed, v.phone2_idle_feed).then(result => {
			ui.hideModal();
			if (!result || !result.ok) {
				ui.addNotification(null, E('p', {}, errors[result?.error] || _('The operation could not be completed. Check Status before retrying.')), 'error');
				return;
			}
			ui.addNotification(null, E('p', {}, result.changed ? _('Configuration saved. Check Status to verify the service after the restart.') : _('No changes; telephony was not restarted.')), 'info');
			this.current = result.config;
			this.editing = false;
			return this.draw();
		}).catch(() => {
			ui.hideModal();
			ui.addNotification(null, E('p', {}, _('The response was lost, so the outcome is uncertain. Check Status and reload the configuration before trying again.')), 'error');
		});
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
