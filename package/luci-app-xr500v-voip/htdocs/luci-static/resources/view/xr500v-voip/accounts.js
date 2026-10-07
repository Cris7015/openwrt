'use strict';
'require view';
'require form';
'require rpc';
'require ui';
'require dom';

var getAccounts = rpc.declare({ object: 'xr500v.voip', method: 'accounts_get', expect: {} });
var applyAccount = rpc.declare({ object: 'xr500v.voip', method: 'accounts_apply',
	params: ['revision', 'line', 'confirm', 'values'], expect: {} });
var labels = {
	account_mode: _('Mode'), network: _('Interface'), username: _('User'),
	domain: _('Domain'), auth_user: _('Authentication user'), proxy: _('Outbound proxy'),
	transport: _('Transport'), register: _('Register'), reg_interval: _('Registration interval'),
	dtmf: _('DTMF'), dial_mode: _('Dialing'), hotline: _('Hotline URI')
};
var errors = {
	line_busy: _('A port is off hook, ringing or in a call. Hang up both phones and try again.'),
	cannot_verify_idle: _('Unable to verify that both ports are idle. Nothing was changed.'),
	cannot_verify_service: _('Unable to query the telephony service. Nothing was changed.'),
	configuration_changed: _('The configuration changed since this page was loaded. Reload it and try again.'),
	invalid_account: _('One of the values is invalid.'),
	invalid_sip_address: _('Enter a hostname or IPv4 address, optionally with :port, without sip:.'),
	unsupported_credentials: _('Spaces, semicolons, quotes, backslashes and angle brackets are not supported in credentials. Nothing was changed.'),
	provider_required: _('A SIP provider account needs a user and a domain.'),
	invalid_hotline: _('Enter a full SIP URI, for example sip:phone2@192.168.1.1:5062.'),
	custom_keypad: _('Keypad dialing needs a Local or SIP provider account.'),
	custom_missing: _('The custom accounts file does not exist.'),
	shared_no_dialing: _('A shared port dials through Phone 1: set Dialing to Incoming only.'),
	shared_needs_phone1: _('Configure an account on Phone 1 first.'),
	network_missing: _('Select an existing interface.'),
	backup_failed: _('Unable to back up the configuration. Nothing was changed.'),
	restart_failed: _('The configuration was saved, but telephony did not restart. Check the status page.'),
	save_failed: _('Unable to save the configuration.'),
	confirmation_required: _('The service restart was not confirmed.')
};

