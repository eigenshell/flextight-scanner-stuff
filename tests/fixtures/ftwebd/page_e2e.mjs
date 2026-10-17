// SPDX-License-Identifier: GPL-2.0-or-later
// The page end to end in a headless Chromium (DevTools protocol over
// node's WebSocket; node >= 22): a user's taps against ftwebd + the fake
// ftpoc, the downloads saved to a folder. tests/test_page.py checks them.
//
//   node page_e2e.mjs <chromium> <http://127.0.0.1:port/> <download dir>
//
// Prints one JSON line: {strip, landed, notes, print, log}.
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, existsSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [chrome, url, dl] = process.argv.slice(2);
const prof = mkdtempSync(join(tmpdir(), "ftweb-e2e-"));
const proc = spawn(chrome, ["--headless=new", "--remote-debugging-port=0",
  "--user-data-dir=" + prof, "--no-first-run", "--no-default-browser-check",
  "--disable-gpu", "--window-size=1000,900", "about:blank"], { stdio: "ignore" });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let ws, seq = 0;
const pending = new Map(), events = [];

async function connect() {
  const f = join(prof, "DevToolsActivePort");
  for (let i = 0; i < 200 && !existsSync(f); i++) await sleep(50);
  const [port, path] = readFileSync(f, "utf8").split("\n");
  ws = new WebSocket("ws://127.0.0.1:" + port + path);
  await new Promise((y, n) => { ws.onopen = y; ws.onerror = n; });
  ws.onmessage = m => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) {
      const [y, n] = pending.get(d.id);
      pending.delete(d.id);
      d.error ? n(new Error(JSON.stringify(d.error))) : y(d.result);
    } else events.push(d);
  };
}
const cdp = (method, params = {}, sessionId) => new Promise((y, n) => {
  pending.set(++seq, [y, n]);
  ws.send(JSON.stringify({ id: seq, method, params, sessionId }));
});

let sid;
async function ev(expr) {
  const r = await cdp("Runtime.evaluate", { expression: expr, awaitPromise: true,
                                            returnByValue: true }, sid);
  if (r.exceptionDetails) throw new Error(expr + ": " + JSON.stringify(r.exceptionDetails));
  return r.result.value;
}
async function until(expr, what, ms = 60000) {
  for (let t = 0; t < ms; t += 100) {
    const v = await ev(expr);
    if (v) return v;
    await sleep(100);
  }
  const st = await ev('document.getElementById("st").textContent');
  const log = await ev('document.getElementById("log").textContent');
  throw new Error("timed out: " + what + " (status: " + st + ")\n" + log);
}

try {
  await connect();
  const { targetId } = await cdp("Target.createTarget", { url: "about:blank" });
  ({ sessionId: sid } = await cdp("Target.attachToTarget", { targetId, flatten: true }));
  await cdp("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: dl,
                                             eventsEnabled: true });
  await cdp("Runtime.enable", {}, sid);
  await cdp("Page.enable", {}, sid);
  await cdp("Page.navigate", { url }, sid);
  await until('document.readyState === "complete" && typeof cut === "function"', "page");
  await until('document.getElementById("em").options.length > 0', "emulsions");
  // a new strip, previewed
  await ev('document.getElementById("new").click()');
  await until('document.getElementById("strip").value !== ""', "strip");
  const strip = await ev('document.getElementById("strip").value');
  await ev('document.getElementById("preview").click()');
  await until('document.querySelectorAll(".fr").length === 6', "frame boxes");
  // frame 3 -> 6MP, frame 4 -> ARC, frame 5 -> skip (each tap: next)
  const tap = (n, k) => ev('(()=>{const b=[...document.querySelectorAll(".fr")]' +
    '.find(b=>b.querySelector(".n").textContent==="' + n + '");' +
    'for(let i=0;i<' + k + ';i++)b.click()})()');
  await tap(3, 1);
  await tap(4, 2);
  await tap(5, 3);
  await until('[...document.querySelectorAll(".fr .s")].map(s=>s.textContent).join() === "4MP,4MP,6MP,ARC,—,—"', "buckets");
  await until('/window/.test(document.getElementById("plan").textContent)', "plan");
  await ev('document.getElementById("scan").click()');
  await until('/ftwebd exit 0/.test(document.getElementById("log").textContent)', "scan");
  await until('document.querySelectorAll("#grid canvas").length >= 2 && document.getElementById("big").width > 0', "prints");
  await ev('(async()=>{await landing})()');
  // clicks on the selected frame (the first): c c M (shift: minus)
  for (const key of ["c", "c", "M"])
    await ev('dispatchEvent(new KeyboardEvent("keydown",{key:"' + key + '"}))');
  await until('[...document.querySelectorAll("#keys b")].map(b=>b.textContent).join() === "+2,-1,0,0"', "clicks");
  // a quarter turn (r); the large view turns with it
  const big = 'document.getElementById("big")';
  const was = await ev('[' + big + '.width,' + big + '.height]');
  await ev('dispatchEvent(new KeyboardEvent("keydown",{key:"r"}))');
  await until(big + '.width === ' + was[1] + ' && ' + big + '.height === ' + was[0], "turned");
  // zoom in (z): one image pixel per device pixel, then a tap: fit again
  await ev('dispatchEvent(new KeyboardEvent("keydown",{key:"z"}))');
  await until(big + '.width > ' + was[1] + ' && document.getElementById("bw").className === "z"', "zoomed");
  await ev(big + '.click()');
  await until(big + '.width === ' + was[1] + ' && document.getElementById("bw").className === ""', "unzoomed");
  // the strip preview zooms
  await ev('document.getElementById("zi").click()');
  await until('document.getElementById("sv").style.minWidth === "960px"', "strip zoomed");
  await until('fetch(S("strip.print")).then(r=>r.text()).then(t=>/cmyd = 2 -1 0 0\\nrotate = 90\\n/.test(t))', "strip.print saved");
  await ev('document.getElementById("dl").click()');
  await ev('document.getElementById("all").click()');
  const want = [strip + "_frame01.jpg", strip + "_frame01.jpg.txt", strip + ".zip"];
  for (let t = 0; t < 600 && !want.every(f => existsSync(join(dl, f))); t++) await sleep(100);
  await sleep(300);
  const out = await ev('(async()=>({landed, notes: document.getElementById("notes").textContent,' +
    ' print: await fetch(S("strip.print")).then(r=>r.text()),' +
    ' log: document.getElementById("log").textContent}))()');
  console.log(JSON.stringify(Object.assign({ strip }, out)));
} catch (e) {
  console.error(String(e && e.stack || e));
  process.exitCode = 1;
} finally {
  try { ws && ws.close(); } catch (e) {}
  proc.kill();
  await sleep(200);
  rmSync(prof, { recursive: true, force: true });
}
