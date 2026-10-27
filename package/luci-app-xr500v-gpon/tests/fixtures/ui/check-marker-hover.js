// Lab check (with route.js installed): hovers the centre of every event
// marker, on the 24 h view and on the view around the Deactivate, and counts
// how many show their event in the box.
async (page) => {
  page.xgFixture = 'normal';
  await page.goto('http://192.168.68.222/cgi-bin/luci/admin/network/omci/fiber');
  await page.evaluate(async () => { const v = L.env.resource_version; for (const p of ['view/xr500v-gpon/fiber.js', 'xr500v-gpon/graph.js', 'xr500v-gpon/analysis.js']) await fetch('/luci-static/resources/' + p + '?v=' + v, { cache: 'reload' }); });
  await page.reload();
  await page.click('.cbi-tabmenu li[data-tab="graphs"] a');
  await page.waitForSelector('#view svg polyline', { timeout: 20000 });
  const out = [];
  for (const view of [ '24h', 'zoom' ]) {
    if (view == 'zoom') {
      await page.click('.cbi-tabmenu li[data-tab="events"] a');
      const rows = await page.$$('#view div[data-tab="events"] tr.tr');
      for (const r of rows) { if (/Deactivate/.test(await r.innerText())) { await (await r.$('button')).click(); break; } }
      await page.waitForTimeout(1500);
    }
    const svg = (await page.$$('#view div[data-tab="graphs"] svg'))[0];
    await svg.scrollIntoViewIfNeeded();
    const box = await svg.boundingBox();
    const xs = await svg.evaluate(s => [...s.querySelectorAll('rect[width="8"]')].map(r => +r.getAttribute('x') + 4));
    let hits = 0;
    for (const x of xs) {
      await page.mouse.move(box.x + x, box.y + 4);
      await page.waitForTimeout(60);
      const txt = await page.evaluate(() => [...document.querySelectorAll('#view div')].find(d => d.style.position === 'fixed')?.innerText || '');
      if (/●/.test(txt)) hits++;
    }
    out.push(view + ': ' + hits + '/' + xs.length + ' marcas muestran su evento');
  }
  return out;
}
