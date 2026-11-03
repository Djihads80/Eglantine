// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Read-only web dashboard served at GET / by the MCP server: watch what an agent is doing
// (screen, status, live timeline). It has no controls on purpose.

namespace Mcp {

/// The whole dashboard page. It polls /status.json and /screen.png and reads /stream/timeline.
inline constexpr const char* kDashboardHtml = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>Eglantine MCP</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{color-scheme:dark;--bg:#111418;--panel:#1a1f26;--line:#2a313b;--text:#d7dde5;--dim:#8792a2;
--warn:#e5b454;--err:#ef6b6b;--ok:#6bd39b;--accent:#7aa7ff}
*{box-sizing:border-box}body{margin:0;font:13px/1.45 system-ui,Segoe UI,sans-serif;background:var(--bg);color:var(--text)}
header{display:flex;gap:16px;align-items:baseline;padding:10px 16px;border-bottom:1px solid var(--line)}
header h1{font-size:15px;margin:0}header span{color:var(--dim)}
main{display:grid;grid-template-columns:minmax(320px,640px) 1fr;gap:12px;padding:12px;height:calc(100vh - 44px)}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:10px;overflow:hidden}
#screen{width:100%;aspect-ratio:16/9;background:#000;border-radius:4px;display:block;object-fit:contain}
table{width:100%;border-collapse:collapse;margin-top:10px}td{padding:3px 4px;border-bottom:1px solid var(--line)}
td:first-child{color:var(--dim);width:40%}
#right{display:flex;flex-direction:column;gap:8px;min-height:0}
#controls{display:flex;gap:10px;flex-wrap:wrap;align-items:center;color:var(--dim)}
#log{flex:1;overflow:auto;font:12px/1.4 ui-monospace,Consolas,monospace;white-space:pre-wrap;min-height:0}
.ev{padding:1px 0;border-bottom:1px solid #1f252d}.k{display:inline-block;min-width:54px;color:var(--accent)}
.f{color:var(--dim)}.warning{color:var(--warn)}.error,.critical,.crash{color:var(--err)}.unimpl{color:#c39bff}
.mark{color:var(--ok)}.break{color:#ff9f5a}
#left{display:flex;flex-direction:column;gap:8px;overflow:auto}
.sub{margin-top:10px;color:var(--dim);font-size:12px;text-transform:uppercase;letter-spacing:.04em}
.mono{font:12px/1.4 ui-monospace,Consolas,monospace;white-space:pre-wrap}
</style></head><body>
<header><h1>Eglantine MCP</h1><span id="title">-</span><span id="conn">connecting...</span></header>
<main>
<section class="panel" id="left"><img id="screen" alt="screen"><table id="status"></table>
<div id="extras"></div></section>
<section class="panel" id="right">
<div id="controls">
<label>min level <select id="lvl"><option>trace</option><option>debug</option><option selected>info</option>
<option>warning</option><option>error</option></select></label>
<label><input type="checkbox" id="pause"> pause</label>
<label>filter <input id="flt" size="18" placeholder="text"></label>
<span id="count"></span>
</div>
<div id="log"></div>
</section>
</main>
<script>
const q=new URLSearchParams(location.search),tok=q.get('token');
const withTok=u=>tok?u+(u.includes('?')?'&':'?')+'token='+encodeURIComponent(tok):u;
const log=document.getElementById('log'),levels=['trace','debug','info','warning','error','critical'];
let n=0,es=null;
function row(e){
  if(document.getElementById('pause').checked)return;
  const f=document.getElementById('flt').value;
  if(f&&!(e.msg+e.source).toLowerCase().includes(f.toLowerCase()))return;
  const d=document.createElement('div');d.className='ev '+(e.level||e.kind);
  d.innerHTML='<span class="f">#'+e.frame+'</span> <span class="k">'+e.kind+'</span> ';
  d.append(document.createTextNode((e.source?e.source+': ':'')+e.msg));
  const stick=log.scrollTop+log.clientHeight>=log.scrollHeight-20;
  log.append(d);if(log.childElementCount>2000)log.firstChild.remove();
  if(stick)log.scrollTop=log.scrollHeight;
  document.getElementById('count').textContent=(++n)+' events';
}
function connect(){
  if(es)es.close();
  es=new EventSource(withTok('/stream/timeline?min_level='+document.getElementById('lvl').value));
  es.onopen=()=>document.getElementById('conn').textContent='live';
  es.onerror=()=>document.getElementById('conn').textContent='reconnecting...';
  es.onmessage=m=>row(JSON.parse(m.data));
}
document.getElementById('lvl').onchange=connect;connect();
// Polls stop while the tab is hidden; screens cost the emulator a frame copy, so keep them sparse.
let polls=0,lastScreen='-';
async function status(){
  if(document.hidden){setTimeout(status,1000);return;}
  try{
    const wantScreen=(polls%3)===0,wantExtras=(polls%5)===0;polls++;
    const params=[wantScreen?'screen=1':'',wantExtras?'extras=1':''].filter(Boolean).join('&');
    const s=await (await fetch(withTok('/status.json'+(params?'?'+params:'')))).json();
    if(s.extras)extras(s.extras);
    if(wantScreen)lastScreen=s.screen;else s.screen=lastScreen;
    document.getElementById('title').textContent=s.title||(s.powered_on?'running':'no game');
    const rows=[['frame',s.frame],['paused',s.paused],['fps',s.perf&&s.perf.game_fps.toFixed(1)],
      ['speed %',s.perf&&s.perf.speed_pct.toFixed(0)],['shaders building',s.perf&&s.perf.shaders_building],
      ['screen',s.screen],['program id',s.program_id],['writes',s.mcp&&s.mcp.writes_enabled],
      ['control',s.control||'free'],['last tool',s.last_tool],['recording',s.recording],
      ['route',s.route],['version',s.version&&s.version.build]];
    document.getElementById('status').innerHTML=rows.filter(r=>r[1]!==undefined&&r[1]!==null)
      .map(r=>'<tr><td>'+r[0]+'</td><td>'+String(r[1]).replace(/</g,'&lt;')+'</td></tr>').join('');
  }catch(e){}
  setTimeout(status,1000);
}
status();
// Debugger, A/B test and reports: shown only when they have something to say.
const esc=v=>String(v).replace(/&/g,'&amp;').replace(/</g,'&lt;');
function extras(x){
  let h='';
  const d=x.debugger;
  if(d&&(d.attached||(d.breakpoints||[]).length)){
    h+='<div class="sub">Debugger: '+esc(d.state)+'</div><table>';
    for(const b of d.breakpoints||[])h+='<tr><td>#'+b.id+' '+esc(b.type)+' '+esc(b.label||b.expr)+
      '</td><td>'+b.hits+' hits'+(b.enabled?'':' (retired)')+' &middot; '+esc(b.mode)+'</td></tr>';
    h+='</table>';
    const hits=d.recent_hits||[];const last=hits[hits.length-1];
    if(last)h+='<div class="mono">last: '+esc(last.label||last.reason||'')+' pc '+
      (last.pc!==undefined?'0x'+Number(last.pc).toString(16):'?')+'\n'+
      esc((last.backtrace||[]).slice(0,6).join('\n'))+'</div>';
  }
  const a=x.ab_test;
  if(a&&a.state&&a.state!=='idle'){
    h+='<div class="sub">A/B test: '+esc(a.setting)+' ('+esc(a.state)+
      (a.phase?', '+esc(a.value)+': '+esc(a.phase):'')+')</div><table>';
    for(const r of a.table||[])h+='<tr><td>'+esc(r.value)+'</td><td>'+
      (r.fps_avg!=null?Number(r.fps_avg).toFixed(1)+' fps':'-')+
      (r.one_percent_low!=null?' (1% low '+Number(r.one_percent_low).toFixed(1)+')':'')+'</td></tr>';
    h+='</table>';
  }
  const rep=x.reports;
  if(rep&&(rep.reports||[]).length){
    h+='<div class="sub">Recent reports</div><table>';
    for(const r of rep.reports)h+='<tr><td>'+esc(r.type)+'</td><td>'+esc(r.file)+'</td></tr>';
    h+='</table>';
  }
  document.getElementById('extras').innerHTML=h;
}
const img=document.getElementById('screen');let busy=false;
async function screen(){
  if(!busy&&!document.hidden){busy=true;try{const r=await fetch(withTok('/screen.png?w=640'));
    if(r.ok){const u=URL.createObjectURL(await r.blob());const old=img.src;img.src=u;if(old.startsWith('blob:'))URL.revokeObjectURL(old);}
  }catch(e){}busy=false;}
  setTimeout(screen,1000);
}
screen();
</script></body></html>
)HTML";

} // namespace Mcp