return view.extend({
	load: function() { return getAccounts(); },
	render: function(data) {
		this.current = data; this.line = 1; this.editing = false;
		this.host = E('div', { id: 'voip-accounts-page' });
		return this.draw().then(() => this.host);
	},
	draw: function() {
		var entry = this.current.lines[this.line - 1];
		var v = Object.assign({}, entry.values, { password: '', clear_password: '0' });
		var m = this.map = new form.JSONMap({ account: v }, _('Telephony accounts'),
			_('One account per phone port. Local works without a provider; SIP provider registers with your operator over the selected interface.'));
		m.readonly = !this.editing || !L.hasViewPermission();
		var s = m.section(form.NamedSection, 'account', 'account', _('Phone %d').format(this.line));
		s.addremove = false; s.anonymous = true;
		s.tab('general', _('General')); s.tab('sip', _('SIP account')); s.tab('dialing', _('Dialing'));
		var add = (tab, kind, key, help) => {
			var o = s.taboption(tab, kind, key, labels[key] || (key === 'password' ? _('Password') : _('Remove the stored password')), help);
			o.rmempty = false; return o;
		};
		var o = add('general', form.ListValue, 'account_mode', this.line === 2 ?
			_('Shared: both phones ring and dial on the Phone 1 account, and the first one lifted answers. The 1#/2# intercom is not available.') : undefined);
		o.value('local', _('Local')); o.value('provider', _('SIP provider'));
		if (entry.custom_available) o.value('custom', _('Custom accounts file'));
		if (this.line === 2) o.value('shared', _('Shared with Phone 1'));
		o = add('general', form.ListValue, 'network', _('Interface used for SIP and audio, configured under Network (IPv4). If it is down, the port waits for it.'));
		var seen = {};
		this.current.networks.forEach(n => { seen[n.name] = true; o.value(n.name, n.name + ' — ' + (n.up ? _('Up') : _('Down')) + (n.ipv4 ? ' (' + n.ipv4 + ')' : '')); });
		if (!seen[v.network]) o.value(v.network, v.network + ' — ' + _('Unavailable'));
		['username', 'domain', 'auth_user', 'proxy'].forEach(k => {
			var help = k === 'domain' ? _('Registrar hostname or IPv4 address, optionally with :port.') :
				k === 'auth_user' ? _('Optional. Defaults to the user.') : k === 'proxy' ? _('Optional. Hostname or IPv4 address, optionally with :port.') : undefined;
			o = add('sip', form.Value, k, help); o.depends('account_mode', 'provider');
			o.optional = k === 'auth_user' || k === 'proxy';
		});
		o = add('sip', form.Value, 'password', entry.password_set ? _('A password is stored. Leave empty to keep it.') : _('No password is stored.'));
		o.password = true; o.optional = true; o.depends('account_mode', 'provider');
		o = add('sip', form.Flag, 'clear_password'); o.depends('account_mode', 'provider');
		o = add('sip', form.ListValue, 'transport', _('Unencrypted. TLS is not supported.'));
		o.value('udp', 'UDP'); o.value('tcp', 'TCP'); o.depends('account_mode', 'provider');
		o = add('sip', form.Flag, 'register'); o.depends('account_mode', 'provider');
		o = add('sip', form.Value, 'reg_interval', _('60–3600 s.')); o.datatype = 'and(uinteger,range(60,3600))'; o.depends('account_mode', 'provider');
		o = add('sip', form.ListValue, 'dtmf', _('How keys pressed during a call are sent.'));
		o.value('rtpevent', _('RTP events (RFC 4733)')); o.value('info', 'SIP INFO'); o.value('auto', _('Automatic')); o.depends('account_mode', 'provider');
		o = add('dialing', form.ListValue, 'dial_mode', _('Keypad: dial, then press # or wait 4 s. In Local mode, 1# calls Phone 1 and 2# calls Phone 2. Not a certified emergency telephone service.'));
		o.value('none', _('Incoming only')); o.value('keypad', _('Keypad')); o.value('hotline', _('Hotline (on off-hook)'));
		['local', 'provider', 'custom'].forEach(mode => o.depends('account_mode', mode));
		o = add('dialing', form.Value, 'hotline', _('Example: sip:phone2@192.168.1.1:5062')); o.depends('dial_mode', 'hotline');
		return m.render().then(node => {
			var selector = E('select', { id: 'voip-account-line', disabled: this.editing ? '' : null, change: ev => { this.line = Number(ev.target.value); return this.draw(); } },
				[1, 2].map(n => E('option', { value: n, selected: this.line === n ? '' : null }, _('Phone %d').format(n))));
			var buttons = [];
			if (this.editing) {
				buttons.push(E('button', { type: 'button', class: 'cbi-button cbi-button-neutral', click: () => { this.editing = false; return this.draw(); } }, _('Cancel')), ' ',
					E('button', { id: 'voip-account-save', type: 'button', class: 'cbi-button cbi-button-apply', click: ui.createHandlerFn(this, 'prepareApply') }, _('Save & Apply')));
			} else if (L.hasViewPermission()) buttons.push(E('button', { id: 'voip-account-edit', type: 'button', class: 'cbi-button cbi-button-action', click: () => { this.editing = true; return this.draw(); } }, _('Edit')));
			buttons.push(' ', E('button', { type: 'button', class: 'cbi-button cbi-button-neutral', click: () => this.reload() }, _('Reload')));
			dom.content(this.host, [E('label', {}, [_('Port'), ' ', selector]), node,
				E('p', {}, _('Custom accounts files are never modified.')),
				E('div', { class: 'cbi-page-actions' }, buttons)]);
		});
	},
	reload: function() { return getAccounts().then(data => { this.current = data; this.editing = false; return this.draw(); }); },
	prepareApply: function() {
		return this.map.save(null, true).then(() => {
			var values = {};
			// Preserve inactive fields when LuCI suppresses their widgets.
			Object.keys(labels).forEach(k => { values[k] = this.map.data.get('json', 'account', k) ?? this.current.lines[this.line - 1].values[k]; });
			// Shared ports are dialed through Phone 1; never submit a dial
			// plan the service would have to reject.
			if (values.account_mode === 'shared') { values.dial_mode = 'none'; values.hotline = ''; }
			values.password = this.map.data.get('json', 'account', 'password') || '';
			values.clear_password = this.map.data.get('json', 'account', 'clear_password') === '1';
			ui.showModal(_('Apply changes'), [
				E('p', {}, _('The telephony service will restart. A call in progress would be dropped.')),
				E('div', { class: 'right' }, [E('button', { type: 'button', class: 'cbi-button cbi-button-neutral', click: ui.hideModal }, _('Cancel')), ' ',
					E('button', { id: 'voip-account-confirm', type: 'button', class: 'cbi-button cbi-button-apply', click: ui.createHandlerFn(this, 'applyValues', values) }, _('Apply'))])
			]);
		});
	},
	applyValues: function(values) {
		return applyAccount(this.current.revision, this.line, true, values).then(result => {
			ui.hideModal();
			if (!result?.ok) { ui.addNotification(null, E('p', {}, errors[result?.error] || _('The changes could not be applied. Check the status page.')), 'error'); return; }
			ui.addNotification(null, E('p', {}, result.changed ? _('Changes applied. The status page shows the registration.') : _('No changes.')), 'info');
			this.current = result.config; this.editing = false; return this.draw();
		}).catch(() => { ui.hideModal(); ui.addNotification(null, E('p', {}, _('No response from the router. Reload the page to see the current configuration.')), 'error'); });
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
