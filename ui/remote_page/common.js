/* the page's look, per browser: accent colour and the favourite symbol (red hearts or yellow stars) */
var LK={acc:'orange',fav:'heart'},ACC={orange:['#ff9f43','#d35f00'],blue:['#4da3ff','#0a66d1'],green:['#3ccf7a','#1a8f4c'],purple:['#b48cff','#7a4fd6'],red:['#ff6b6b','#cc2b2b']};
try{var lk=JSON.parse(localStorage.getItem('look')||'{}');if(ACC[lk.acc])LK.acc=lk.acc;if(lk.fav==='star')LK.fav='star'}catch(e){}
function look(){var lt=matchMedia('(prefers-color-scheme: light)').matches?1:0,d=document.documentElement.style;d.setProperty('--acc',ACC[LK.acc][lt]);
d.setProperty('--fav',LK.fav==='star'?['#ffcc33','#c99700'][lt]:['#ff4d5e','#d61f3b'][lt]);try{localStorage.setItem('look',JSON.stringify(LK))}catch(e){}
[].forEach.call(document.querySelectorAll('.fa'),function(b){star(b,/9733|9829|\u2605|\u2665/.test(b.innerHTML)||b.innerHTML==='\u2605'||b.innerHTML==='\u2665')})}
var $=function(i){return document.getElementById(i)};
function fm(ms){var s=Math.floor(ms/1000);return Math.floor(s/60)+':'+('0'+s%60).slice(-2)}
function enc(o){return Object.keys(o).map(function(k){return k+'='+encodeURIComponent(o[k])}).join('&')}
var mt;function say(t,ok){var m=$('msg');m.textContent=t||'';m.className=ok?'':'err';clearTimeout(mt);if(ok)mt=setTimeout(function(){say('')},2500)}
function cmdj(c,b){return fetch('api/'+c,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b||''})
.then(function(r){return r.json().then(function(j){say(j.ok?'':(j.message||'Not possible right now'));setTimeout(refresh,300);return j})})
.catch(function(){say('Player not reachable');return{}})}
function cmd(c,b){return cmdj(c,b).then(function(j){return !!j.ok})}
function star(b,on){b.classList.add('fa');b.innerHTML=LK.fav==='star'?(on?'&#9733;':'&#9734;'):(on?'&#9829;':'&#9825;');b.setAttribute('aria-label',on?'Remove from favourites':'Save to favourites')}
/* edits: a bottom sheet of actions; prompt/confirm for names and deletes */
function sheet(t,acts){var s=$('sh');s.textContent='';var h=document.createElement('b');h.textContent=t;s.appendChild(h);
acts.concat([['Cancel',null]]).forEach(function(a){var b=document.createElement('button');b.textContent=a[0];b.onclick=function(){shut();if(a[1])a[1]()};s.appendChild(b)});
s.classList.remove('hide');$('shb').classList.remove('hide')}
function shut(){$('sh').classList.add('hide');$('shb').classList.add('hide')}
look();try{matchMedia('(prefers-color-scheme: light)').addEventListener('change',look)}catch(e){}
