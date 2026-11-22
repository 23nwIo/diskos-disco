/* the page's look, per browser: accent colour, the favourite symbol and its colour (each colour: dark, light theme) */
var LK={acc:'orange',fav:'heart',fc:'red'},ACC={orange:['#ff9f43','#d35f00'],blue:['#4da3ff','#0a66d1'],green:['#3ccf7a','#1a8f4c'],purple:['#b48cff','#7a4fd6'],red:['#ff6b6b','#cc2b2b']};
var SYM={heart:['\u2665','\u2661'],star:['\u2605','\u2606'],diamond:['\u25c6','\u25c7'],circle:['\u25cf','\u25cb'],flower:['\u273f','\u2740']};
var FCOL={red:['#ff4d5e','#d61f3b'],yellow:['#ffcc33','#c99700'],orange:['#ff9f43','#d35f00'],pink:['#ff7eb6','#d6337a'],purple:['#b48cff','#7a4fd6'],blue:['#4da3ff','#0a66d1'],green:['#3ccf7a','#1a8f4c'],white:['#f2f2f2','#333333']};
try{var lk=JSON.parse(localStorage.getItem('look')||'{}');if(ACC[lk.acc])LK.acc=lk.acc;if(SYM[lk.fav])LK.fav=lk.fav;
if(FCOL[lk.fc])LK.fc=lk.fc;else if(lk.fav==='star')LK.fc='yellow'}catch(e){}   /* before 2026-11-22: hearts were red, stars yellow */
function look(){var lt=matchMedia('(prefers-color-scheme: light)').matches?1:0,d=document.documentElement.style;d.setProperty('--acc',ACC[LK.acc][lt]);
d.setProperty('--fav',FCOL[LK.fc][lt]);try{localStorage.setItem('look',JSON.stringify(LK))}catch(e){}
[].forEach.call(document.querySelectorAll('.fa'),function(b){star(b,b.dataset.on==='1')})}
var $=function(i){return document.getElementById(i)};
function fm(ms){var s=Math.floor(ms/1000);return Math.floor(s/60)+':'+('0'+s%60).slice(-2)}
function enc(o){return Object.keys(o).map(function(k){return k+'='+encodeURIComponent(o[k])}).join('&')}
var mt;function say(t,ok){var m=$('msg');m.textContent=t||'';m.className=ok?'':'err';clearTimeout(mt);if(ok)mt=setTimeout(function(){say('')},2500)}
function cmdj(c,b){return fetch('api/'+c,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b||''})
.then(function(r){return r.json().then(function(j){say(j.ok?'':(j.message||'Not possible right now'));setTimeout(refresh,300);return j})})
.catch(function(){say('Player not reachable');return{}})}
function cmd(c,b){return cmdj(c,b).then(function(j){return !!j.ok})}
function star(b,on){b.classList.add('fa');b.dataset.on=on?'1':'0';b.textContent=SYM[LK.fav][on?0:1];b.setAttribute('aria-label',on?'Remove from favourites':'Save to favourites')}
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
look();try{matchMedia('(prefers-color-scheme: light)').addEventListener('change',look)}catch(e){}
