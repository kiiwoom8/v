// run.cjs <url> [steps-json] - opens url in a headful Chromium with
// accessibility on (Xvfb, AT-SPI; see run.sh), runs the steps, then prints
// dump.py's tree. Steps: {"click": "<selector>"}, {"wait": ms},
// {"eval": "<script>"}, {"dump": ""} (an extra dump in between).
const { chromium } = require('playwright');
const { execSync } = require('child_process');
const py = process.env.AX_PYTHON || 'python3';
const dump = (extra) => execSync(`${py} ${__dirname}/dump.py ${extra || ''}`).toString();

(async () => {
  const url = process.argv[2];
  const steps = process.argv[3] ? JSON.parse(process.argv[3]) : [];
  const browser = await chromium.launch({ headless: false,
    args: ['--force-renderer-accessibility', '--window-position=0,0',
           '--window-size=1400,900', '--site-per-process'] });
  const page = await browser.newPage({ viewport: null });
  await page.goto(url);
  await page.waitForTimeout(800);
  for (const s of steps) {
    if (s.click) await page.click(s.click);
    if (s.wait) await page.waitForTimeout(s.wait);
    if (s.eval) await page.evaluate(s.eval);
    if (s.dump !== undefined) console.log(dump(s.dump));
  }
  console.log(dump(process.env.AX_DUMP_ARGS));
  await browser.close();
})().catch(e => { console.error(e); process.exit(1); });
