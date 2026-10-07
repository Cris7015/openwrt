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
	account_mode: _('Account mode'), network: _('Network interface'), username: _('SIP user / telephone number'),
	domain: _('SIP domain / registrar'), auth_user: _('Authentication user'), proxy: _('Outbound proxy'),
	transport: _('SIP transport'), register: _('Register with provider'), reg_interval: _('Registration interval'),
	dtmf: _('In-call DTMF method'), dial_mode: _('Outgoing dialing'), hotline: _('Hotline SIP URI')
};
var errors = {
	line_busy: _('A port is off hook, ringing, or has active audio. Hang up both phones before applying.'),
	cannot_verify_idle: _('Unable to verify that both ports are idle. No changes were applied.'),
	cannot_verify_service: _('Unable to query the telephony service. No changes were applied.'),
	configuration_changed: _('The configuration changed. Reload before editing again.'),
	invalid_account: _('Invalid account settings. Check the field values.'),
	invalid_sip_address: _('Use a hostname or IPv4 address, optionally followed by :port. Do not include sip: or a URL path.'),
	unsupported_credentials: _('This Baresip version cannot safely read spaces, semicolons, quotes, backslashes or angle brackets in authentication fields. The credentials were not changed.'),
	provider_required: _('Provider mode requires a SIP user and domain.'),
	invalid_hotline: _('Enter a complete SIP URI, for example sip:phone2@192.168.1.1:5062.'),
	custom_keypad: _('Keypad dialing requires a managed Local or SIP provider account.'),
	custom_missing: _('The custom accounts file does not exist.'),
	shared_no_dialing: _('In Shared mode Phone 2 has no dial plan of its own: set Outgoing dialing to "Incoming calls only" and clear the hotline. Dialing is configured on Phone 1.'),
	shared_needs_phone1: _('Shared mode requires a managed account on Phone 1. Configure Phone 1 first.'),
	network_missing: _('Select an existing OpenWrt interface. Create the provider VLAN in Network first.'),
	backup_failed: _('Unable to create a private backup. No changes were applied.'),
	restart_failed: _('Settings were saved, but the restart failed. Check Status before retrying.'),
	save_failed: _('Unable to save the configuration.'),
	confirmation_required: _('Confirm the telephony restart before applying.')
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
			_('One account per FXS port. Local mode works without an ISP. SIP provider mode uses your provider credentials and an existing network interface.'));
		m.readonly = !this.editing || !L.hasViewPermission();
		var s = m.section(form.NamedSection, 'account', 'account', _('Phone %d').format(this.line));
		s.addremove = false; s.anonymous = true;
		s.tab('general', _('General')); s.tab('sip', _('SIP account')); s.tab('dialing', _('Dialing'));
		var add = (tab, kind, key, help) => {
			var o = s.taboption(tab, kind, key, labels[key] || (key === 'password' ? _('New password') : _('Clear saved password')), help);
			o.rmempty = false; return o;
		};
		var o = add('general', form.ListValue, 'account_mode', this.line === 2 ?
			_('Shared makes this jack ring and dial with the Phone 1 account: both phones ring, the first one lifted takes the call, and Phone 2 gets no SIP account or process of its own. The 1# / 2# local intercom does not exist in Shared mode.') : undefined);
		o.value('local', _('Local (no registration)')); o.value('provider', _('SIP provider'));
		if (entry.custom_available) o.value('custom', _('Custom accounts file (preserved)'));
		if (this.line === 2) o.value('shared', _('Shared with Phone 1 (rings and dials on the Phone 1 line)'));
		o = add('general', form.ListValue, 'network', _('Create VLANs and configure addressing, DNS, routes and firewall under Network. This page never changes them. If the selected interface is down, this port waits; it does not fall back to another network. IPv4 is required in this version.'));
		var seen = {};
		this.current.networks.forEach(n => { seen[n.name] = true; o.value(n.name, n.name + ' — ' + (n.up ? _('Up') : _('Down')) + (n.ipv4 ? ' (' + n.ipv4 + ')' : '')); });
		if (!seen[v.network]) o.value(v.network, v.network + ' — ' + _('Unavailable'));
		['username', 'domain', 'auth_user', 'proxy'].forEach(k => {
			var help = k === 'domain' ? _('Hostname or IPv4, optionally :port. This is the SIP address domain and registration target. Use Outbound proxy when the provider supplies a separate next-hop server.') :
				k === 'auth_user' ? _('Optional; defaults to the SIP user.') : k === 'proxy' ? _('Optional hostname or IPv4, optionally :port. No sip: prefix.') : undefined;
			o = add('sip', form.Value, k, help); o.depends('account_mode', 'provider');
			o.optional = k === 'auth_user' || k === 'proxy';
		});
		o = add('sip', form.Value, 'password', entry.password_set ? _('A password is stored. Leave blank to keep it. The saved password is never sent to this page.') : _('No managed password is stored.'));
		o.password = true; o.optional = true; o.depends('account_mode', 'provider');
		o = add('sip', form.Flag, 'clear_password'); o.depends('account_mode', 'provider');
		o = add('sip', form.ListValue, 'transport', _('UDP and TCP are not encrypted. TLS/SRTP and automatic NAT traversal are not configured by this version.'));
		o.value('udp', 'UDP'); o.value('tcp', 'TCP'); o.depends('account_mode', 'provider');
		o = add('sip', form.Flag, 'register'); o.depends('account_mode', 'provider');
		o = add('sip', form.Value, 'reg_interval', _('Seconds (60–3600).')); o.datatype = 'and(uinteger,range(60,3600))'; o.depends('account_mode', 'provider');
		o = add('sip', form.ListValue, 'dtmf', _('Baresip signaling preference. Analog keypad detection during an established call is not implemented yet; this does not promise IVR support.'));
		o.value('rtpevent', _('RTP events')); o.value('info', 'SIP INFO'); o.value('auto', _('Automatic')); o.depends('account_mode', 'provider');
		o = add('dialing', form.ListValue, 'dial_mode', _('Keypad: lift the handset, dial, then press # to send (or wait 4 seconds). In Local mode, 1# calls Phone 1 and 2# calls Phone 2. Hardware validation is required. This is not a replacement for a tested emergency telephone service.'));
		o.value('none', _('Incoming calls only')); o.value('keypad', _('Telephone keypad')); o.value('hotline', _('Fixed destination on off-hook'));
		['local', 'provider', 'custom'].forEach(mode => o.depends('account_mode', mode));
		o = add('dialing', form.Value, 'hotline', _('Example: sip:phone2@192.168.1.1:5062')); o.depends('dial_mode', 'hotline');
		return m.render().then(node => {
			var selector = E('select', { id: 'voip-account-line', disabled: this.editing ? '' : null, change: ev => { this.line = Number(ev.target.value); return this.draw(); } },
				[1, 2].map(n => E('option', { value: n, selected: this.line === n ? '' : null }, _('Phone %d').format(n))));
			var buttons = [];
			if (this.editing) {
				buttons.push(E('button', { type: 'button', class: 'cbi-button cbi-button-neutral', click: () => { this.editing = false; return this.draw(); } }, _('Cancel editing')), ' ',
					E('button', { id: 'voip-account-save', type: 'button', class: 'cbi-button cbi-button-apply', click: ui.createHandlerFn(this, 'prepareApply') }, _('Save & Apply')));
			} else if (L.hasViewPermission()) buttons.push(E('button', { id: 'voip-account-edit', type: 'button', class: 'cbi-button cbi-button-action', click: () => { this.editing = true; return this.draw(); } }, _('Edit')));
			buttons.push(' ', E('button', { type: 'button', class: 'cbi-button cbi-button-neutral', click: () => this.reload() }, _('Reload')));
			dom.content(this.host, [E('p', { id: 'voip-account-mode' }, this.editing ? _('Edit mode. Nothing is applied until you confirm.') : _('Read-only mode. Click Edit to change the values.')),
				E('label', {}, [_('Port'), ' ', selector]), node,
				E('p', {}, _('Custom account files are kept untouched. Switching to a managed profile changes which account is used. Network events may restart telephony when its address changes.')),
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
			ui.showModal(_('Apply account settings'), [
				E('p', {}, _('Save this port and restart telephony? Hang up both phones first. A private backup is made, and applying is blocked while a port is busy. A call arriving during the change may be interrupted.')),
				E('p', {}, _('No VLAN, route or firewall changes will be made. Provider registration requires a working provider network.')),
				E('div', { class: 'right' }, [E('button', { type: 'button', class: 'cbi-button cbi-button-neutral', click: ui.hideModal }, _('Cancel')), ' ',
					E('button', { id: 'voip-account-confirm', type: 'button', class: 'cbi-button cbi-button-apply', click: ui.createHandlerFn(this, 'applyValues', values) }, _('Confirm & Apply'))])
			]);
		});
	},
	applyValues: function(values) {
		return applyAccount(this.current.revision, this.line, true, values).then(result => {
			ui.hideModal();
			if (!result?.ok) { ui.addNotification(null, E('p', {}, errors[result?.error] || _('The operation failed. Reload and check Status before retrying.')), 'error'); return; }
			ui.addNotification(null, E('p', {}, result.changed ? _('Account saved. Check Status; a running process does not prove SIP registration.') : _('No changes; telephony was not restarted.')), 'info');
			this.current = result.config; this.editing = false; return this.draw();
		}).catch(() => { ui.hideModal(); ui.addNotification(null, E('p', {}, _('Response lost. The outcome is uncertain; reload and check Status before retrying.')), 'error'); });
	},
	handleSave: null, handleSaveApply: null, handleReset: null
});
