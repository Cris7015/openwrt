'use strict';
'require baseclass';

/*
 * Pure helpers behind the panel's analysis views: the cursor tooltip, the
 * day x hour map and the CSV export. No DOM and no String.format, so the
 * node tests can load this file with stubs (tests/js/run.js). Times are
 * shown in the browser's local time.
 */

var TIP_EVENTS = 3;
var CSV_COLUMNS = 'rx_dbm_avg,rx_dbm_min,rx_dbm_max,temp_c_avg,temp_c_min,temp_c_max,bip,events';
/* not inline after `return`: LuCI's jsmin would take the slash for a division */
var CSV_QUOTE = /[",\n]/;

function pad(n) {
	return (n < 10 ? '0' : '') + n;
}

function dbm(nw) {
	return (nw > 0) ? 10 * Math.log10(nw / 1e6) : null;
}

function celsius(mc) {
	return (mc != null) ? mc / 1000 : null;
}

function triple(p, key, conv) {
	var avg = conv(p[key + '_avg']);

	return (avg != null) ? { avg: avg, min: conv(p[key + '_min']), max: conv(p[key + '_max']) } : null;
}

function inBucket(events, t0, step) {
	return (events || []).filter(function(e) { return e.t >= t0 && e.t < t0 + step; });
}

function dayKey(d) {
	return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate());
}

/* "UTC-03:00": the browser's offset at time t */
function utcOffset(t) {
	var m = -new Date(t * 1000).getTimezoneOffset(), a = Math.abs(m);

	return 'UTC' + (m < 0 ? '-' : '+') + pad(Math.floor(a / 60)) + ':' + pad(a % 60);
}

function csvCell(v) {
	var s = (v == null) ? '' : String(v);

	return CSV_QUOTE.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
}

function fixed1(v) {
	return (v != null) ? v.toFixed(1) : '';
}

