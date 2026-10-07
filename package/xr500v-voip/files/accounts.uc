// Shared by rpcd and the runtime renderer. Pure validation/serialization;
// no file, process, network or hardware access here.
export const account_defaults = {
	account_mode: 'local', network: 'lan', username: '', domain: '',
	auth_user: '', proxy: '', transport: 'udp', register: '1',
	reg_interval: '300', dtmf: 'rtpevent', dial_mode: 'none', hotline: ''
};

export function sip_host(s) {
	if (type(s) != 'string' || !length(s) || length(s) > 253 || index(s, '\x00') >= 0 || match(s, /[[:space:][:cntrl:]]|[^a-zA-Z0-9.:-]/)) return false;
	let p = split(s, ':');
	if (length(p) > 2) return false;
	if (length(p) == 2 && (!match(p[1], /^[0-9]{1,5}$/) || int(p[1]) < 1 || int(p[1]) > 65535)) return false;
	if (match(p[0], /^[0-9.]+$/)) {
		let octets = split(p[0], '.');
		return length(octets) == 4 && length(filter(octets, (v) => match(v, /^[0-9]{1,3}$/) && int(v, 10) <= 255)) == 4;
	}
	for (let label in split(p[0], '.'))
		if (!match(label, /^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$/)) return false;
	return true;
}

export function sip_user(s) {
	return type(s) == 'string' && length(s) > 0 && length(s) <= 96 && index(s, '\x00') < 0 && !match(s, /[[:space:][:cntrl:]]|[^a-zA-Z0-9_.+*~-]/);
}

export function sip_uri(s) {
	if (type(s) != 'string' || length(s) > 360 || index(s, '\x00') >= 0 || match(s, /[[:space:]]/)) return false;
	let m = match(s, /^sip:([^@]+)@([^;]+)(;transport=(udp|tcp))?$/);
	return m != null && sip_user(m[1]) && sip_host(m[2]);
}

// `line` is REQUIRED to accept 'shared': the shared ring group is a property of
// the SECOND jack only (phone1 keeps the account, phone2 joins its bell). A
// legacy one-argument call therefore never validates 'shared'.
export function account_validate(a, line) {
	for (let key, def in account_defaults)
		if (type(a[key]) != 'string' || length(a[key]) > 360 || index(a[key], '\x00') >= 0) return 'invalid_account';
	if (index(line == 2 ? ['local', 'provider', 'custom', 'shared'] : ['local', 'provider', 'custom'], a.account_mode) < 0 ||
	    !length(a.network) || length(a.network) > 32 || match(a.network, /[[:space:][:cntrl:]]|[^a-zA-Z0-9_]/) ||
	    index(['none', 'keypad', 'hotline'], a.dial_mode) < 0 ||
	    index(['udp', 'tcp'], a.transport) < 0 || index(['0', '1'], a.register) < 0 ||
	    index(['rtpevent', 'info', 'auto'], a.dtmf) < 0 ||
	    match(a.reg_interval, /[[:space:][:cntrl:]]|[^0-9]/) || length(a.reg_interval) > 4 || int(a.reg_interval) < 60 || int(a.reg_interval) > 3600)
		return 'invalid_account';
	if ((a.username && !sip_user(a.username)) || (a.domain && !sip_host(a.domain)) ||
	    (a.proxy && !sip_host(a.proxy))) return 'invalid_sip_address';
	// libre 3.16 msg_param_decode does not unquote/unescape auth_pass.
	// Reject unsupported delimiters instead of silently changing a secret.
	for (let value in [a.auth_user, a.password || ''])
		if (type(value) != 'string' || length(value) > 128 || index(value, '\x00') >= 0 || match(value, /[[:space:][:cntrl:]]|[^!-~]|[;"\\<>]/)) return 'unsupported_credentials';
	if (a.account_mode == 'provider' && (!a.username || !a.domain)) return 'provider_required';
	if (a.hotline && !sip_uri(a.hotline)) return 'invalid_hotline';
	if (a.dial_mode == 'hotline' && !a.hotline) return 'invalid_hotline';
	if (a.account_mode == 'custom' && a.dial_mode == 'keypad') return 'custom_keypad';
	// In shared mode phone2 has no account, no baresip and no dial plan of its
	// own: everything is dialed through phone1. Refuse a setting that would
	// silently do nothing instead of pretending it was applied.
	if (a.account_mode == 'shared' && (a.dial_mode != 'none' || a.hotline)) return 'shared_no_dialing';
	return null;
}

export function account_render(a, line, ip) {
	let uri = a.account_mode == 'local' ? sprintf('sip:phone%d@%s:%d', line, ip, 5058 + line * 2) :
		'sip:' + a.username + '@' + a.domain + ';transport=' + a.transport;
	let result = '<' + uri + '>;answermode=manual;audio_codecs=PCMU,PCMA;regint=' +
		(a.account_mode == 'local' || a.register == '0' ? '0' : a.reg_interval);
	if (a.account_mode == 'provider') {
		if (a.auth_user) result += ';auth_user=' + a.auth_user;
		if (a.password) result += ';auth_pass=' + a.password;
		if (a.proxy) result += ';outbound="sip:' + a.proxy + ';transport=' + a.transport + '"';
		result += ';dtmfmode=' + a.dtmf;
	}
	return result + '\n';
}
