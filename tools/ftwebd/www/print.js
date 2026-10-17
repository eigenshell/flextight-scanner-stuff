// SPDX-License-Identifier: GPL-2.0-or-later
// The page's print stage (index.html loads it deferred: nothing here
// is needed before frames land): cut each finished window in the
// engine, the strip's balance, the grid and the large view, C/M/Y/D
// clicks (buttons, keys c m y d, Shift: minus, arrows: frames) and the
// downloads. Shares index.html's globals ($, ft, api, cur, ...).
"use strict";
// a finished window: its files -> the engine's cut
async function cut(name,quiet){const s=cur,[raw,cal,meta,faul]=await Promise.all([get(F(name),1),get(F(name+".cal"),1),get(F(name+".txt"),1),get(F(name+".faul"),1)]);
 if(s!==cur)return;if(!raw||!cal||!meta)return quiet||st(name+": files missing",1);
 try{const r=await ft("cut",raw,cal,meta,faul);for(const f of r.frames)if(!landed.includes(f.frame))landed.push(f.frame);landed.sort((a,b)=>a-b);if(!sel)sel=landed[0]}
 catch(e){if(!quiet)st(name+": "+e.message,1)}}
// the strip's balance over every landed frame, then the screen copies
async function reprint(){if(!landed.length)return;try{$("notes").textContent=await ft("measure")}catch(e){return st(e.message,1)}
 shown.clear();const g=$("grid");g.innerHTML="";for(const f of landed){const c=document.createElement("canvas");c.id="t"+f;c.title="frame "+f;c.onclick=()=>pick(f);g.append(c)}
 await pick(sel||landed[0]);for(const f of landed)if(!shown.has(f))await show(f,0)}
async function show(f,big){const d=await ft("display",f,big?Math.min(1600,Math.round(innerWidth*devicePixelRatio)):320),c=big?$("big"):$("t"+f);
 if(!c)return;c.width=d.w;c.height=d.h;c.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(d.rgba.buffer),d.w,d.h),0,0);
 if(big){const t=$("t"+f);if(t){t.width=d.w;t.height=d.h;t.getContext("2d").drawImage(c,0,0)}}shown.set(f,1)}
function draw(k){$("keys").innerHTML="";$("dl").disabled=$("all").disabled=!k;if(!k){const c=$("big");c.width=c.height=0;return}
 "CMYD".split("").forEach((n,i)=>{const s=document.createElement("span");s.className="k";
  s.innerHTML=n+' <button>−</button><b>'+(k[i]>0?"+":"")+k[i]+'</b><button>+</button>';
  const b=s.querySelectorAll("button");b[0].onclick=()=>click(i,-1);b[1].onclick=()=>click(i,1);$("keys").append(s)})}
async function pick(f){sel=f;for(const c of $("grid").children)c.className=c.id==="t"+f?"sel":"";kk[f]=await ft("printGet",f);draw(kk[f]);await show(f,1)}
// one click after another (fast keys must not read stale clicks)
let clickQ=Promise.resolve();
const click=(i,d)=>clickQ=clickQ.then(async()=>{if(!sel)return;const f=sel,k=kk[f].slice();k[i]+=d;
 try{await ft("clicks",f,k);kk[f]=k;if(f===sel){draw(k);await show(f,1)}savePrint()}catch(e){st(e.message,1)}});
function savePrint(){clearTimeout(putT);putT=setTimeout(async()=>{try{await api(S("strip.print"),{method:"PUT",body:await ft("printText")})}catch(e){st(e.message,1)}},400)}
function save(name,bytes,type){const a=document.createElement("a");a.href=URL.createObjectURL(new Blob([bytes],{type}));a.download=name;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(a.href),6e4)}
$("em").onchange=async()=>{const n=await ft("printEmulsion",$("em").value);if(n)st(n+" frame(s) back to auto: their clicks do not fit");savePrint();await reprint()};
$("dl").onclick=async()=>{try{st("printing frame "+sel+" …");const r=await ft("jpeg",sel),n=cur+"_frame"+String(sel).padStart(2,"0")+".jpg";save(n,r.jpg,"image/jpeg");save(n+".txt",r.text,"text/plain");st("")}catch(e){st(e.message,1)}};
$("all").onclick=async()=>{try{st("printing every frame …");save(cur+".zip",await ft("zip",cur),"application/zip");st("")}catch(e){st(e.message,1)}};
addEventListener("keydown",e=>{if(/INPUT|SELECT/.test(e.target.tagName)||!sel||e.ctrlKey||e.metaKey||e.altKey)return;const i="cmyd".indexOf(e.key.toLowerCase()),j=landed.indexOf(sel);
 if(i>=0)click(i,e.key===e.key.toLowerCase()?1:-1);else if(e.key==="ArrowRight"&&j<landed.length-1)pick(landed[j+1]);else if(e.key==="ArrowLeft"&&j>0)pick(landed[j-1]);else return;e.preventDefault()});
let landing=Promise.resolve();
const land=n=>landing=landing.then(async()=>{await cut(n);await reprint()});
