/* the page's look, per browser: page colour, the favourite symbol and its colour. The choices come from the Disc
 * (api/look: look.conf on the card, which the host edits on the system page); this browser keeps what it picked.
 * ACC: the page colours until api/look answers (and for picks saved before 2026-11-24, by their old names). */
function cap(s){return typeof s==='string'?s.charAt(0).toUpperCase()+s.slice(1):''}
var LK={acc:'Orange',fav:'',fc:'',sc:null},ACC={Orange:['#ff9f43','#d35f00'],Blue:['#4da3ff','#0a66d1'],Green:['#3ccf7a','#1a8f4c'],Purple:['#b48cff','#7a4fd6'],Red:['#ff6b6b','#cc2b2b']};
var LOOK={symbols:['\u2764\ufe0f'],colors:[{name:'Red',dark:'#ff4d5e',light:'#d61f3b'}]};   /* until api/look answers */
try{var lk=JSON.parse(localStorage.getItem('look')||'{}');if(lk.acc)LK.acc=cap(lk.acc);
LK.fav=lk.fav==='heart'?'\u2764\ufe0f':lk.fav==='star'?'\u2b50':typeof lk.fav==='string'?lk.fav:'';   /* before 2026-11-23: names */
LK.fc=cap(lk.fc);if(lk.sc&&lk.sc.page)LK.sc=lk.sc}catch(e){}
function favsym(){return LOOK.symbols.indexOf(LK.fav)>=0?LK.fav:LOOK.symbols[0]}
function favcol(){for(var i=0;i<LOOK.colors.length;i++)if(LOOK.colors[i].name===LK.fc)return LOOK.colors[i];return LOOK.colors[0]}
function acccol(){for(var i=0;i<LOOK.colors.length;i++)if(LOOK.colors[i].name===LK.acc)return LOOK.colors[i];var a=ACC[LK.acc]||ACC.Orange;return{name:LK.acc,dark:a[0],light:a[1]}}
/* a colour scheme (LK.sc, a copy of the look.conf entry so it shows before api/look answers) sets the page's own colours;
 * the light/dark variants of the favourite colour follow its page colour */
