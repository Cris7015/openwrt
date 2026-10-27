// Lab: UI fixtures for the panel's Playwright checks (not installed).
// Run with the Playwright MCP tool browser_run_code_unsafe (filename = this
// file): it answers the page's xr500v.gpon ubus calls with generated data,
// relative to "now", and forwards every other call to the router. Pick the
// scenario with a code call (each call has its own globals, the page object
// is shared): page.xgFixture = 'normal' | 'empty' | 'flapping' | 'patterns'
// | 'nofiber'; then reload the page. Nothing is written on the router.
async (page) => {
	const HOUR = 3600, MIN = 60;

	function rxAt(t, dip) {
		return (t >= dip && t < dip + 180) ? 4000 : Math.round(8300 + 150 * Math.sin(t / 5000));
	}

	function tempAt(t) {
		return Math.round(50500 + 1500 * Math.sin(t / 20000));
	}

	function point(t, step, f) {
		const rx = f.rx(t), temp = tempAt(t);

		return { t, n: step / 60, rx_min: rx ? rx - (step > 60 ? 100 : 0) : rx, rx_avg: rx,
			rx_max: rx ? rx + (step > 60 ? 80 : 0) : rx, temp_min: temp - (step > 60 ? 300 : 0),
			temp_avg: temp, temp_max: temp + (step > 60 ? 300 : 0), bip: f.bip(t, step) };
	}

	function ring(now, step, count, f) {
		const out = [];

		for (let i = count - 1; i >= 0; i--)
			out.push(point(now - 20 - i * step - ((now - 20) % step) * (step > 60 ? 1 : 0), step, f));
		return out;
	}

	function build(name, now) {
		const dip = now - 6 * HOUR;
		let events = [], f = {
			rx: (t) => rxAt(t, dip),
			bip: (t, step) => (t <= dip && dip < t + step) ? 12 : (t <= now - 30 * HOUR && now - 30 * HOUR < t + step) ? 15 : 0
		};
		const summary = {
			now, stale: false, health: 'ok', alerts: [], ever_o5: true, state: 5, o5_since: now - 25 * HOUR,
			olt: { vendor: 'HWTC', version: '10', profile: 'huawei' }, onu_id: 1, fec: { ds: 2, us: 2 },
			optics: { rx_nw: 8300, tx_nw: 1480000, temp_mc: 51000, bias_ua: 17100, volt_uv: 3259000, alarms: 0 },
			counters: { bip_total: 27, bip_session: 12, gem_delin: 15, deactivate: 1 }, serial_hint: 'TEST…'
		};

		events = [
			{ t: now - 2 * HOUR, type: 'resync', severity: 'warn', msg: 'Grant FIFO resync' },
			{ t: dip + 180, type: 'o5_up', severity: 'info', msg: 'Link operational (O5)' },
			{ t: dip + 60, type: 'o5_down', severity: 'crit', msg: 'Link lost: left O5', win: true },
			{ t: dip, type: 'deactivate', severity: 'crit', msg: 'OLT sent Deactivate_ONU-ID; GPON restarting', win: true },
			{ t: now - 30 * HOUR, type: 'bip_burst', severity: 'warn', msg: '15 BIP errors in one minute', n: 15 }
		];

		if (name == 'empty')
			return { summary: { now, stale: true, health: 'unknown', alerts: [], ever_o5: false, optics: {}, counters: {}, olt: {} },
				history: { '24h': { window: '24h', step: 60, points: [] }, '7d': { window: '7d', step: 300, points: [] },
					'30d': { window: '30d', step: 1800, points: [] } },
				events: [], around: (t) => ({ t0: t - 1800, t1: t + 1800, step: 60, source: 'none', points: [], events: [] }) };

		if (name == 'nofiber') {
			Object.assign(summary, { health: 'nofiber', ever_o5: false, state: 1, o5_since: null,
				optics: { rx_nw: 0, tx_nw: 0, temp_mc: 45000, bias_ua: 0, volt_uv: 3259000, alarms: 0 },
				counters: { bip_total: 0, bip_session: 0, gem_delin: 0, deactivate: 0 }, olt: {} });
			f = { rx: () => 0, bip: () => 0 };
			events = [];
		}

		if (name == 'flapping') {
			events = [];
			for (let i = 0; i < 300; i++)
				events.push({ t: now - 20 - i * 288, type: (i % 2) ? 'o5_down' : 'o5_up',
					severity: (i % 2) ? 'crit' : 'info', msg: 'x' });
		}

		if (name == 'patterns') {
			f.bip = (t, step) => (new Date(t * 1000).getHours() == 3) ? 6 : 0;
			for (let d = 1; d < 30; d += 4) {
				const x = new Date((now - d * 86400) * 1000);
				x.setHours(3, 20, 0, 0);
				events.push({ t: Math.floor(x.getTime() / 1000), type: 'deactivate', severity: 'crit', msg: 'x' });
			}
			events.sort((a, b) => b.t - a.t);
		}

		return {
			summary, events,
			history: { '24h': { window: '24h', step: 60, points: ring(now, 60, 1440, f) },
				'7d': { window: '7d', step: 300, points: ring(now, 300, 2016, f) },
				'30d': { window: '30d', step: 1800, points: ring(now, 1800, 1440, f) } },
			around: (t) => {
				const pts = [];
				for (let x = t - 1800; x <= t + 1800; x += MIN)
					pts.push(point(x, 60, f));
				return { t0: t - 1800, t1: t + 1800, step: 60, source: 'saved', points: pts,
					events: events.filter((e) => e.t >= t - 1800 && e.t <= t + 1800) };
			}
		};
	}

	await page.unroute('**/ubus/**').catch(() => {});
	await page.route('**/ubus/**', async (route) => {
		let body;

		try { body = JSON.parse(route.request().postData() || 'null'); } catch (e) { return route.continue(); }
		const calls = Array.isArray(body) ? body : [ body ];
		const ours = calls.filter((c) => c && c.params && c.params[1] === 'xr500v.gpon');

		if (!ours.length)
			return route.continue();

		const fx = build(page.xgFixture || 'normal', Math.floor(Date.now() / 1000));
		const answer = (c) => {
			const m = c.params[2], a = c.params[3] || {};
			const d = (m == 'summary') ? fx.summary
				: (m == 'history') ? (fx.history[a.window] || fx.history['24h'])
				: (m == 'events') ? { events: fx.events.slice(0, a.limit || fx.events.length) }
				: (m == 'around') ? fx.around(a.t) : {};
			return { jsonrpc: '2.0', id: c.id, result: [ 0, d ] };
		};
		let out;

		if (ours.length == calls.length) {
			out = calls.map(answer);
		} else {
			const real = await (await route.fetch()).json();
			out = (Array.isArray(real) ? real : [ real ]).map((r) => {
				const c = ours.find((x) => x.id === r.id);
				return c ? answer(c) : r;
			});
		}
		await route.fulfill({ status: 200, contentType: 'application/json',
			body: JSON.stringify(Array.isArray(body) ? out : out[0]) });
	});
	return 'fixture: ' + (page.xgFixture || 'normal');
}
