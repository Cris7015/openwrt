'use strict';
'require baseclass';
'require rpc';

var object = 'omci.agent';

function declare(method, params, expect, reject) {
	return rpc.declare({
		object: object,
		method: method,
		params: params || [],
		expect: expect || {},
		reject: !!reject
	});
}

/* A call that changes something: without reject a failed call resolves like a
 * successful one, and the page tells the user it worked. */
function change(method, params) {
	return declare(method, params, {}, true);
}

return baseclass.extend({
	family: declare('family', [], { family: {} }),
	status: declare('status', [ 'device' ], { status: {} }),
	agentSet: change('agent_set', [ 'device', 'enabled', 'permissive', 'fake_omci', 'dying_gasp' ]),
	configList: declare('config_list', [ 'device' ], { config: {} }),
	configDetails: declare('config_details', [ 'device' ], { config: {} }),
	configGet: declare('config_get', [ 'device', 'key' ], { config: {} }),
	configInspect: declare('config_inspect', [ 'device', 'key' ], { config: {} }),
	configSet: change('config_set', [ 'device', 'key', 'value' ]),
	classList: declare('class_list', [ 'device' ], { classes: [] }),
	classGet: declare('class_get', [ 'device', 'class_id' ], { class: {} }),
	mibList: declare('mib_list', [ 'device' ], { objects: [] }),
	mibGet: declare('mib_get', [ 'device', 'class_id', 'entity_id' ], { object: {} }),
	mibSet: change('mib_set', [ 'device', 'class_id', 'entity_id', 'attr_mask', 'data' ]),
	mibDelete: change('mib_delete', [ 'device', 'class_id', 'entity_id' ]),
	mibReset: change('mib_reset', [ 'device' ]),
	rawTx: change('raw_tx', [ 'device', 'pdu' ]),
	apply: change('apply', [ 'device' ]),
	profileList: declare('profile_list', [], { profiles: [] }),
	profileApply: change('profile_apply', [ 'device', 'profile', 'serial', 'password' ])
});
