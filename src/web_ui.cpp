// Embedded compact browser player. No external scripts, fonts, services or build-time packages.
// A scanned pairing code opens the page with a one-time link in the URL fragment, which browsers
// never send, so it stays out of every request line and proxy log.
#include "web_server.h"

namespace web {
std::string_view Page() {
    return R"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="light dark"><meta name="theme-color" content="#808080"><meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes"><link rel="manifest" href="/manifest.webmanifest"><link rel="icon" href="/icon.png"><link rel="apple-touch-icon" href="/icon.png">
<title>cordinal</title><link rel="stylesheet" href="/style.css">
<script src="/app.js" defer></script></head><body>
<main id="app"><header id="player"><div class="now"><img id="cover" alt="" hidden>
<div id="emptyCover" aria-hidden="true">♪</div><div class="details"><strong id="title">cordinal</strong>
<div id="subtitle">Your library, on this device</div><small id="format"></small></div></div>
<div class="timeline"><output id="elapsed">0:00</output><div class="wave"><canvas id="wave" aria-hidden="true"></canvas>
<input id="seek" type="range" min="0" max="1000" value="0" aria-label="Seek" disabled></div><output id="remaining">−0:00</output></div>
<div class="transport"><div class="order"><button id="shuffle" title="Shuffle" aria-label="Shuffle" aria-pressed="false"></button>
<button id="repeat" title="Repeat off" aria-label="Repeat off"></button><button id="recent" title="Recently played" aria-label="Recently played"></button></div>
<div class="playback"><button id="previous" title="Previous" aria-label="Previous"></button><button id="play" title="Play" aria-label="Play"></button>
<button id="next" title="Next" aria-label="Next"></button></div><div class="volume"><span id="speaker" aria-hidden="true"></span>
<input id="volume" type="range" min="0" max="100" value="100" aria-label="Volume"><small id="deviceVolume" hidden>Device volume</small></div></div>
<div class="navigation"><nav id="tabs" aria-label="Library and playlists"></nav><input id="search" type="search" placeholder="Search Library" aria-label="Search music">
<button id="options" aria-label="Player options" title="Player options">⋯</button></div>
<div id="settings" hidden><label>Library view <select id="view"><option value="tracks">Tracks</option><option value="albums">Albums</option><option value="artists">Artists</option></select></label>
<label>Theme <select id="theme"><option value="server">Follow server</option></select></label>
<label id="qualitySetting" hidden>Streaming <select id="quality" title="Low data: 96 kb/s AAC. Balanced: 128 kb/s AAC. Original keeps the source quality. Saved for this browser."><option value="aac">Low data · about 45 MB/hour</option><option value="balanced">Balanced · about 60 MB/hour</option><option value="original">Original quality</option></select></label>
<button id="awayLink" hidden>Listen away from home</button><button id="logout">Unpair this browser</button>
<small id="offlineHint" hidden>Keeping music on this phone needs cordinal's secure (https) address.</small></div>
<div id="crumb" hidden><button id="back">‹ Library</button><span id="location"></span><button id="keep" hidden>Keep on this phone</button></div></header>
<div id="notice" role="status" aria-live="polite"></div><section id="browser" aria-label="Music library"><div id="rows"></div>
<button id="more" hidden>Load more</button></section><footer id="summary"></footer></main>
<dialog id="pairing"><form id="pairForm"><h1>Connect to cordinal</h1><p>On the computer, open <b>Web server...</b> and choose <b>Pair device</b>. Scan its code with the phone's camera, or enter the digits here.</p>
<label>Pairing code <input id="code" inputmode="numeric" pattern="[0-9]{6,12}" maxlength="12" autocomplete="one-time-code" required></label>
<label>Device name <input id="deviceName" maxlength="80" placeholder="My phone" autocomplete="off"></label>
<p id="pairError" role="alert"></p><button type="submit">Connect</button></form></dialog>
<dialog id="actions"><strong id="actionTitle"></strong><button id="actionPlay">Play</button><button id="actionNext">Play next</button>
<button id="actionAdd">Add to queue</button><button id="actionUp">Move up</button><button id="actionDown">Move down</button>
<button id="actionRemove">Remove from queue</button><button id="actionClose">Close</button></dialog>
<audio id="audio" preload="none" playsinline></audio></body></html>)html";
}
std::string_view Style() {
    return R"css(@font-face{font-family:'DELTARUNE Pixel';src:url('/deltarune.ttf') format('truetype');font-display:swap}
*{box-sizing:border-box}html{height:100%;visibility:hidden}body{margin:0;height:100dvh;background:var(--bg);color:var(--text);font:13px var(--face,system-ui,sans-serif)}
button,input,select{font:inherit;color:inherit}button{border:0;background:transparent;cursor:pointer}button:disabled,input:disabled{opacity:.45;cursor:default}
button:hover{background:var(--hover)}button:focus-visible,input:focus-visible,select:focus-visible{outline:2px solid var(--accent);outline-offset:-2px}
button[aria-pressed=true],.active{color:var(--accent)}input,select{background:var(--bg);border:1px solid var(--border);border-radius:0;padding:4px;min-width:0}
input[type=range]{padding:0;accent-color:var(--accent);border:0}button svg{width:20px;height:20px;stroke:currentColor;fill:none;stroke-width:1.5;stroke-linecap:round;stroke-linejoin:round;vertical-align:middle}
[hidden]{display:none!important}#app{height:100%;max-width:1100px;margin:auto;display:flex;flex-direction:column}#player{flex:none;background:var(--surface);padding:8px 8px 0}
.now{display:flex;gap:10px;align-items:center;min-width:0}.now>img,#emptyCover{height:64px;width:64px;object-fit:contain;flex:none;background:var(--surface2)}
#emptyCover{display:grid;place-items:center;font-size:26px;color:var(--sub)}.details{min-width:0;flex:1}.details strong,.details div{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.details strong{font-weight:700;margin-bottom:4px}.details div{color:var(--sub)}small{color:var(--sub)}#format{display:block;min-height:14px;font-size:11px;margin-top:3px}
.timeline{display:flex;align-items:center;gap:8px;height:36px}.timeline output{width:42px;flex:none;color:var(--sub);font-variant-numeric:tabular-nums;text-align:center}
.wave{position:relative;flex:1;height:26px}#wave{width:100%;height:100%;display:block}#seek{position:absolute;inset:0;width:100%;height:100%;margin:0;opacity:0;cursor:pointer}
.wave:focus-within{outline:1px solid var(--accent)}.transport{display:flex;align-items:center;justify-content:space-between;gap:2px;height:46px}
.order,.playback,.volume{display:flex;align-items:center}.order button{width:32px;height:44px}.playback button{width:40px;height:44px}.volume{justify-content:flex-end;gap:5px;min-width:0;width:102px}
#repeat{position:relative}#repeat[data-mode=one]::after{content:'1';position:absolute;inset:15px 0 auto;font-size:10px;line-height:14px}
.volume svg{width:16px;height:16px;stroke:var(--sub);fill:none;stroke-width:1.5}.volume input{width:74px;height:24px}.volume small{font-size:10px}
.navigation{display:flex;align-items:center;gap:5px;min-height:36px}#tabs{display:flex;flex:1;overflow-x:auto;scrollbar-width:thin;min-width:75px}#tabs button{white-space:nowrap;padding:8px;min-height:36px}
#tabs button[aria-current=page]{color:var(--accent);font-weight:700}#search{width:34%;max-width:250px;height:27px}#options{height:36px;width:30px;flex:none;font-size:21px}
#settings{display:flex;flex-wrap:wrap;gap:12px;padding:8px 0;border-top:1px solid var(--border)}#settings label{display:flex;align-items:center;gap:8px}#settings button{padding:6px}
#crumb{display:flex;align-items:center;gap:8px;border-top:1px solid var(--border);min-height:32px;color:var(--sub)}#back{min-height:32px;padding:4px 8px}#keep{margin-left:auto;flex:none;min-height:32px;padding:4px 8px;color:var(--accent)}#location{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#notice{flex:none;color:var(--amber);padding:0 8px;background:var(--surface);line-height:1.5}#notice:not(:empty){padding-top:5px;padding-bottom:5px}
#browser{flex:1;min-height:0;overflow:auto;overscroll-behavior:contain;scrollbar-color:var(--border) var(--bg)}.columns{position:sticky;top:0;z-index:1;background:var(--bg);border-bottom:1px solid var(--border);color:var(--sub)}
.row,.columns{display:grid;grid-template-columns:20px 28px minmax(0,1fr) minmax(70px,24%) 42px;align-items:center;gap:6px;min-height:30px;padding:0 8px}
.row{cursor:pointer}.row:hover{background:var(--hover)}.row.playing{color:var(--playing);background:var(--surface2)}.row .number,.duration{text-align:right;font-variant-numeric:tabular-nums;color:var(--sub)}
.row .name,.row .artist{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.row .artist{color:var(--sub)}.row .mark{font-size:12px}.row .name button{float:right;width:30px;min-height:30px;color:var(--sub)}
.mix{font-size:10px;background:var(--select);color:var(--selectText);padding:0 3px;margin-left:5px}.group{display:flex;align-items:center;gap:8px;min-height:42px;padding:4px 8px;background:var(--surface);cursor:pointer}
.group img{width:30px;height:30px;object-fit:contain}.group .groupTitle{font-weight:700;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.group>div{min-width:0;flex:1}.group small{display:block;font-size:11px}
.group .placeholder{width:30px;height:30px;display:grid;place-items:center;background:var(--surface2);color:var(--sub);flex:none}
.group .groupCount{font-weight:400;color:var(--sub);white-space:nowrap;font-size:11px}.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:12px;padding:12px}
.card{text-align:left;padding:0;min-width:0}.card img,.card .placeholder{width:100%;aspect-ratio:1;object-fit:contain;background:var(--surface2)}.card .placeholder{display:grid;place-items:center;color:var(--sub);font-size:32px}
.card strong,.card small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-top:4px}.card strong{font-weight:700}.empty{padding:24px 12px;color:var(--sub)}
#more{display:block;margin:10px auto;padding:8px 24px;border:1px solid var(--border)}footer{flex:none;min-height:22px;padding:3px 8px;font-size:11px;color:var(--sub);background:var(--surface);border-top:1px solid var(--border)}
dialog{background:var(--surface);color:var(--text);border:1px solid var(--border);max-width:min(400px,calc(100vw - 24px));padding:20px}dialog::backdrop{background:var(--bg);opacity:.85}dialog h1{font-size:17px;margin:0 0 14px;font-weight:700}
dialog p{color:var(--sub);line-height:1.5}dialog label{display:block;margin:14px 0}dialog input{display:block;width:100%;margin-top:6px;padding:8px}dialog button{padding:10px;border:1px solid var(--border);min-height:40px}
#pairForm button{width:100%;background:var(--select);color:var(--selectText)}#pairError{color:var(--error)}#code{font-size:24px;letter-spacing:6px;text-align:center;font-variant-numeric:tabular-nums}#actions button{display:block;width:100%;text-align:left;margin-top:5px}#actionTitle{display:block;margin-bottom:12px}
@media(pointer:coarse){.row{min-height:38px}.row .name button{min-height:38px}.group{min-height:48px}}@media(min-width:700px){#player{padding:12px 16px 0}.now>img,#emptyCover{width:72px;height:72px}.row,.columns{grid-template-columns:24px 36px minmax(0,1fr) minmax(120px,27%) 60px;min-height:30px}.order button{width:40px}.volume{width:120px}.volume input{width:90px}}
@media(max-width:359px){.volume{width:80px}.volume input{width:52px}.order button{width:28px}.playback button{width:36px}.row,.columns{grid-template-columns:14px 22px minmax(0,1fr) 70px;gap:4px}.row .duration,.columns .duration{display:none}.groupCount{display:none}#player{padding-left:6px;padding-right:6px}}
)css";
}
std::string_view Script() {
    return R"js('use strict';
const $=id=>document.getElementById(id),audio=$('audio');
const icons={play:'M8 5l11 7-11 7z',pause:'M8 5v14M16 5v14',next:'M5 5l11 7-11 7zM19 5v14',previous:'M19 5L8 12l11 7zM5 5v14',
shuffle:'M3 6h3l12 12h3M18 15l3 3-3 3M3 18h3l4-4M14 10l4-4h3M18 3l3 3-3 3',
repeat:'M4 8a6 6 0 016-4h8l-3-3M18 4l-3 3M20 16a6 6 0 01-6 4H6l3 3M6 20l3-3',recent:'M3 4v6h6M3 10a9 9 0 119 11M12 6v6l4 2',
speaker:'M3 10h4l5-4v12l-5-4H3zM16 8a6 6 0 010 8M18 5a10 10 0 010 14'};
function icon(id,name){$(id).innerHTML='<svg viewBox="0 0 24 24" aria-hidden="true"><path d="'+icons[name]+'"/></svg>';}
for(const name of ['play','next','previous','shuffle','repeat','recent','speaker'])icon(name,name);
function storedTheme(){try{return localStorage.getItem('cordinal.theme')||'server';}catch(e){return 'server';}}
function storedQuality(){try{const q=localStorage.getItem('cordinal.quality');return q==='aac'||q==='balanced'?q:'original';}catch(e){return 'original';}}
const state={tab:'library',view:'tracks',group:'',album:'',artist:'',date:'',items:[],total:0,queue:[],index:-1,current:null,
shuffle:false,deck:[],seen:new Set(),past:[],repeat:0,history:[],themes:null,theme:storedTheme(),quality:storedQuality(),peaks:[],
mode:'original',offset:0,sequence:0,session:'',heard:0,lastTime:0,lastWall:0,playing:false,seeking:false,started:false,
wanted:false,loadingAt:null,resumeAt:0,lastAdvance:0,waitSince:0,loadTicket:0,pendingSource:false,
lastReport:0,reportChain:Promise.resolve(),load:0,source:0,notice:'',action:null,actionIndex:-1,actionQueue:false};
function message(text){state.notice=text;$('notice').textContent=text;}
function duration(seconds){seconds=Math.max(0,Math.floor(Number.isFinite(seconds)?seconds:0));return(seconds>=3600?Math.floor(seconds/3600)+':':'')+
String(Math.floor(seconds/60)%60).padStart(seconds>=3600?2:1,'0')+':'+String(seconds%60).padStart(2,'0');}
function text(tag,value,cls){const e=document.createElement(tag);if(cls)e.className=cls;e.textContent=value;return e;}
function key(){const a=new Uint8Array(16);crypto.getRandomValues(a);return Array.from(a,n=>n.toString(16).padStart(2,'0')).join('');}
async function api(path,params,post){const options={credentials:'same-origin',cache:'no-store'};
if(post){options.method='POST';options.headers={'Content-Type':'application/x-www-form-urlencoded'};options.body=new URLSearchParams(post);}
const response=await fetch('/api/v1/'+path+(params?'?'+new URLSearchParams(params):''),options);
if(response.status===401&&path!=='pair'){if(!$('pairing').open)$('pairing').showModal();throw Error('Pair this browser with cordinal.');}
const data=await response.json().catch(()=>({error:'The server did not answer.'}));if(!response.ok)throw Error(data.error||'The request failed.');return data;}
function applyTheme(){if(!state.themes)return;let id=state.theme==='server'?state.themes.current:Number(state.theme);
if(!Number.isInteger(id)||id<0||id>=state.themes.themes.length)id=state.themes.current;
if(id===0)id=(state.theme==='server'?state.themes.dark:matchMedia('(prefers-color-scheme:dark)').matches)?2:1;
const t=state.themes.themes[id],root=document.documentElement;for(const [role,color]of Object.entries(t.colors))root.style.setProperty('--'+role,color);
let face='system-ui,sans-serif';if(t.font==='Georgia')face='Georgia,serif';else if(t.font==='Bahnschrift')face='Bahnschrift,system-ui,sans-serif';
else if(t.font==='Consolas')face='Consolas,monospace';else if(t.font==='DELTARUNE Pixel')face='"DELTARUNE Pixel",monospace';
root.style.setProperty('--face',face);root.style.colorScheme=id===1||id===5?'light':'dark';root.style.fontSize=t.font==='DELTARUNE Pixel'?'16px':'13px';
document.querySelector('meta[name=theme-color]').content=t.colors.surface;
document.body.style.fontSize=t.font==='DELTARUNE Pixel'?'16px':'13px';root.style.visibility='visible';state.mark=t.mark;drawWave();markPlaying();}
async function refreshThemes(){try{state.themes=await api('themes');if($('theme').options.length===1){state.themes.themes.forEach((t,i)=>{
const o=new Option(t.name,String(i));$('theme').add(o);});$('theme').value=state.theme;}applyTheme();}catch(e){message('Could not reach cordinal. Check the server and network.');}}
matchMedia('(prefers-color-scheme:dark)').addEventListener('change',applyTheme);
$('theme').onchange=()=>{state.theme=$('theme').value;try{localStorage.setItem('cordinal.theme',state.theme);}catch(e){}applyTheme();};
function currentQuery(){const q={view:state.view,q:$('search').value};if(state.tab!=='library')q.playlist=state.tab;
if(state.group==='album'){q.album=state.album;q.artist=state.artist;q.date=state.date;}else if(state.group==='artist')q.artist=state.artist;return q;}
function makeTab(id,label){const b=text('button',label);b.dataset.tab=id;b.setAttribute('aria-current',state.tab===id?'page':'false');b.onclick=()=>{
state.tab=id;state.group=state.album=state.artist=state.date=state.keptSet='';$('search').value='';$('view').disabled=id!=='library';loadView();};return b;}
async function tabs(){const data=await api('catalog',{view:'playlists'});state.offline=false;$('tabs').replaceChildren(makeTab('library','Library'),makeTab('queue','Now Playing'));
for(const p of data.items)$('tabs').append(makeTab(p.id,p.name));keptTab();}
function keptTab(){if(canKeep&&Object.keys(kept.sets).length&&!$('tabs').querySelector('[data-tab=kept]'))$('tabs').append(makeTab('kept','On this phone'));}
async function loadView(append=false){const serial=++state.load;try{document.querySelectorAll('#tabs button').forEach(b=>b.setAttribute('aria-current',b.dataset.tab===state.tab?'page':'false'));
$('search').placeholder='Search '+(state.tab==='queue'?'Now Playing':state.tab==='library'?'Library':'playlist');
crumb();
if(state.tab==='kept'&&!state.keptSet){renderKept();return;}
if(state.tab==='queue'||state.tab==='recent'||state.tab==='kept'){const set=kept.sets[state.keptSet],source=state.tab==='queue'?state.queue:
state.tab==='kept'?(set?set.ids.map(id=>kept.tracks[id]).filter(Boolean):[]):state.history;const words=$('search').value.toLocaleLowerCase().split(/\s+/).filter(Boolean);
const matches=source.filter(t=>words.every(w=>(t.title+' '+t.artist+' '+t.album).toLocaleLowerCase().includes(w)));state.total=matches.length;state.items=matches.slice(0,append?state.items.length+100:100);renderRows();return;}
const query=currentQuery();if(state.tab!=='library'||state.group)query.view='tracks';query.offset=append?state.items.length:0;query.limit=100;
const result=await api('catalog',query);if(serial!==state.load)return;state.items=append?state.items.concat(result.items):result.items;state.total=result.total;
if(query.view==='albums'||query.view==='artists')renderCards(query.view);else renderRows();
}catch(e){if(serial===state.load)message(e.message);}}
function artwork(id){const img=document.createElement('img');img.alt='';img.loading='lazy';img.src='/api/v1/art?id='+encodeURIComponent(id);img.onerror=()=>{const placeholder=text('span','♪','placeholder');placeholder.setAttribute('aria-hidden','true');img.replaceWith(placeholder);};return img;}
function renderCards(kind){const grid=text('div','','cards');for(const item of state.items){const b=text('button','','card');b.append(artwork(item.cover),text('strong',item.name||'Unknown '+(kind==='albums'?'album':'artist')),
text('small',(kind==='albums'?item.artist+' · ':'')+item.count+' tracks'));b.onclick=()=>{state.group=kind==='albums'?'album':'artist';state.album=kind==='albums'?item.name:'';state.artist=kind==='albums'?item.artist:item.name;state.date=item.date||'';loadView();};grid.append(b);}
$('rows').replaceChildren(grid);summary();}
)js" R"js(function renderRows(){const rows=$('rows');rows.replaceChildren();if(!state.items.length){rows.append(text('div',state.tab==='queue'?'Your queue is empty. Choose music from Library.':'No tracks match this view.','empty'));summary();return;}
const columns=text('div','','columns');for(const [v,c]of [['',''],['#','number'],['Title',''],['Artist',''],['Time','duration']])columns.append(text('span',v,c));rows.append(columns);
const counts=new Map();for(const t of state.items){const key=[t.albumArtist,t.album,t.date].join('\u0001');counts.set(key,(counts.get(key)||0)+1);}
let previous='';state.items.forEach((t,i)=>{const group=[t.albumArtist,t.album,t.date].join('\u0001');if(group!==previous&&t.album){previous=group;const h=text('div','','group');
h.setAttribute('role','button');h.tabIndex=0;h.setAttribute('aria-label','Play '+t.album);h.append(artwork(t.id));const info=text('div','');info.append(text('div',t.album,'groupTitle'),text('small',(t.albumArtist||t.artist)+(t.date?' · '+t.date:'')));h.append(info);
h.append(text('span',counts.get(group)+' tracks','groupCount'));
h.onclick=()=>playBrowse(t,i);h.onkeydown=e=>{if(e.key==='Enter'){e.preventDefault();playBrowse(t,i);}};rows.append(h);}
const row=text('div','','row');row.dataset.id=t.id;row.tabIndex=0;row.setAttribute('role','button');row.setAttribute('aria-label','Play '+t.title+' by '+t.artist);
row.append(text('span','','mark'),text('span',t.number?String(t.number).padStart(2,'0'):'','number'));const name=text('span','','name');const more=text('button','⋯');more.setAttribute('aria-label','Actions for '+t.title);
more.onclick=e=>{e.stopPropagation();showActions(t,i);};name.append(more,document.createTextNode(t.title));if(t.duration>=3600)name.append(text('span','MIX','mix'));
row.append(name,text('span',t.artist,'artist'),text('span',duration(t.duration),'duration'));row.onclick=()=>playBrowse(t,i);row.onkeydown=e=>{if(e.target===row&&e.key==='Enter'){e.preventDefault();playBrowse(t,i);}};
row.oncontextmenu=e=>{e.preventDefault();showActions(t,i);};rows.append(row);});markPlaying();summary();}
function summary(){$('more').hidden=state.items.length>=state.total;$('summary').textContent=state.total+' '+
((state.tab==='library'&&!state.group&&state.view!=='tracks')?state.view:'tracks')+(state.current?' · On this device':'');}
function markPlaying(){document.querySelectorAll('.row').forEach(row=>{const on=state.current&&state.session&&row.dataset.id===state.current.id;row.classList.toggle('playing',!!on);row.querySelector('.mark').textContent=on?(state.mark||'▶'):'';});}
async function allTracks(query,update){let list=[],total=1;while(list.length<total){const data=await api('catalog',{...query,view:'tracks',offset:list.length,limit:200});
if(!data.items.length)break;list.push(...data.items);total=data.total;if(update&&update(list)===false)break;}return list;}
function playBrowse(t,index){state.past=[];state.seen=new Set();if(state.tab==='queue'){state.index=state.queue.indexOf(t);state.seen.add(state.index);resetDeck();startTrack(t);return;}
const ticket=++state.queueTicket;state.queue=state.items.slice();state.index=index;state.seen.add(index);resetDeck();startTrack(t);
if(state.tab==='recent'||state.tab==='kept')return;const query=currentQuery();allTracks(query,list=>{if(ticket!==state.queueTicket)return false;if(list.length<=state.index)return;
const at=state.index,active=state.current;state.queue=list.slice();state.index=list[at]?.id===active.id?at:Math.max(0,list.findIndex(n=>n.id===active.id));resetDeck();}).catch(e=>message(e.message));}
state.queueTicket=0;
function showActions(t,index){state.action=t;state.actionIndex=index;state.actionQueue=state.tab==='queue';$('actionTitle').textContent=t.title;
for(const id of ['actionUp','actionDown','actionRemove'])$(id).hidden=!state.actionQueue;$('actions').showModal();}
$('actionClose').onclick=()=>$('actions').close();
$('actionPlay').onclick=()=>{const t=state.action;$('actions').close();playBrowse(t,state.actionIndex);};
$('actionNext').onclick=()=>{const at=Math.max(0,state.index+1);state.queue.splice(at,0,{...state.action});queueChanged();if(state.shuffle){state.deck=state.deck.filter(i=>i!==at);state.deck.unshift(at);}$('actions').close();message('Added to play next.');if(state.tab==='queue')loadView();};
$('actionAdd').onclick=()=>{state.queue.push({...state.action});queueChanged();$('actions').close();message('Added to queue.');if(state.tab==='queue')loadView();};
$('actionRemove').onclick=()=>{const i=state.queue.indexOf(state.action);if(i>=0){state.queue.splice(i,1);if(i<state.index)--state.index;else if(i===state.index)state.index=i-1;}
queueChanged();$('actions').close();loadView();};
function moveQueue(delta){const i=state.queue.indexOf(state.action),j=i+delta;if(i>=0&&j>=0&&j<state.queue.length){[state.queue[i],state.queue[j]]=[state.queue[j],state.queue[i]];
if(state.index===i)state.index=j;else if(state.index===j)state.index=i;}queueChanged();$('actions').close();loadView();}
$('actionUp').onclick=()=>moveQueue(-1);$('actionDown').onclick=()=>moveQueue(1);
function position(){if(state.loadingAt!==null)return state.loadingAt;if(audio.error||audio.readyState===0)return state.resumeAt;
return state.offset+(Number.isFinite(audio.currentTime)?audio.currentTime:0);}
function sample(){const now=performance.now(),at=audio.currentTime;if(state.playing&&!state.seeking&&Number.isFinite(at)){
const delta=at-state.lastTime,wall=(now-state.lastWall)/1000;if(delta>0){state.lastAdvance=now;state.waitSince=0;}
if(!audio.error&&audio.readyState>0)state.resumeAt=state.offset+at;
if(delta>0&&delta<=wall+.4)state.heard+=Math.min(delta,wall+.1);}
state.lastTime=Number.isFinite(at)?at:0;state.lastWall=now;}
function report(event){if(!state.session||(!state.started&&event!=='start'))return;
// A kept copy plays without cordinal: its listen is logged here and sent when cordinal is reachable.
if(state.mode==='kept'){if(event==='ended'||event==='skip')logPlay(event==='skip');return;}const packet={session:state.session,seq:++state.sequence,id:state.current.id,event,heard:state.heard.toFixed(3)};
state.reportChain=state.reportChain.then(()=>api('listen',null,packet)).catch(e=>message('Listening history: '+e.message));state.lastReport=performance.now();}
function finish(manual){if(!state.current)return;sample();if(state.started)report(manual?'skip':'ended');cancelRetry();state.wanted=false;state.waitSince=0;
state.started=false;state.session='';state.playing=false;markPlaying();showPlay(false);}
function mediaMetadata(){if(!('mediaSession'in navigator)||!state.current)return;const t=state.current;try{navigator.mediaSession.metadata=new MediaMetadata({title:t.title,artist:t.artist,album:t.album,
artwork:[{src:new URL('/api/v1/art?id='+encodeURIComponent(t.id),location.href).href}]});}catch(e){}}
function updatePosition(){const length=state.current?state.current.duration:0,at=position();$('elapsed').textContent=duration(at);$('remaining').textContent='−'+duration(length-at);
if(!state.seeking)$('seek').value=length?Math.min(1000,at/length*1000):0;$('seek').setAttribute('aria-valuetext',duration(at)+' of '+duration(length));drawWave();
if('mediaSession'in navigator&&navigator.mediaSession.setPositionState&&length>0){try{navigator.mediaSession.setPositionState({duration:length,position:Math.min(at,length),playbackRate:1});}catch(e){}}}
)js" R"js(function streamMode(t){if(kept.tracks[t.id]&&(state.offline||navigator.onLine===false||state.quality!=='original'))return 'kept';
return state.quality!=='original'?'aac':t.original&&audio.canPlayType(t.mime)?'original':'wav';}
function startTrack(t){finish(true);++state.source;state.current=t;state.heard=0;state.sequence=0;state.session=key();state.mode=streamMode(t);
retries=0;
state.offset=0;state.peaks=[];state.lastTime=0;state.lastWall=performance.now();message('');$('title').textContent=t.title;$('subtitle').textContent=t.artist+(t.album?' · '+t.album:'');
$('cover').hidden=false;$('emptyCover').hidden=true;$('cover').src='/api/v1/art?id='+encodeURIComponent(t.id);$('seek').disabled=false;
state.history=state.history.filter(n=>n.id!==t.id);state.history.unshift(t);state.history.length=Math.min(100,state.history.length);mediaMetadata();markPlaying();loadSource(0,true);
const source=state.source;fetch('/api/v1/peaks?id='+encodeURIComponent(t.id),{credentials:'same-origin'}).then(r=>r.ok?r.json():[]).then(p=>{if(source===state.source){state.peaks=p;drawWave();}}).catch(()=>{});}
$('cover').onerror=()=>{$('cover').hidden=true;$('emptyCover').hidden=false;};
function loadSource(at,resume=state.wanted){cancelRetry();const ticket=++state.loadTicket;state.wanted=resume;state.loadingAt=state.resumeAt=at;state.lastAdvance=performance.now();state.waitSince=0;
state.offset=state.mode==='aac'?Math.max(0,at):state.mode==='wav'?Math.floor(Math.max(0,at)/3600)*3600:0;const local=Math.max(0,at-state.offset);
const bitrate=state.quality==='balanced'?128:96,query={id:state.current.id,format:state.mode,start:state.offset};if(state.mode==='aac')query.bitrate=bitrate;
state.playing=false;state.seeking=true;
$('format').textContent=state.mode==='kept'?'Kept on this phone · '+(kept.tracks[state.current.id].keptAs||''):state.mode==='aac'?'AAC · '+bitrate+' kb/s · '+(state.quality==='balanced'?'balanced':'low data'):state.mode==='wav'?'PCM 16-bit · 48 kHz (converted)':state.current.codec+' · original';
audio.onloadedmetadata=()=>{if(local>0)audio.currentTime=Math.min(local,Math.max(0,audio.duration-.01));state.loadingAt=null;state.lastTime=audio.currentTime;state.lastWall=performance.now();state.lastAdvance=state.lastWall;state.seeking=false;updatePosition();};
// A paused seek or quality change must not give the browser a new URL to preload.
state.pendingSource=!resume;if(!resume){audio.removeAttribute('src');audio.load();showPlay(false);updatePosition();return;}
audio.src=state.mode==='kept'?'/offline/audio/'+state.current.id:'/api/v1/audio?'+new URLSearchParams(query);const promise=audio.play();if(promise)promise.catch(e=>{if(ticket!==state.loadTicket)return;
if(e.name==='NotAllowedError'){state.wanted=false;showPlay(false);message('Tap Play to continue playback.');}else if(e.name!=='AbortError'&&e.name!=='NotSupportedError')message('Could not play this track. Tap Play to retry.');});}
function seek(at){if(!state.current)return;sample();state.seeking=true;at=Math.max(0,Math.min(at,state.current.duration-.01));
retries=0;
if(state.pendingSource||state.mode==='aac'||(state.mode==='wav'&&(at<state.offset||at>=state.offset+3600)))loadSource(at);else audio.currentTime=Math.max(0,at-state.offset);report('seek');}
function resetDeck(){state.deck=state.queue.map((_,i)=>i).filter(i=>i!==state.index&&!state.seen.has(i));for(let i=state.deck.length-1;i>0;i--){const j=Math.floor(Math.random()*(i+1));[state.deck[i],state.deck[j]]=[state.deck[j],state.deck[i]];}}
function queueChanged(){++state.queueTicket;state.past=[];state.seen=new Set([state.index]);resetDeck();}
function next(manual=true){if(!state.queue.length){finish(manual);audio.pause();return;}let i=state.index+1;
if(!manual&&state.repeat===2)i=state.index;else if(state.shuffle){if(!state.deck.length&&state.repeat===1){state.seen=new Set([state.index]);resetDeck();if(!state.deck.length)state.deck=[state.index];}i=state.deck.length?state.deck.shift():state.queue.length;}
if(i>=state.queue.length){if(state.repeat===1)i=0;else{finish(manual);audio.pause();return;}}
finish(manual);if(i!==state.index)state.past.push(state.index);state.index=i;state.seen.add(i);startTrack(state.queue[i]);}
function previous(){if(position()>3){seek(0);return;}if(state.queue.length){state.index=state.past.length?state.past.pop():Math.max(0,state.index-1);startTrack(state.queue[state.index]);}}
function togglePlay(){if(!state.current){if(state.items.length&&state.items[0].id)playBrowse(state.items[0],0);else message('Choose a track to play.');return;}
if(!state.session){startTrack(state.current);return;}if(retryTimer||(state.wanted&&!audio.paused)){pausePlayback();return;}
if(state.pendingSource){retries=0;loadSource(position(),true);return;}
if(audio.error){const at=position();finish(false);retries=0;state.session=key();state.sequence=0;state.heard=0;loadSource(at,true);return;}
state.wanted=true;retries=0;audio.play().catch(()=>{state.wanted=false;showPlay(false);message('Could not resume playback.');});}
function pausePlayback(){state.wanted=false;cancelRetry();state.waitSince=0;audio.pause();showPlay(false);}
function showPlay(playing){const label=playing?'Pause':'Play';icon('play',playing?'pause':'play');$('play').setAttribute('aria-label',label);$('play').title=label;
if('mediaSession'in navigator)navigator.mediaSession.playbackState=playing?'playing':'paused';}
audio.onplay=()=>{state.wanted=true;state.lastTime=audio.currentTime;state.lastWall=performance.now();state.playing=true;if(!state.started){state.started=true;report('start');}else report('resume');showPlay(true);};
audio.onpause=()=>{sample();state.playing=false;if(!state.seeking&&!retryTimer&&!state.waitSince)state.wanted=false;if(state.started)report('pause');showPlay(false);};
audio.onseeking=()=>{state.seeking=true;};audio.onseeked=()=>{state.seeking=false;state.lastTime=audio.currentTime;state.lastWall=performance.now();state.resumeAt=position();updatePosition();};
audio.ontimeupdate=()=>{sample();if(state.started&&performance.now()-state.lastReport>=5000)report('progress');updatePosition();};
audio.onended=()=>{sample();if(state.mode==='aac'&&position()+.1<Math.min(state.current.duration,state.offset+3600)){reconnect();return;}
if((state.mode==='wav'||state.mode==='aac')&&state.offset+3600<state.current.duration-.05){loadSource(state.offset+3600);return;}next(false);};
audio.onerror=()=>{if(!state.current)return;if(state.mode==='original'&&audio.error&&(audio.error.code===3||audio.error.code===4)){const at=position();state.mode='wav';loadSource(at);}
else if(state.mode==='aac'&&audio.error&&(audio.error.code===3||audio.error.code===4)){state.wanted=false;cancelRetry();showPlay(false);message('Compressed audio could not play. Try again, or choose Original quality.');}
else if(state.mode==='kept'){state.wanted=false;showPlay(false);message('This kept track could not play. Remove it from this phone and keep it again.');}
else if(state.session)reconnect();else message('The track could not be streamed. Check the server, then tap Play to retry.');};
let retries=0,retryTimer=0;
function cancelRetry(){if(retryTimer){clearTimeout(retryTimer);retryTimer=0;}}
function reconnect(immediate=false){if(retryTimer||!state.current||!state.session||!state.wanted)return;
if(!state.waitSince)state.waitSince=performance.now();if(navigator.onLine===false){message('Waiting for a connection…');return;}
message(retries>=8?'Waiting for cordinal…':'Reconnecting…');const source=state.source;
const delay=immediate?0:retries>=8?60000:Math.min(15000,1000*2**retries);retries=Math.min(8,retries+1);
retryTimer=setTimeout(()=>{retryTimer=0;if(source===state.source&&state.current&&state.session&&state.wanted)loadSource(position());},delay);}
audio.onplaying=()=>{cancelRetry();retries=0;state.waitSince=0;state.lastAdvance=performance.now();if(['Reconnecting…','Buffering…','Waiting for a connection…','Waiting for cordinal…'].includes(state.notice))message('');};
audio.onwaiting=audio.onstalled=()=>{if(state.wanted&&audio.readyState<3){if(!state.waitSince)state.waitSince=performance.now();message('Buffering…');}};
addEventListener('online',()=>{if(state.wanted&&(retryTimer||state.waitSince||audio.error)){cancelRetry();retries=0;reconnect(true);}});
addEventListener('offline',()=>{if(state.wanted&&audio.readyState<3)reconnect();});
)js" R"js(function drawWave(){const canvas=$('wave'),width=canvas.clientWidth,height=canvas.clientHeight;if(!width||!height)return;const scale=devicePixelRatio||1;
if(canvas.width!==Math.round(width*scale)||canvas.height!==Math.round(height*scale)){canvas.width=Math.round(width*scale);canvas.height=Math.round(height*scale);}
const c=canvas.getContext('2d');c.setTransform(scale,0,0,scale,0,0);const colors=getComputedStyle(document.documentElement);c.fillStyle=colors.getPropertyValue('--surface');c.fillRect(0,0,width,height);
const ratio=state.current&&state.current.duration?position()/state.current.duration:0,count=Math.floor(width/3);for(let i=0;i<count;i++){
const bucket=state.peaks.length?state.peaks[Math.floor(i/count*state.peaks.length)]/255:.6;const h=Math.max(2,bucket*height);c.fillStyle=colors.getPropertyValue(i/count<=ratio?'--accent':'--faint');c.fillRect(i*3,(height-h)/2,2,h);}}
new ResizeObserver(drawWave).observe($('wave'));
$('seek').oninput=()=>{$('elapsed').textContent=duration(Number($('seek').value)/1000*(state.current?.duration||0));};$('seek').onchange=()=>seek(Number($('seek').value)/1000*(state.current?.duration||0));
$('play').onclick=togglePlay;$('next').onclick=()=>next(true);$('previous').onclick=previous;
$('shuffle').onclick=()=>{state.shuffle=!state.shuffle;state.seen=new Set([state.index]);resetDeck();$('shuffle').setAttribute('aria-pressed',String(state.shuffle));};
$('repeat').onclick=()=>{state.repeat=(state.repeat+1)%3;$('repeat').dataset.mode=['off','all','one'][state.repeat];$('repeat').setAttribute('aria-label','Repeat '+['off','all','one'][state.repeat]);$('repeat').setAttribute('aria-pressed',String(!!state.repeat));$('repeat').title='Repeat '+['off','all','one'][state.repeat];};
$('recent').onclick=()=>{state.tab='recent';state.group=state.album=state.artist='';$('search').value='';loadView();};
audio.volume=.5;if(Math.abs(audio.volume-.5)>.01){$('volume').hidden=true;$('deviceVolume').hidden=false;}audio.volume=1;
$('volume').oninput=()=>{audio.volume=(Number($('volume').value)/100)**2;};
if('mediaSession'in navigator){const handlers={play:()=>{if(audio.paused)togglePlay();},pause:pausePlayback,previoustrack:previous,nexttrack:()=>next(true),
seekto:e=>seek(e.seekTime),seekbackward:e=>seek(position()-(e.seekOffset||10)),seekforward:e=>seek(position()+(e.seekOffset||10))};
for(const [action,fn]of Object.entries(handlers))try{navigator.mediaSession.setActionHandler(action,fn);}catch(e){}}
$('options').onclick=()=>{$('settings').hidden=!$('settings').hidden;};$('view').onchange=()=>{state.view=$('view').value;state.group=state.album=state.artist='';loadView();};
$('quality').value=state.quality;$('quality').onchange=()=>{sample();const at=position(),resume=state.wanted;state.quality=$('quality').value;
retries=0;
try{localStorage.setItem('cordinal.quality',state.quality);}catch(e){}if(state.current){state.mode=streamMode(state.current);message('');loadSource(at,resume);report('seek');}};
let searchTimer;$('search').oninput=()=>{clearTimeout(searchTimer);searchTimer=setTimeout(()=>loadView(),180);};$('more').onclick=()=>loadView(true);
$('back').onclick=()=>{if(state.tab==='kept'){state.keptSet='';state.group='';loadView();return;}state.tab='library';state.group=state.album=state.artist=state.date='';loadView();};
function deviceName(){const a=navigator.userAgent;return /iPhone/.test(a)?'iPhone':/iPad/.test(a)?'iPad':/Android/.test(a)?'Android phone':
/Mac/.test(a)?'Mac':/Windows/.test(a)?'Windows PC':/Linux/.test(a)?'Linux PC':'Browser';}
$('pairForm').onsubmit=async e=>{e.preventDefault();const button=e.submitter;button.disabled=true;try{await api('pair',null,{code:$('code').value,name:$('deviceName').value||deviceName()});
$('pairing').close();$('code').value='';$('pairError').textContent='';message('');await tabs();await loadView();}catch(error){$('pairError').textContent=error.message;}finally{button.disabled=false;}};
$('pairing').addEventListener('cancel',e=>e.preventDefault());
$('logout').onclick=async()=>{finish(false);audio.pause();await api('logout',null,{});state.current=null;state.queue=[];state.items=[];state.peaks=[];
$('rows').replaceChildren();$('title').textContent='cordinal';$('subtitle').textContent='Your library, on this device';$('cover').hidden=true;$('emptyCover').hidden=false;$('pairing').showModal();};
document.addEventListener('visibilitychange',()=>{if(document.hidden&&state.started){sample();report('progress');}else if(!document.hidden){refreshThemes();updatePosition();}});
window.addEventListener('pagehide',()=>{sample();if(state.started&&state.mode==='kept'){logPlay(false);state.started=false;}if(state.started){const packet=new URLSearchParams({session:state.session,seq:String(++state.sequence),id:state.current.id,event:'ended',heard:state.heard.toFixed(3)});
fetch('/api/v1/listen',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:packet,keepalive:true}).catch(()=>{});}});
document.addEventListener('keydown',e=>{if(e.code==='Space'&&!['INPUT','SELECT','TEXTAREA','BUTTON'].includes(e.target.tagName)&&!$('pairing').open){e.preventDefault();togglePlay();}});
$('awayLink').onclick=async()=>{try{location.href=(await api('away',null,{})).url;}catch(e){message(e.message);}};
)js" R"js(// Keep on this phone: an album's or playlist's tracks saved in the browser's cache, played by the
// service worker (sw.js) without a connection. Browsers allow that only on an https address.
const canKeep=isSecureContext&&'serviceWorker'in navigator&&'caches'in window;let kept={tracks:{},sets:{}},keeping=false;
if(canKeep)navigator.serviceWorker.register('/sw.js').catch(()=>{});$('offlineHint').hidden=canKeep;
const keptCache=()=>caches.open('cordinal-kept');
async function loadKept(){if(!canKeep)return;try{const r=await(await keptCache()).match('/offline/index.json');if(r)kept=await r.json();}catch(e){}}
async function saveKept(){await(await keptCache()).put('/offline/index.json',new Response(JSON.stringify(kept),{headers:{'Content-Type':'application/json'}}));}
function setId(){return state.tab==='kept'?state.keptSet:state.group==='album'?'album:'+[state.artist,state.album,state.date].join('\u0001'):'playlist:'+state.tab;}
function keepable(){return canKeep&&(state.tab==='kept'?!!state.keptSet:state.group==='album'||(!state.group&&!['library','queue','recent'].includes(state.tab)));}
function crumb(){const list=!state.group&&keepable();$('crumb').hidden=!state.group&&!list;$('back').hidden=!state.group;
$('location').textContent=state.tab==='kept'?(kept.sets[state.keptSet]?.name||''):list?($('tabs').querySelector('[aria-current=page]')?.textContent||''):
state.album||state.artist||(state.group==='album'?'Unknown album':'Unknown artist');
$('keep').hidden=!keepable();$('keep').textContent=kept.sets[setId()]?'Kept on this phone · Remove':'Keep on this phone';}
function renderKept(){const grid=text('div','','cards');for(const [id,set]of Object.entries(kept.sets)){const b=text('button','','card');
b.append(artwork(set.ids[0]||''),text('strong',set.name),text('small',set.ids.length+' tracks'));b.onclick=()=>{state.keptSet=id;state.group='kept';loadView();};grid.append(b);}
if(!grid.children.length)grid.append(text('div','Nothing is kept on this phone yet. Open an album or playlist and choose Keep on this phone.','empty'));
state.items=[];state.total=Object.keys(kept.sets).length;$('rows').replaceChildren(grid);$('more').hidden=true;$('summary').textContent=state.total+' kept on this phone';}
// 128 kb/s AAC where cordinal can make it (about 60 MB an hour), else the original file the browser plays.
function keepFormat(t){return state.dataSaver?{format:'aac',bitrate:128}:t.original&&audio.canPlayType(t.mime)?{format:'original'}:null;}
async function removeKept(id){delete kept.sets[id];const used=new Set(Object.values(kept.sets).flatMap(s=>s.ids)),c=await keptCache();
for(const t of Object.keys(kept.tracks))if(!used.has(t)){await c.delete('/offline/audio/'+t);await c.delete('/offline/art/'+t);delete kept.tracks[t];}
await saveKept();if(state.tab==='kept'){state.keptSet='';state.group='';}}
$('keep').onclick=async()=>{if(keeping)return;const id=setId(),name=$('location').textContent;
if(kept.sets[id]){await removeKept(id);message('Removed from this phone.');loadView();return;}
keeping=true;$('keep').disabled=true;const ids=[];let skipped=0;
try{if(navigator.storage&&navigator.storage.persist)await navigator.storage.persist().catch(()=>false);
const list=await allTracks(currentQuery());if(state.dataSaver&&navigator.storage&&navigator.storage.estimate){const e=await navigator.storage.estimate(),need=list.reduce((n,t)=>n+(kept.tracks[t.id]?0:t.duration*16000),0);
if(e.quota&&e.quota-e.usage<need)throw Error('This phone has no room for '+Math.ceil(need/1e6)+' MB more of music.');}
const c=await keptCache();for(const t of list){const f=keepFormat(t);if(!kept.tracks[t.id]){if(!f||t.duration>3600){++skipped;continue;}
message('Keeping '+(ids.length+skipped+1)+' of '+list.length+' on this phone…');
const r=await fetch('/api/v1/audio?'+new URLSearchParams({id:t.id,start:0,keep:1,...f}),{credentials:'same-origin'});
if(!r.ok)throw Error((await r.json().catch(()=>({}))).error||'A track could not be downloaded.');
await c.put('/offline/audio/'+t.id,new Response(await r.blob(),{headers:{'Content-Type':r.headers.get('Content-Type')||'audio/aac'}}));
const art=await fetch('/api/v1/art?id='+encodeURIComponent(t.id),{credentials:'same-origin'}).catch(()=>null);if(art&&art.ok)await c.put('/offline/art/'+t.id,art);
kept.tracks[t.id]={...t,keptAs:f.format==='aac'?'AAC · 128 kb/s':t.codec};}ids.push(t.id);}
message('Kept '+ids.length+' tracks on this phone'+(skipped?'; '+skipped+' could not be kept (over an hour, or not playable in this browser).':'.'));}
catch(e){message(e.name==='QuotaExceededError'?'This phone is out of room for more music.':e.message);}
finally{if(ids.length)kept.sets[id]={name,ids};await saveKept().catch(()=>{});keeping=false;$('keep').disabled=false;keptTab();crumb();}};
// Each play of a kept copy is logged on the phone with the time it ended and sent in batches;
// cordinal counts each once, and only for tracks this browser downloaded to keep.
function storedPlays(){try{return JSON.parse(localStorage.getItem('cordinal.plays')||'[]');}catch(e){return[];}}
function savePlays(plays){try{localStorage.setItem('cordinal.plays',JSON.stringify(plays.slice(-2000)));}catch(e){}}
function logPlay(manual){if(state.heard<.5||!state.current)return;const plays=storedPlays();
plays.push({id:key(),track:state.current.id,end:Math.floor(Date.now()/1000),heard:state.heard.toFixed(1),manual:manual?1:0});savePlays(plays);flushPlays();}
let flushing=false;
async function flushPlays(){if(flushing)return;flushing=true;try{for(let plays=storedPlays();plays.length;plays=storedPlays()){const batch=plays.slice(0,30);
await api('played',null,{plays:batch.map(p=>[p.id,p.track,p.end,p.heard,p.manual].join('_')).join('*')});
const sent=new Set(batch.map(p=>p.id));savePlays(storedPlays().filter(p=>!sent.has(p.id)));}}catch(e){}finally{flushing=false;}}
let revision=-1;
async function refreshLibrary(){if($('pairing').open)return;try{const data=await api('state');
$('qualitySetting').hidden=!data.dataSaver;state.dataSaver=data.dataSaver;if(state.offline){state.offline=false;revision=-1;message('');}flushPlays();
$('awayLink').hidden=!data.away||new URL(data.away).origin===location.origin;if(!data.ready){message('Preparing the library…');return;}
if(data.revision!==revision){revision=data.revision;await tabs();if(state.tab!=='queue'&&state.tab!=='recent')await loadView();if(state.notice==='Preparing the library…')message('');}}catch(e){}}
async function init(){const link=new URLSearchParams(location.hash.slice(1)).get('pair');if(link){history.replaceState(null,'',location.pathname);
try{await api('pair',null,{link,name:deviceName()});message('This browser is paired with cordinal.');}catch(e){message(e.message);}}
await loadKept();await refreshThemes();try{await tabs();await loadView();await refreshLibrary();}catch(e){
// No connection: what is kept on this phone still plays.
if(e instanceof TypeError&&Object.keys(kept.sets).length){state.offline=true;$('tabs').replaceChildren(makeTab('kept','On this phone'),makeTab('queue','Now Playing'));
state.tab='kept';state.keptSet='';state.group='';loadView();message('No connection to cordinal: playing music kept on this phone.');}else if(!$('pairing').open)message(e.message);}}
init();setInterval(()=>{refreshThemes();refreshLibrary();
// Preparing a long compressed track precedes its metadata; normal playback still recovers promptly.
if(state.wanted&&state.waitSince&&performance.now()-state.lastAdvance>(state.loadingAt===null?25000:120000))reconnect();},5000);
)js";
}
// Network first for the page and its theme list, kept in a cache for when there is none; kept music
// only ever from the cache, answering the byte ranges a media element asks for so seeking works.
std::string_view ServiceWorker() {
    return R"js('use strict';
const SHELL='cordinal-shell',KEPT='cordinal-kept',PAGE=['/','/app.js','/style.css','/manifest.webmanifest','/icon.png','/deltarune.ttf','/api/v1/themes'];
self.addEventListener('install',e=>{self.skipWaiting();e.waitUntil(caches.open(SHELL).then(c=>c.addAll(PAGE.slice(0,5))).catch(()=>{}));});
self.addEventListener('activate',e=>e.waitUntil(self.clients.claim()));
async function ranged(request,response){const blob=await response.blob(),size=blob.size,type=response.headers.get('Content-Type')||'audio/aac';
const m=/^bytes=(\d*)-(\d*)$/.exec(request.headers.get('Range')||'');if(!m||(m[1]===''&&m[2]===''))return new Response(blob,{headers:{'Content-Type':type,'Content-Length':String(size),'Accept-Ranges':'bytes'}});
let start=m[1]===''?Math.max(0,size-Number(m[2])):Number(m[1]),end=m[1]!==''&&m[2]!==''?Math.min(Number(m[2]),size-1):size-1;
if(start>end)return new Response(null,{status:416,headers:{'Content-Range':'bytes */'+size}});
return new Response(blob.slice(start,end+1),{status:206,headers:{'Content-Type':type,'Content-Length':String(end-start+1),'Content-Range':'bytes '+start+'-'+end+'/'+size,'Accept-Ranges':'bytes'}});}
self.addEventListener('fetch',e=>{const url=new URL(e.request.url);if(url.origin!==location.origin||e.request.method!=='GET')return;
if(url.pathname.startsWith('/offline/')){e.respondWith(caches.open(KEPT).then(c=>c.match(url.pathname)).then(r=>!r?new Response('',{status:404}):
url.pathname.startsWith('/offline/audio/')?ranged(e.request,r):r));return;}
const page=PAGE.includes(url.pathname),art=url.pathname==='/api/v1/art';if(!page&&!art)return;
e.respondWith(fetch(e.request).then(r=>{if(page&&r.ok){const copy=r.clone();caches.open(SHELL).then(c=>c.put(url.pathname,copy));}return r;}).catch(async()=>
(page?await caches.match(url.pathname,{cacheName:SHELL}):await(await caches.open(KEPT)).match('/offline/art/'+url.searchParams.get('id')))||Response.error()));});
)js";
}
}  // namespace web