return baseclass.extend({
	dbm: dbm,

	/* [{ t, n, avg, min, max }] in display units; points without data skipped */
	series: function(points, field) {
		var out = [];

		(points || []).forEach(function(p) {
			var v;

			if (field == 'rx')
				v = triple(p, 'rx', dbm);
			else if (field == 'temp')
				v = triple(p, 'temp', celsius);
			else
				v = (p.bip != null) ? { avg: p.bip, min: p.bip, max: p.bip } : null;

			if (v)
				out.push({ t: p.t, n: p.n || 1, avg: v.avg, min: v.min, max: v.max });
		});

		return out;
	},

	inWindow: function(data, t0, t1) {
		return (data || []).filter(function(d) { return d.t >= t0 && d.t < t1; });
	},

	/* index of the entry closest in time to t (data sorted by t), -1 if none */
	nearest: function(data, t) {
		var lo = 0, hi = (data ? data.length : 0) - 1;

		if (hi < 0)
			return -1;
		while (hi - lo > 1) {
			var mid = (lo + hi) >> 1;

			if (data[mid].t <= t)
				lo = mid;
			else
				hi = mid;
		}
		return (Math.abs(data[hi].t - t) < Math.abs(data[lo].t - t)) ? hi : lo;
	},

	/* index of the last entry at or before t (the first one if t is earlier),
	 * -1 if none: the point whose interval [p.t, p.t + step) holds t */
	floor: function(data, t) {
		var lo = 0, hi = (data ? data.length : 0) - 1;

		if (hi < 0)
			return -1;
		if (data[hi].t <= t)
			return hi;
		while (hi - lo > 1) {
			var mid = (lo + hi) >> 1;

			if (data[mid].t <= t)
				lo = mid;
			else
				hi = mid;
		}
		return lo;
	},

	/* What the cursor at t points at: the point whose interval holds t, its
	 * values, and the events of that interval or within `slack` seconds of
	 * the cursor (a marker is 8 px wide). null without points. */
	tooltip: function(points, events, t, step, slack) {
		var i = this.floor(points, t), p, evs;

		if (i < 0)
			return null;
		p = points[i];
		slack = slack || 0;
		evs = (events || []).filter(function(e) {
			return (e.t >= p.t && e.t < p.t + step) || Math.abs(e.t - t) <= slack;
		});
		return {
			t: p.t,
			rx: triple(p, 'rx', dbm),
			temp: triple(p, 'temp', celsius),
			bip: (p.bip != null) ? p.bip : null,
			events: evs.slice(0, TIP_EVENTS),
			more: Math.max(0, evs.length - TIP_EVENTS)
		};
	},

	/* The event in the viewer's language; the recorder's own text otherwise */
	eventText: function(ev) {
		switch (ev.type) {
		case 'o5_up':      return _('Link operational (O5)');
		case 'o5_down':    return _('Link lost: left O5');
		case 'deactivate': return _('The OLT sent Deactivate_ONU-ID; the GPON link restarted');
		case 'rx_crit':    return _('Received power below the critical threshold');
		case 'rx_warn':    return _('Received power below the warning threshold');
		case 'rx_high':    return _('Received power above the overload warning');
		case 'temp_warn':  return _('Laser temperature above the warning threshold');
		case 'resync':     return _('Grant FIFO resync');
		case 'o6':         return _('Signal lost for an instant (O6); waiting up to 100 ms for the OLT');
		case 'phy_shift':
			return (ev.diff != null)
				? _('Back in O5 without new ranging; the PHY delay moved %d units of 3.2 ns').replace('%d', ev.diff)
				: _('Back in O5 without new ranging');
		case 'eqd_adjust':
			return (ev.delta != null)
				? _('The OLT adjusted the equalization delay by %d bits').replace('%d', ev.delta)
				: _('The OLT adjusted the equalization delay');
		case 'to2':        return _('No Popup within 100 ms (TO2); the GPON link restarted');
		case 'wan_auth':   return _('The ISP rejected the PPPoE login: the credentials may have changed');
		case 'wan_no_pado': return _('No PPPoE server answered: the VLAN or the service may have changed');
		case 'sn_stuck':   return _('Stuck before O5 with light on the fiber: the OLT may have dropped this serial');
		case 'sip_fail':
			return (ev.n) ? _('SIP registration keeps failing (%d): the account may have changed').replace('%d', ev.n)
				: _('SIP registration keeps failing: the account may have changed');
		case 'bip_burst':
			return (ev.n != null) ? _('%d BIP errors in one minute').replace('%d', ev.n) : _('BIP error burst');
		case 'bosa_alarm':
			return (ev.flags != null)
				? _('Optical module alarm (flags 0x%s)').replace('%s', pad(ev.flags.toString(16)))
				: _('Optical module alarm');
		}
		return ev.msg || ev.type;
	},

	/* HH:MM, or YYYY-MM-DD HH:MM, in the browser's time zone */
	localTime: function(t, withDate) {
		var d = new Date(t * 1000), hm = pad(d.getHours()) + ':' + pad(d.getMinutes());

		return withDate ? dayKey(d) + ' ' + hm : hm;
	},

	/* Local times with their UTC offset in the header, or in each cell when
	 * the rows span a DST change. With t0/t1, only the points the graphs
	 * draw: after downtime the rings still hold older ones. */
	csv: function(points, events, step, t0, t1) {
		var self = this, rows, offsets, mixed;

		points = (t0 != null) ? this.inWindow(points, t0, t1 + 1) : (points || []);
		offsets = points.map(function(p) { return utcOffset(p.t); });
		mixed = offsets.some(function(o) { return o != offsets[0]; });
		rows = [ (mixed ? 'time (local)' : 'time (' + (offsets[0] || utcOffset(Date.now() / 1000)) + ')') + ',' + CSV_COLUMNS ];

		points.forEach(function(p, i) {
			var rx = triple(p, 'rx', dbm) || {}, temp = triple(p, 'temp', celsius) || {};

			rows.push([
				self.localTime(p.t, true) + (mixed ? ' ' + offsets[i] : ''),
				fixed1(rx.avg), fixed1(rx.min), fixed1(rx.max),
				fixed1(temp.avg), fixed1(temp.min), fixed1(temp.max),
				(p.bip != null) ? p.bip : '',
				inBucket(events, p.t, step).map(function(e) { return e.type; }).join(';')
			].map(csvCell).join(','));
		});

		return rows.join('\n') + '\n';
	},

	/* The last `days` local days up to now's, oldest first, 24 hours each:
	 * BIP errors and events per severity; seen = the hour has data. */
	heatmap: function(points, events, now, days) {
		var names = [ _('Sun'), _('Mon'), _('Tue'), _('Wed'), _('Thu'), _('Fri'), _('Sat') ],
		    end = new Date(now * 1000), out = [], index = {};

		for (var i = 0; i < days; i++) {
			var d = new Date(end.getFullYear(), end.getMonth(), end.getDate() - (days - 1 - i)),
			    hours = [];

			for (var h = 0; h < 24; h++)
				hours.push({ bip: 0, crit: 0, warn: 0, info: 0, seen: false });
			index[dayKey(d)] = out.length;
			out.push({ day: dayKey(d), label: names[d.getDay()] + ' ' + d.getDate(), hours: hours });
		}

		function cell(t) {
			var d = new Date(t * 1000), k = index[dayKey(d)];

			return (k != null) ? out[k].hours[d.getHours()] : null;
		}

		(points || []).forEach(function(p) {
			var c = cell(p.t);

			if (c) {
				c.seen = true;
				c.bip += p.bip || 0;
			}
		});
		(events || []).forEach(function(e) {
			var c = cell(e.t);

			if (c && (e.severity == 'crit' || e.severity == 'warn' || e.severity == 'info'))
				c[e.severity]++;
		});

		return out;
	}
});
