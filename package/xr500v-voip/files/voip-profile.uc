#!/usr/bin/ucode
'use strict';
import { readfile, writefile, chmod, stat, unlink } from 'fs';
import { cursor } from 'uci';
import { connect } from 'ubus';
import { account_defaults, account_validate, account_render } from '/usr/share/xr500v-voip/accounts.uc';

// Invoked after the legacy renderer. Managed profiles are explicit opt-in;
// pre-existing custom files remain in place and can be selected again.
let line = ARGV[0];
if (line != '1' && line != '2') exit(2);
let c = cursor(), section = 'phone' + line;
let mode = c.get('xr500v-voip', section, 'account_mode');
if (!mode) exit(0);
let a = {};
for (let k, v in account_defaults) a[k] = c.get('xr500v-voip', section, k) ?? v;
a.password = c.get('xr500v-voip', section, 'password') || '';
let error = account_validate(a, int(line));
if (error) { warn('voip: invalid account profile (' + error + ')\n'); exit(2); }
// Shared ring group: phone2 has no account, no baresip and no manager. The
// init script must not start an instance for it -- exit 4 is the explicit
// "line intentionally not started" contract (defence in depth: voip.init
// already skips this line before calling us).
if (a.account_mode == 'shared') {
	if (line != '2') { warn('voip: shared mode is only valid on phone2\n'); exit(2); }
	if (!c.get('xr500v-voip', 'phone1', 'account_mode')) {
		warn('voip: phone2 shared mode ignored, phone1 has no managed account\n');
		exit(2);
	}
	exit(4);
}
let bus = connect();
let n = bus?.call('network.interface.' + a.network, 'status', {});
let ip = n?.['ipv4-address']?.[0]?.address;
let dev = n?.l3_device;
if (!n?.up || !ip || !dev || !match(ip, /^[0-9.]+$/) || !match(dev, /^[a-zA-Z0-9_.:@-]{1,15}$/)) {
	warn('voip: selected interface is not ready (IPv4 required)\n'); exit(3);
}
const dir = '/var/run/xr500v-voip/' + section;
function put(name, text) {
	if (writefile(dir + '/' + name, text) != length(text) || !chmod(dir + '/' + name, 384)) die('runtime_write_failed');
}
let base = readfile(dir + '/config');
if (base == null) exit(2);
// Bind SIP and SDP to the selected address. Routing/DNS/firewall are owned
// by netifd, never synthesized here. Never fall back to another interface.
base = join('\n', filter(split(base, '\n'), (row) => !match(row, /^[ \t]*(sip_listen|net_interface|sip_transports)[ \t]+/)));
base += sprintf('\nsip_listen %s:%d\nnet_interface %s\nsip_transports %s\n', ip, 5058 + int(line) * 2, ip, a.transport);
put('config', base);
if (a.account_mode != 'custom') put('accounts', account_render(a, int(line), ip));
else if (!stat('/etc/xr500v-voip/' + section + '/accounts')) { warn('voip: custom accounts file missing\n'); exit(2); }
if (a.dial_mode == 'hotline') put('speeddial', a.hotline + '\n');
else unlink(dir + '/speeddial');
// Three-line bounded input, not shell source. A collector cannot dial until
// this profile explicitly enables keypad mode. Local shortcuts: 1# and 2#.
put('dialing', a.dial_mode + '\n' + a.account_mode + '\n' + (a.account_mode == 'local' ? ip : a.domain) + '\n');
