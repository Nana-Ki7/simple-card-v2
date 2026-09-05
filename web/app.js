'use strict';
/* SimpleCardGame v2 前端逻辑（GLM 版）
   协议：ws://HOST:9002 JSON，card_id 0..51 suit=id/13(0♣1♦2♥3♠) rank=id%13+3 */

const LS_TOKEN = 'nanaki_token';
const LS_NAME  = 'nanaki_name';
const CARD_H2 = 38, CARD_SK = 49;
const SUIT_CH = ['♣','♦','♥','♠'];
const RANK_CH = { 11:'J', 12:'Q', 13:'K', 14:'A', 15:'2' };

const $  = id => document.getElementById(id);
const qs = k => new URLSearchParams(location.search).get(k);
const suitOf = id => Math.floor(id/13);
const rankOf = id => id%13+3;
// 排序：先按点数，同点数再按花色（♣<♦<♥<♠）
const cmpCard = (a,b) => { const ra=rankOf(a), rb=rankOf(b); return ra!==rb ? ra-rb : suitOf(a)-suitOf(b); };
const rankText = r => RANK_CH[r]||String(r);
const cardName = id => SUIT_CH[suitOf(id)]+rankText(rankOf(id));
const isRedSuit = s => s===1||s===2;
function escapeHtml(s){ return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
function parseRoom(v){ if(v==null)return null; const s=String(v).trim(); return /^\d{1,9}$/.test(s)?Number(s):null; }
function setUrlRoom(rid){ const u=new URL(location.href); if(rid==null)u.searchParams.delete('room'); else u.searchParams.set('room',String(rid)); history.replaceState(null,'',u); }

const S = {
  ws:null, connected:false,
  token: localStorage.getItem(LS_TOKEN)||null,
  name:  localStorage.getItem(LS_NAME)||'',
  roomId:null, mySeat:null, phase:'idle',
  players:{},
  hand:[], selected:new Set(),
  turnSeat:null, roundSeq:0, canPass:false,
  handCounts:[0,0,0,0], deadline:0,
  lastPlay:null, passedSeats:new Set(), finishedSeats:new Set(),
  revealed:{}, teams:null,
  backoff:1000, reconnectTimer:null, closedByUs:false,
};

function showView(name){
  document.querySelectorAll('.view').forEach(v=>v.classList.remove('active'));
  const el=$('view-'+name); if(el)el.classList.add('active');
}
let toastTimer=null;
function toast(msg){ const t=$('toast'); t.textContent=msg; t.classList.add('show'); clearTimeout(toastTimer); toastTimer=setTimeout(()=>t.classList.remove('show'),2600); }
function setStatus(state){
  const txt={connecting:'连接中…',online:'已连接',offline:'连接断开',reconnecting:'重连中…'}[state]||state;
  $('conn-dot').className='dot '+state; $('conn-text').textContent=txt;
}
function setConnRoom(){ $('conn-room').textContent = S.roomId!=null?'房间 '+S.roomId:''; }
function seatName(seat){ const p=S.players[seat]; if(p&&p.name)return p.name; if(seat===S.mySeat&&S.name)return S.name; return '玩家'+(seat+1); }

function wsUrl(){ const host=qs('host')||location.hostname||'127.0.0.1'; return 'ws://'+host+':9002'; }
function connect(){
  clearTimeout(S.reconnectTimer);
  try{ if(S.ws){ S.ws.onopen=S.ws.onmessage=S.ws.onclose=S.ws.onerror=null; S.ws.close(); } }catch(e){}
  setStatus(S.backoff>1000?'reconnecting':'connecting');
  let ws; try{ ws=new WebSocket(wsUrl()); }catch(e){ scheduleReconnect(); return; }
  S.ws=ws;
  ws.onopen=()=>{ send({type:'hello', token:S.token||undefined, name:S.name}); };
  ws.onmessage=ev=>{ let m=null; try{m=JSON.parse(ev.data);}catch(e){return;} if(m&&m.type)handleMsg(m); };
  ws.onerror=()=>{ try{ws.close();}catch(e){} };
  ws.onclose=()=>{ S.connected=false; setStatus('offline'); scheduleReconnect(); };
}
function scheduleReconnect(){ if(S.closedByUs)return; setStatus('reconnecting'); S.reconnectTimer=setTimeout(connect,S.backoff); S.backoff=Math.min(S.backoff*2,10000); }
function send(obj){ if(S.ws&&S.ws.readyState===WebSocket.OPEN){ S.ws.send(JSON.stringify(obj)); return true; } return false; }

function handleMsg(m){
  switch(m.type){
    case 'welcome': onWelcome(m); break;
    case 'room_state': onRoomState(m); break;
    case 'room_update': onRoomUpdate(m); break;
    case 'game_started': onGameStarted(m); break;
    case 'your_hand': onYourHand(m); break;
    case 'last_play_sync': onLastPlaySync(m); break;
    case 'turn_update': onTurnUpdate(m); break;
    case 'play_made': onPlayMade(m); break;
    case 'pass_made': onPassMade(m); break;
    case 'reveal': onReveal(m); break;
    case 'hand_empty': onHandEmpty(m); break;
    case 'round_end': onRoundEnd(m); break;
    case 'game_over': onGameOver(m); break;
    case 'player_conn': onPlayerConn(m); break;
    case 'bots_added': onBotsAdded(m); break;
    case 'left_room': onLeftRoom(m); break;
    case 'error': onError(m); break;
  }
}
function onWelcome(m){
  S.connected=true; S.backoff=1000; setStatus('online');
  if(m.token){ S.token=m.token; localStorage.setItem(LS_TOKEN,m.token); }
  if(m.name){ S.name=m.name; localStorage.setItem(LS_NAME,m.name); if($('name-input').value!==m.name)$('name-input').value=m.name; }
  if(S.roomId==null){
    const urlRoom=parseRoom(qs('room'));
    if(urlRoom!=null){ $('room-input').value=urlRoom; showView('lobby');
      setTimeout(()=>{ if(S.roomId==null&&S.connected)send({type:'join_room',room_id:urlRoom}); },350);
    } else showView('lobby');
  }
}
function onRoomState(m){
  S.roomId=m.room_id; S.mySeat=m.seat; S.phase=m.phase||'waiting';
  setConnRoom(); setUrlRoom(S.roomId);
  if(!S.players[S.mySeat]) S.players[S.mySeat]={name:S.name||('玩家'+(S.mySeat+1)),connected:true};
  if(S.phase==='playing')showView('table'); else showView('waiting');
  renderWaiting(); renderTable();
}
function onRoomUpdate(m){ if(m.room_id!==S.roomId)return; S.players={}; (m.players||[]).forEach(p=>{ S.players[p.seat]={name:p.name,connected:p.connected!==false,bot:!!p.bot}; }); renderWaiting(); renderTable(); }
function onGameStarted(m){
  S.phase='playing'; S.passedSeats.clear(); S.finishedSeats.clear(); S.revealed={}; S.lastPlay=null; S.selected.clear(); S.teams=null;
  S.handCounts=[13,13,13,13]; hideResult(); showView('table');
  if(m.turn_seat!=null)setTurn(m.turn_seat,m.timeout_ms); renderTable();
}
function onYourHand(m){
  if(m.room_id!==S.roomId)return;
  S.mySeat=m.seat; S.hand=(m.hand||[]).slice().sort(cmpCard); S.selected.clear();
  if(m.turn_seat!=null)S.turnSeat=m.turn_seat;
  hideResult(); showView('table'); renderTable();
}
function onLastPlaySync(m){ if(m.room_id!==S.roomId)return; S.lastPlay={seat:m.seat,cards:(m.cards||[]).slice(),roundSeq:m.round_seq}; if(m.round_seq!=null)S.roundSeq=m.round_seq; renderTable(); }
function onTurnUpdate(m){
  if(m.room_id!==S.roomId)return;
  S.turnSeat=m.seat; if(m.round_seq!=null)S.roundSeq=m.round_seq; S.canPass=!!m.can_pass;
  if(Array.isArray(m.hand_counts))S.handCounts=m.hand_counts.slice();
  setTurn(m.seat,m.timeout_ms);
}
function setTurn(seat,timeoutMs){ S.turnSeat=seat; S.deadline=(timeoutMs!=null)?Date.now()+timeoutMs:0; renderTable(); }
function onPlayMade(m){
  if(m.room_id!==S.roomId)return;
  S.lastPlay={seat:m.seat,cards:(m.cards||[]).slice(),roundSeq:m.round_seq};
  if(m.round_seq!=null)S.roundSeq=m.round_seq;
  if(Array.isArray(m.hand_counts))S.handCounts=m.hand_counts.slice();
  if(m.seat===S.mySeat){ const played=new Set(m.cards||[]); S.hand=S.hand.filter(id=>!played.has(id)); S.selected.clear(); }
  S.passedSeats.delete(m.seat); S.deadline=0; renderTable();
}
function onPassMade(m){ if(m.room_id!==S.roomId)return; S.passedSeats.add(m.seat); renderTable(); }
function onReveal(m){
  if(m.room_id!==S.roomId)return;
  const id=m.kind==='H2'?CARD_H2:(m.kind==='SK'?CARD_SK:null);
  if(id!=null){ S.revealed[id]=m.seat; const who=(m.seat===S.mySeat)?'你':'「'+seatName(m.seat)+'」'; toast(cardName(id)+' 已落桌 — '+who+' 是关键队！'); }
  renderTable();
}
function onHandEmpty(m){ if(m.room_id!==S.roomId)return; S.finishedSeats.add(m.seat); if(m.seat===S.mySeat)toast('你已出完，等待队友…'); renderTable(); }
function onRoundEnd(m){
  if(m.room_id!==S.roomId)return;
  S.passedSeats.clear(); S.lastPlay=null;
  if(m.round_seq!=null)S.roundSeq=m.round_seq;
  if(m.leader!=null)S.turnSeat=m.leader; S.deadline=0; renderTable();
}
function onGameOver(m){ if(m.room_id!==S.roomId)return; S.phase='over'; S.deadline=0; if(Array.isArray(m.teams))S.teams=m.teams.slice(); showResult(m); }
function onPlayerConn(m){
  if(m.room_id!==S.roomId)return;
  if(!S.players[m.seat])S.players[m.seat]={name:seatName(m.seat)};
  S.players[m.seat].connected=!!m.connected;
  renderWaiting(); renderTable();
  if(m.seat===S.mySeat)toast(m.connected?'连接已恢复':'连接已断开，座位为你保留');
}
function onLeftRoom(m){ if(m.room_id!=null&&S.roomId!=null&&m.room_id!==S.roomId)return; resetRoomState(); setUrlRoom(null); setConnRoom(); showView('lobby'); toast('已离开房间'); }
function onBotsAdded(m){
  if(m.room_id!=null&&S.roomId!=null&&m.room_id!==S.roomId)return;
  if(Array.isArray(m.players))m.players.forEach(p=>{
    S.players[p.seat]={name:p.name,connected:true,bot:p.bot!==false};
  });
  renderWaiting();
  toast('已添加 AI 玩家 🤖');
}
function resetRoomState(){
  S.roomId=null; S.mySeat=null; S.phase='idle'; S.players={};
  S.hand=[]; S.selected.clear(); S.turnSeat=null; S.roundSeq=0; S.canPass=false;
  S.handCounts=[0,0,0,0]; S.deadline=0; S.lastPlay=null;
  S.passedSeats.clear(); S.finishedSeats.clear(); S.revealed={}; S.teams=null;
  hideResult(); setConnRoom();
}
function onError(m){
  const code=String(m.code||''), msg=m.message||'请求失败';
  if(code==='BAD_TOKEN'||code==='TOKEN_EXPIRED'||/token/i.test(code)){
    localStorage.removeItem(LS_TOKEN); S.token=null; resetRoomState(); showView('lobby');
    toast('登录状态失效，正在重新连接…'); connect(); return;
  }
  switch(code){
    case 'ALREADY_IN_ROOM': if(S.roomId!=null)return; toast('你已在一个房间中'); break;
    case 'ROOM_NOT_FOUND': toast('房间不存在或已解散'); if(S.roomId!=null){resetRoomState();showView('lobby');} setUrlRoom(null); break;
    case 'GAME_STARTED': toast('该房间已开局，无法加入'); break;
    case 'ROOM_FULL': toast('房间已满（4/4）'); break;
    case 'NOT_IN_ROOM': if(S.roomId!=null){resetRoomState();showView('lobby');} toast('你已不在房间中'); break;
    case 'NOT_YOUR_TURN': toast('还没轮到你'); break;
    case 'CANNOT_BEAT': toast('压不过上一手，换一手或选择不出'); break;
    case 'CARD_NOT_IN_HAND': toast('选中的牌不在你手里'); break;
    case 'PASSED_LOCKED': toast('本圈已选择不出，等待新一轮'); break;
    case 'LEADER_CANNOT_PASS': toast('你是领出位，必须出牌'); break;
    default: toast(msg);
  }
}
function posOfSeat(seat){ if(S.mySeat==null)return ['bottom','right','top','left'][seat]; return ['bottom','right','top','left'][(seat-S.mySeat+4)%4]; }
function seatAtPos(pos){ for(let s=0;s<4;s++)if(posOfSeat(s)===pos)return s; return null; }
function isKeySeat(seat){ if(S.teams)return S.teams[seat]===0; return S.revealed[CARD_H2]===seat||S.revealed[CARD_SK]===seat; }

function renderWaiting(){
  if(S.roomId==null)return;
  $('wait-room-id').textContent=S.roomId;
  const grid=$('seats-grid'); grid.innerHTML='';
  let botCount=0;
  for(let s=0;s<4;s++){
    const p=S.players[s]; const d=document.createElement('div');
    d.className='seat'+(p?(p.connected?'':' off'):' empty');
    if(p){ if(p.bot)botCount++;
      const nm=p.name||('玩家'+(s+1));
      const tag=s===S.mySeat?'<i class="you">你</i>':(p.bot?'<i class="bot-tag">🤖 AI</i>':'');
      d.innerHTML='<div class="s-ava">'+(p.bot?'🤖':escapeHtml(nm[0]||'?'))+'</div><div class="s-name">'+escapeHtml(nm)+tag+'</div><div class="s-state">'+(p.bot?'AI 玩家':(p.connected?'已就绪':'已断线'))+'</div>';
    } else d.innerHTML='<div class="s-ava">?</div><div class="s-name muted">空位</div><div class="s-state">等待加入</div>';
    grid.appendChild(d);
  }
  const n=Object.keys(S.players).length, hint=$('wait-hint');
  const aiTxt=botCount>0?'（含 AI）':'';
  hint.textContent=n>=4?('满 4 人！即将自动开局…'+aiTxt):('等待玩家加入…（'+n+'/4）'+aiTxt);
  hint.classList.toggle('ready',n>=4);
  updateBotBtn();
}
function renderTable(){ if(S.roomId==null)return; renderPlates(); renderCenter(); renderHand(); renderActions(); }
function renderPlates(){
  ['top','left','right','bottom'].forEach(pos=>{
    const el=$('plate-'+pos), seat=seatAtPos(pos);
    if(seat==null){ el.innerHTML=''; return; }
    const p=S.players[seat];
    const name=(p&&p.name)||(seat===S.mySeat?S.name:null)||('玩家'+(seat+1));
    const connected=p?p.connected!==false:true;
    const my=seat===S.mySeat;
    const count=(my&&S.hand.length>0)?S.hand.length:(S.handCounts[seat]||0);
    const badges=[];
    if(S.finishedSeats.has(seat))badges.push('<span class="b b-done">出完</span>');
    else if(S.passedSeats.has(seat))badges.push('<span class="b b-pass">PASS</span>');
    if(isKeySeat(seat))badges.push('<span class="b b-key">★ 关键队</span>');
    if(!connected)badges.push('<span class="b b-off">断线</span>');
    el.className='plate pos-'+pos+(S.turnSeat===seat?' turn':'')+(isKeySeat(seat)?' key':'')+(!connected?' off':'');
    el.innerHTML='<div class="p-top"><span class="p-name">'+escapeHtml(name)+(my?'<i class="you">你</i>':'')+'</span><span class="p-seat">#'+(seat+1)+'</span></div>'+
      '<div class="p-meta"><span class="p-count">'+count+' 张</span>'+badges.join('')+'</div>';
  });
}
function cardHTML(id){
  const s=suitOf(id);
  const key=(id===CARD_H2&&S.revealed[CARD_H2]!=null)||(id===CARD_SK&&S.revealed[CARD_SK]!=null);
  return '<div class="card'+(isRedSuit(s)?' red':'')+(key?' keycard':'')+'"><span class="cr">'+rankText(rankOf(id))+'</span><span class="cs">'+SUIT_CH[s]+'</span></div>';
}
function renderCenter(){
  $('round-info').textContent=(S.phase==='playing'||S.phase==='over')&&S.roundSeq>0?'第 '+S.roundSeq+' 轮':'';
  const cp=$('center-play');
  if(S.phase!=='playing'&&S.phase!=='over'){ cp.innerHTML=''; renderPill(); return; }
  if(S.lastPlay&&S.lastPlay.cards&&S.lastPlay.cards.length){
    const who=(S.lastPlay.seat===S.mySeat)?'你':seatName(S.lastPlay.seat);
    cp.innerHTML='<div class="cp-who">'+escapeHtml(who)+' 出的牌</div><div class="cp-cards">'+S.lastPlay.cards.slice().sort(cmpCard).map(cardHTML).join('')+'</div>';
  } else if(S.turnSeat!=null){
    const who=(S.turnSeat===S.mySeat)?'你':seatName(S.turnSeat);
    cp.innerHTML='<div class="cp-who muted">'+(S.roundSeq>1?'新一轮 · ':'')+'等待 '+escapeHtml(who)+' 领出</div>';
  } else cp.innerHTML='<div class="cp-who muted">等待开局…</div>';
  renderPill();
}
function renderPill(){
  const pill=$('turn-pill');
  if(S.phase==='over'){ pill.className='pill idle'; pill.textContent='本局结束'; return; }
  if(S.phase!=='playing'){ pill.className='pill idle'; pill.textContent=''; return; }
  let sec=null; if(S.deadline)sec=Math.max(0,Math.ceil((S.deadline-Date.now())/1000));
  if(sec!=null&&sec<=0){ pill.className='pill idle'; pill.textContent='等待服务器…'; return; }
  const cd=sec!=null?' · '+sec+'s':'';
  if(S.finishedSeats.has(S.mySeat)){ pill.className='pill idle'; pill.textContent='已出完 · 等待队友'; }
  else if(S.turnSeat===S.mySeat){ pill.className='pill myturn'; pill.textContent='轮到你'+cd; }
  else if(S.turnSeat!=null){ pill.className='pill'; pill.textContent='等待 '+seatName(S.turnSeat)+cd; }
  else { pill.className='pill idle'; pill.textContent='等待服务器…'; }
}
function renderHand(){
  const wrap=$('hand'); wrap.innerHTML=''; if(S.mySeat==null)return;
  const ids=S.hand, n=ids.length;
  const finished=S.finishedSeats.has(S.mySeat);
  $('fin-banner').classList.toggle('hidden',!finished);
  ids.forEach((id,i)=>{
    const off=i-(n-1)/2;
    const w=document.createElement('div');
    w.className='cardw'+(S.selected.has(id)?' picked':'');
    w.style.setProperty('--rot',(off*2).toFixed(2)+'deg');
    w.style.setProperty('--arc',(Math.abs(off)*Math.abs(off)*0.55).toFixed(1)+'px');
    w.style.zIndex=String(i+1);
    const mineKey=(id===CARD_H2&&S.revealed[CARD_H2]===S.mySeat)||(id===CARD_SK&&S.revealed[CARD_SK]===S.mySeat);
    const b=document.createElement('button');
    b.type='button';
    b.className='card'+(isRedSuit(suitOf(id))?' red':'')+(S.selected.has(id)?' sel':'')+(mineKey?' minekey':'');
    b.dataset.id=String(id);
    b.innerHTML='<span class="cr">'+rankText(rankOf(id))+'</span><span class="cs">'+SUIT_CH[suitOf(id)]+'</span>';
    b.addEventListener('click',()=>toggleSelect(id));
    w.appendChild(b); wrap.appendChild(w);
  });
}
function toggleSelect(id){
  if(S.phase!=='playing')return;
  if(S.finishedSeats.has(S.mySeat))return;
  if(S.selected.has(id))S.selected.delete(id); else S.selected.add(id);
  renderHand(); renderActions();
}
function renderActions(){
  const finished=S.finishedSeats.has(S.mySeat);
  $('action-bar').classList.toggle('hidden',finished&&S.phase==='playing');
  const my=S.phase==='playing'&&S.turnSeat===S.mySeat&&!finished;
  const play=$('btn-play'),pass=$('btn-pass');
  play.disabled=!(my&&S.selected.size>0);
  pass.disabled=!(my&&S.canPass);
  play.textContent=(my&&S.selected.size)?'出牌（'+S.selected.size+'）':'出牌';
}
function showResult(m){
  const isKeyWin=m.winner_team===0;
  $('result-title').textContent=isKeyWin?'关键队获胜！':'平民队获胜！';
  $('result-sub').textContent=isKeyWin?'♥2 + ♠K 阵营拿下本局':'平民阵营守住本局';
  const order=$('result-order'); order.innerHTML='';
  (m.finish_order||[]).forEach((seat,idx)=>{
    const li=document.createElement('li'); const key=Array.isArray(m.teams)?m.teams[seat]===0:false;
    li.className=key?'key':'';
    li.innerHTML='<span class="rk">'+(idx+1)+'</span> '+escapeHtml(seatName(seat))+'<span class="tag '+(key?'t-key':'t-civ')+'">'+(key?'关键队':'平民队')+'</span>';
    order.appendChild(li);
  });
  const t=$('result-teams'); t.innerHTML='';
  if(Array.isArray(m.teams))m.teams.forEach((team,seat)=>{
    const chip=document.createElement('span'); chip.className='team-chip '+(team===0?'t-key':'t-civ');
    chip.textContent=seatName(seat)+' · '+(team===0?'关键队':'平民队'); t.appendChild(chip);
  });
  $('overlay-result').classList.remove('hidden');
  renderPlates(); renderPill();
}
function hideResult(){ $('overlay-result').classList.add('hidden'); }

function readName(){ const v=$('name-input').value.trim(); if(!v){toast('请先输入昵称');$('name-input').focus();return null;} localStorage.setItem(LS_NAME,v); return v; }
function doCreate(){ if(!readName())return; if(!S.connected){toast('未连接到服务器，请稍候…');return;} send({type:'create_room'}); }
function doJoin(){
  if(!readName())return;
  const rid=parseRoom($('room-input').value);
  if(rid==null){toast('请输入正确的房间号');return;}
  if(!S.connected){toast('未连接到服务器，请稍候…');return;}
  send({type:'join_room',room_id:rid});
}
/* ===== 等待房“添加 AI 玩家” ===== */
function ensureWaitBotBtn(){
  let btn=$('btn-add-bot');
  if(!btn){
    btn=document.createElement('button');
    btn.id='btn-add-bot'; btn.type='button';
    btn.style.cssText='display:block;margin:10px auto 0;padding:10px 16px;';
    btn.textContent='➕ 添加 AI 玩家';
    btn.addEventListener('click',doAddBot);
    const anchor=$('wait-hint');
    if(anchor&&anchor.parentNode)anchor.insertAdjacentElement('afterend',btn);
    else { const v=$('view-waiting'); if(v)v.appendChild(btn); }
  }
  updateBotBtn();
}
function updateBotBtn(){
  const btn=$('btn-add-bot'); if(!btn)return;
  const inWaiting=S.roomId!=null&&S.phase==='waiting';
  btn.style.display=inWaiting?'block':'none';
  if(!inWaiting)return;
  const n=Object.keys(S.players).length;
  btn.disabled=(n>=4);
  btn.textContent=n>=4?'AI 已满员（4/4）':'➕ 添加 AI 玩家';
}
function doAddBot(){
  if(!S.connected){toast('未连接到服务器，请稍候…');return;}
  if(S.phase!=='waiting'){toast('对局已开始，无法添加 AI');return;}
  if(Object.keys(S.players).length>=4){toast('房间已满（4/4）');return;}
  send({type:'add_bot',count:1});
}
function doCreateWithBots(){
  if(!readName())return;
  if(!S.connected){toast('未连接到服务器，请稍候…');return;}
  send({type:'create_room',bots:3});
}
function setupCreateBotsBtn(){
  let b=$('btn-create-bots');
  if(!b){
    b=document.createElement('button');
    b.id='btn-create-bots'; b.type='button';
    b.style.cssText='margin-left:8px;';
    b.textContent='创建房间 + 3 AI';
    b.addEventListener('click',doCreateWithBots);
    const c=$('btn-create');
    if(c&&c.parentNode)c.insertAdjacentElement('afterend',b);
  }
}
function doPlay(){
  if(S.phase!=='playing'||S.turnSeat!==S.mySeat)return;
  if(S.finishedSeats.has(S.mySeat))return;
  const cards=[...S.selected];
  if(!cards.length){toast('请先选择要出的牌');return;}
  for(const c of cards){ if(!S.hand.includes(c)){toast('选中的牌不在你手里');return;} }
  send({type:'play',cards});
}
function doPass(){
  if(S.phase!=='playing'||S.turnSeat!==S.mySeat)return;
  if(S.finishedSeats.has(S.mySeat))return;
  if(!S.canPass){toast('你是领出位，必须出牌');return;}
  send({type:'pass'});
}

function init(){
  $('name-input').value=S.name||'';
  const urlRoom=parseRoom(qs('room'));
  if(urlRoom!=null)$('room-input').value=urlRoom;
  $('lobby-hint').textContent='服务器：'+wsUrl();
  $('name-input').addEventListener('input',e=>{ const v=e.target.value.trim(); localStorage.setItem(LS_NAME,v); if(!S.connected)S.name=v; });
  $('room-input').addEventListener('keydown',e=>{ if(e.key==='Enter')doJoin(); });
  $('name-input').addEventListener('keydown',e=>{ if(e.key==='Enter')e.target.blur(); });
  $('btn-create').addEventListener('click',doCreate);
  setupCreateBotsBtn();
  $('btn-join').addEventListener('click',doJoin);
  ensureWaitBotBtn();
  $('btn-leave').addEventListener('click',()=>{ if(!send({type:'leave_room'}))toast('未连接到服务器'); });
  $('btn-play').addEventListener('click',doPlay);
  $('btn-pass').addEventListener('click',doPass);
  $('btn-again').addEventListener('click',()=>location.reload());
  setInterval(()=>{ if(S.phase==='playing'&&S.roomId!=null)renderPill(); },250);
  window.addEventListener('beforeunload',()=>{ S.closedByUs=true; try{if(S.ws)S.ws.close();}catch(e){} });
  connect();
}
init();
