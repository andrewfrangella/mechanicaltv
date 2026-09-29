// Optional end-to-end browser check. Playwright is a test dependency only.
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const { spawn, execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert/strict');

(async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mechanical-tv-browser-'));
  const fixture = path.join(root, 'pattern.mp4');
  execFileSync('ffmpeg', ['-loglevel','error','-f','lavfi','-i','testsrc2=size=320x250:rate=10','-t','5','-c:v','libx264','-pix_fmt','yuv420p',fixture]);
  const source = `
import json, sys
from pathlib import Path
from mechanical_tv.server import Application, Server, Handler, password_hash
root = Path(sys.argv[1])
salt = 'b2' * 16
(root / 'auth.json').write_text(json.dumps({'salt':salt, 'hash':password_hash('browser-test-password', salt)}))
app = Application(root)
server = Server(('127.0.0.1', 0), Handler)
server.app = app
print('READY:' + str(server.server_port), flush=True)
server.serve_forever()
`;
  const server = spawn('python3', ['-u','-c',source,root], {cwd:path.join(__dirname,'..')});
  let browser;
  try {
    const port = await new Promise((resolve,reject) => {
      let text='';
      const timeout=setTimeout(() => reject(new Error('Server startup timeout')),10000);
      server.stdout.on('data', data => { text+=data;const match=text.match(/READY:(\d+)/);if(match){clearTimeout(timeout);resolve(Number(match[1]));} });
      server.once('exit', code => {clearTimeout(timeout);reject(new Error(`Server exited: ${code}`));});
      server.stderr.on('data',data => process.stderr.write(data));
    });
    browser = await chromium.launch({headless:true, ...(process.env.CHROMIUM_PATH ? {executablePath:process.env.CHROMIUM_PATH} : {})});
    const page=await browser.newPage({viewport:{width:1280,height:960}});
    const errors=[];page.on('pageerror', error => errors.push(error.message));
    await page.goto(`http://127.0.0.1:${port}`);
    await page.locator('#password').fill('browser-test-password');
    await page.getByRole('button',{name:'Open studio'}).click();
    await page.locator('#studio').waitFor({state:'visible'});
    await page.getByRole('button',{name:'02 / Library'}).click();
    await page.locator('#file').setInputFiles(fixture);
    await page.getByRole('button',{name:'Upload & prepare'}).click();
    await page.waitForFunction(() => document.querySelector('.library-item p')?.textContent.startsWith('READY'),null,{timeout:30000});
    await page.getByRole('button',{name:'Select',exact:true}).click();
    await page.locator('#play').click();
    await page.waitForFunction(() => document.querySelector('#play-state').textContent === 'PLAYING');
    await page.waitForFunction(() => document.querySelector('#position').textContent === '0:01');
    await page.locator('#pause').click();
    await page.waitForFunction(() => document.querySelector('#play-state').textContent === 'PAUSED');
    const current=await page.locator('#position').textContent();
    await page.waitForTimeout(1200);
    assert.equal(await page.locator('#position').textContent(),current);
    await page.screenshot({path:path.join(root,'desktop.png'),fullPage:true});
    await page.reload();
    await page.waitForFunction(() => document.querySelector('#play-state').textContent === 'PAUSED');
    await page.setViewportSize({width:390,height:844});
    await page.screenshot({path:path.join(root,'mobile.png'),fullPage:true});
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth),false,'Mobile layout overflows');
    await page.locator('#stop').click();
    await page.waitForFunction(() => document.querySelector('#play-state').textContent === 'STOPPED');
    await page.getByRole('button',{name:'03 / System'}).click();
    await page.getByText('Conversion worker',{exact:true}).waitFor();
    await page.locator('#logout').click();
    await page.locator('#login').waitFor({state:'visible'});
    assert.deepEqual(errors,[]);
    console.log(`Browser smoke passed. Screenshots: ${root}`);
  } finally {
    if(browser) await browser.close();
    server.kill('SIGTERM');
  }
})().catch(error => {console.error(error);process.exitCode=1;});