function lum(h){var n=parseInt(h.slice(1),16);return((n>>16)*299+(n>>8&255)*587+(n&255)*114)/255000}
function look(){var r=document.documentElement,d=r.style,sc=LK.sc,lt=sc?(lum(sc.page)>.5?1:0):matchMedia('(prefers-color-scheme: light)').matches?1:0;
r.classList.toggle('sch',!!sc);['--frame','--bg','--fg','--mut','--card','--line'].forEach(function(k){d.removeProperty(k)});
if(sc){d.setProperty('--frame',sc.frame);d.setProperty('--bg',sc.page);d.setProperty('--fg',sc.text);d.setProperty('--mut',sc.muted);
d.setProperty('--card','color-mix(in srgb,'+sc.text+' 9%,'+sc.page+')');d.setProperty('--line','color-mix(in srgb,'+sc.text+' 18%,'+sc.page+')');r.style.colorScheme=lt?'light':'dark'}else d.removeProperty('color-scheme');
d.setProperty('--acc',sc?sc.link:acccol()[lt?'light':'dark']);
d.setProperty('--fav',favcol()[lt?'light':'dark']);try{localStorage.setItem('look',JSON.stringify(LK))}catch(e){}
[].forEach.call(document.querySelectorAll('.fa'),function(b){star(b,b.dataset.on==='1')})}
var $=function(i){return document.getElementById(i)};
function fm(ms){var s=Math.floor(ms/1000);return Math.floor(s/60)+':'+('0'+s%60).slice(-2)}
function enc(o){return Object.keys(o).map(function(k){return k+'='+encodeURIComponent(o[k])}).join('&')}
var mt;function say(t,ok){var m=$('msg');m.textContent=t||'';m.className=ok?'':'err';clearTimeout(mt);if(ok)mt=setTimeout(function(){say('')},2500)}
function cmdj(c,b){return fetch('api/'+c,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b||''})
.then(function(r){return r.json().then(function(j){say(j.ok?'':(j.message||'Not possible right now'));setTimeout(refresh,300);return j})})
.catch(function(){say('Player not reachable');return{}})}
function cmd(c,b){return cmdj(c,b).then(function(j){return !!j.ok})}
function star(b,on){b.classList.add('fa');b.dataset.on=on?'1':'0';b.textContent=favsym();b.classList.toggle('off',!on);b.setAttribute('aria-label',on?'Remove from favourites':'Save to favourites')}
/* edits: a bottom sheet of actions; prompt/confirm for names and deletes */
function sheet(t,acts){var s=$('sh');s.textContent='';var h=document.createElement('b');h.textContent=t;s.appendChild(h);
acts.concat([['Cancel',null]]).forEach(function(a){var b=document.createElement('button');b.textContent=a[0];b.onclick=function(){shut();if(a[1])a[1]()};s.appendChild(b)});
s.classList.remove('hide');$('shb').classList.remove('hide')}
/* After the list ends (own player): the station (a radio favourite) or a playlist, played once, again and again, or shuffled */
var PLF=['once','again and again','again and again','shuffled','shuffled'];function lesay(le){return le.mode===2?(le.station?'Station: '+le.station:'No station chosen yet'):le.mode===3?(le.pl_id?'Playlist: '+(le.playlist||'(deleted)')+', '+PLF[le.pl_flags&2?3:le.pl_flags]:'No playlist chosen yet'):''}
function lpick(m,then){fetch('api/library/'+(m==3?'playlists':'radio')+'?limit=500',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){var a=(j.items||[]).map(function(x){return[x.name,function(){
if(m==3)sheet(x.name,[['In order, once',0],['In order, again and again',1],['Shuffled',2]].map(function(f){return[f[0],function(){cmd('list_end',enc({mode:3,pl:x.id,flags:f[1]})).then(function(ok){if(ok)say('Plays '+x.name+' after the list',1);if(then)then()})}]}));
else cmd('list_end',enc({mode:2,pos:x.pos,url:x.url})).then(function(ok){if(ok)say('Plays '+x.name+' after the list',1);if(then)then()})}]});
if(!a.length){say(m==3?'No playlists yet':'No radio favourites yet - save stations in the Radio tab');return}sheet(m==3?'Playlist after the list':'Station after the list',a)}).catch(function(){say('Player not reachable')})}
/* a stream address of your own (not in the station directory): its name and URL */
function askurl(then){var u=prompt('Stream address (http:// or https://)','https://');if(!u||!/^https?:\/\/./i.test(u.trim())){if(u)say('That is not a stream address (http:// or https://)');return}
var n=prompt('Name of the station',u.trim().replace(/^https?:\/\//i,'').split(/[/?]/)[0]);if(!n||!n.trim())return;then(n.trim(),u.trim())}
function shut(){$('sh').classList.add('hide');$('shb').classList.add('hide')}
/* Public modes (Jukebox, Hit&Guess and every later game; Jo, 2026-11-22): a guest has ONE name for all of them.
 * Each mode's guest page reads it with gname() and stores it with gname_set(); moved() reloads the page when the
 * host switched the mode (the server answers "... reload the page"), so the guest lands in the new mode by name. */
function gname(){try{return localStorage.getItem('pn')||localStorage.getItem('hn')||''}catch(e){return''}}
function gname_set(n){try{localStorage.setItem('pn',n);localStorage.removeItem('hn')}catch(e){}}
function moved(m){if(!/reload the page/.test(m||''))return 0;var t=0;try{t=+sessionStorage.getItem('mv')||0;sessionStorage.setItem('mv',Date.now())}catch(e){}
if(Date.now()-t>5000)location.reload();return 1}   /* at most one reload in 5 s: never a reload loop */
/* the choices; the selects #lkf (the symbols as they are) and #lkc (the colours' names) when the page has them */
function fillsel(){var f=$('lkf'),c=$('lkc'),a=$('lka'),m=$('lkm'),ss=LOOK.schemes||[];
if(LK.sc){var k=ss.filter(function(x){return x.name===LK.sc.name})[0];LK.sc=k||null}   /* the card's colours win; a removed scheme ends */
if(m){m.textContent='';[{name:''}].concat(ss).forEach(function(x){var o=document.createElement('option');o.value=x.name;o.textContent=x.name||'Dark or light (as the device)';m.appendChild(o)});
m.value=LK.sc?LK.sc.name:'';m.onchange=function(){LK.sc=ss.filter(function(x){return x.name===m.value})[0]||null;if(a)a.disabled=!!LK.sc;look()}}if(a)a.disabled=!!LK.sc;
if(a){a.textContent='';LOOK.colors.forEach(function(x){var o=document.createElement('option');o.value=o.textContent=x.name;a.appendChild(o)});
if(LOOK.colors.every(function(x){return x.name!==LK.acc})){var o=document.createElement('option');o.value=o.textContent=LK.acc;a.appendChild(o)}   /* an earlier pick stays */
a.value=LK.acc;a.onchange=function(){LK.acc=a.value;look()}}if(f){f.textContent='';LOOK.symbols.forEach(function(s){var o=document.createElement('option');o.value=o.textContent=s;f.appendChild(o)});f.value=favsym();
f.onchange=function(){LK.fav=f.value;look();if(typeof qver!=='undefined')qver=-1}}
if(c){c.textContent='';LOOK.colors.forEach(function(x){var o=document.createElement('option');o.value=o.textContent=x.name;c.appendChild(o)});c.value=favcol().name;c.onchange=function(){LK.fc=c.value;look()}}}
function looks(then){fetch('api/look',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){if(!j.ok||!j.symbols.length||!j.colors.length)return;LOOK=j;fillsel();look();if(then)then(j)}).catch(function(){})}
look();looks();try{matchMedia('(prefers-color-scheme: light)').addEventListener('change',look)}catch(e){}
