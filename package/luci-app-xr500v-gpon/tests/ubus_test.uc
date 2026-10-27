// Integration test against the deployed recorder: LuCI reaches it through
// rpcd, which adds ubus_rpc_session to every call. Deploy first:
// scripts/gpon-panel-deploy.sh <pkgdir>
// scripts/gpon-panel-test.sh <pkgdir> tests/ubus_test.uc
'use strict';

import { ok, eq, done } from 'assert';
import * as ubus from 'ubus';

const SID = '00000000000000000000000000000000';

let bus = ubus.connect();

function call(method, args) {
	let r = bus.call('xr500v.gpon', method, args);
	return { r, err: r == null ? bus.error() : null };
}

// ubus_methods_accept_rpc_session: the calls the LuCI views make.
let c = call('summary', { ubus_rpc_session: SID });
eq('ubus_summary_with_session_err', c.err, null);
ok('ubus_summary_with_session_state', c.r?.now > 0);

c = call('history', { window: '24h', ubus_rpc_session: SID });
eq('ubus_history_with_session_err', c.err, null);
ok('ubus_history_with_session_points', type(c.r?.points) == 'array');

c = call('events', { limit: 5, ubus_rpc_session: SID });
eq('ubus_events_with_session_err', c.err, null);
ok('ubus_events_with_session_list', type(c.r?.events) == 'array');

c = call('around', { t: time() - 60, ubus_rpc_session: SID });
eq('ubus_around_with_session_err', c.err, null);
ok('ubus_around_with_session_points', type(c.r?.points) == 'array');

// ubus_methods_without_session: the CLI and the init script.
eq('ubus_summary_plain_err', call('summary').err, null);

done();
