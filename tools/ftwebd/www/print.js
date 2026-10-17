// SPDX-License-Identifier: GPL-2.0-or-later
// The page's print stage (index.html loads it deferred: nothing here
// is needed before frames land): cut each finished window in the
// engine, the strip's balance, the grid and the large view, C/M/Y/D
// clicks (buttons, keys c m y d, Shift: minus, arrows: frames) and the
// downloads; each frame's turn (buttons, keys r R); the large view's zoom
// (tap, z) and the strip preview's (- +). Shares index.html's globals
// ($, ft, api, cur, ...).
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
// the large view fits the screen, or (zoomed: tap it, z) shows the print
// at one image pixel per device pixel
let zoomed=0;
const bigEdge=()=>zoomed?2e4:Math.min(1600,Math.round(innerWidth*devicePixelRatio));
async function show(f,big){const d=await ft("display",f,big?bigEdge():320),c=big?$("big"):$("t"+f);
 if(!c)return;c.width=d.w;c.height=d.h;c.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(d.rgba.buffer),d.w,d.h),0,0);
 if(big){c.style.width=zoomed?d.w/devicePixelRatio+"px":"";const t=$("t"+f),s=Math.min(1,320/Math.max(d.w,d.h));
  if(t){t.width=Math.round(d.w*s);t.height=Math.round(d.h*s);t.getContext("2d").drawImage(c,0,0,t.width,t.height)}}shown.set(f,1)}
async function zoom(e){if(!sel)return;const c=$("big"),r=c.getBoundingClientRect(),m=e instanceof MouseEvent&&r.width,
 fx=m?(e.clientX-r.left)/r.width:.5,fy=m?(e.clientY-r.top)/r.height:.5,b=$("bw");
 zoomed=!zoomed;b.className=zoomed?"z":"";if(zoomed)st("zooming …");await show(sel,1);st("");
 if(zoomed){b.scrollLeft=fx*b.scrollWidth-b.clientWidth/2;b.scrollTop=fy*b.scrollHeight-b.clientHeight/2}}
$("big").onclick=zoom;
function draw(k){$("keys").innerHTML="";$("dl").disabled=$("all").disabled=!k;if(!k){const c=$("big");c.width=c.height=0;return}
 "CMYD".split("").forEach((n,i)=>{const s=document.createElement("span");s.className="k";
  s.innerHTML=n+' <button>−</button><b>'+(k[i]>0?"+":"")+k[i]+'</b><button>+</button>';
  const b=s.querySelectorAll("button");b[0].onclick=()=>click(i,-1);b[1].onclick=()=>click(i,1);$("keys").append(s)});
 const t=document.createElement("span");t.className="k";
 t.innerHTML='<button title="turn left (Shift+R)">↺</button><button title="turn right (R)">↻</button><button title="every frame turned as this one">↻ all</button>';
 const b=t.querySelectorAll("button");b[0].onclick=()=>turn(-90);b[1].onclick=()=>turn(90);b[2].onclick=turnAll;$("keys").append(t)}
async function pick(f){sel=f;for(const c of $("grid").children)c.className=c.id==="t"+f?"sel":"";kk[f]=await ft("printGet",f);draw(kk[f]);await show(f,1)}
// one click after another (fast keys must not read stale clicks)
let clickQ=Promise.resolve();
const click=(i,d)=>clickQ=clickQ.then(async()=>{if(!sel)return;const f=sel,k=kk[f].slice();k[i]+=d;
 try{await ft("clicks",f,k);kk[f]=k;if(f===sel){draw(k);await show(f,1)}savePrint()}catch(e){st(e.message,1)}});
// the print's turn (strip.print rotate): the screen copies, the JPEGs
const turn=d=>clickQ=clickQ.then(async()=>{if(!sel)return;const f=sel;
 try{await ft("rotate",f,((await ft("rotation",f))+d+360)%360);savePrint();await show(f,f===sel)}catch(e){st(e.message,1)}});
const turnAll=()=>clickQ=clickQ.then(async()=>{if(!sel)return;
 try{const r=await ft("rotation",sel);for(const f of landed)if(f!==sel&&await ft("rotation",f)!==r){await ft("rotate",f,r);await show(f,0)}savePrint()}catch(e){st(e.message,1)}});
function savePrint(){clearTimeout(putT);putT=setTimeout(async()=>{try{await api(S("strip.print"),{method:"PUT",body:await ft("printText")})}catch(e){st(e.message,1)}},400)}
function save(name,bytes,type){const a=document.createElement("a");a.href=URL.createObjectURL(new Blob([bytes],{type}));a.download=name;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(a.href),6e4)}
$("em").onchange=async()=>{const n=await ft("printEmulsion",$("em").value);if(n)st(n+" frame(s) back to auto: their clicks do not fit");savePrint();await reprint()};
$("dl").onclick=async()=>{try{st("printing frame "+sel+" …");const r=await ft("jpeg",sel),n=cur+"_frame"+String(sel).padStart(2,"0")+".jpg";save(n,r.jpg,"image/jpeg");save(n+".txt",r.text,"text/plain");st("")}catch(e){st(e.message,1)}};
$("all").onclick=async()=>{try{st("printing every frame …");save(cur+".zip",await ft("zip",cur),"application/zip");st("")}catch(e){st(e.message,1)}};
addEventListener("keydown",e=>{if(/INPUT|SELECT/.test(e.target.tagName)||!sel||e.ctrlKey||e.metaKey||e.altKey)return;const i="cmyd".indexOf(e.key.toLowerCase()),j=landed.indexOf(sel);
 if(i>=0)click(i,e.key===e.key.toLowerCase()?1:-1);else if(e.key==="r"||e.key==="R")turn(e.key==="r"?90:-90);else if(e.key==="z")zoom(e);else if(e.key==="ArrowRight"&&j<landed.length-1)pick(landed[j+1]);else if(e.key==="ArrowLeft"&&j>0)pick(landed[j-1]);else return;e.preventDefault()});
let landing=Promise.resolve();
const land=n=>landing=landing.then(async()=>{await cut(n);await reprint()});
// the strip preview's zoom (the frame boxes are in %: they follow)
const ZS=[1,1.5,2,3,4];let sz=+ls("zoom")||1;
function szoom(d){const b=$("sv").parentNode,c=(b.scrollLeft+b.clientWidth/2)/b.scrollWidth;
 sz=ZS[Math.max(0,Math.min(ZS.length-1,ZS.indexOf(sz)+d))];ls("zoom",""+sz);
 Object.assign($("sv").style,{width:sz>1?"calc("+sz*100+"% - 20px)":"",minWidth:640*sz+"px"});
 b.scrollLeft=c*b.scrollWidth-b.clientWidth/2;$("zo").disabled=sz===1;$("zi").disabled=sz===ZS[ZS.length-1]}
$("zo").onclick=()=>szoom(-1);$("zi").onclick=()=>szoom(1);szoom(0);
