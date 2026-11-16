#ifndef WEB_PAGE_H
#define WEB_PAGE_H

#include <Arduino.h>

const char HTML_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Car Tester S3 v1.9</title>
<style>
* { box-sizing: border-box; }
body { font-family: Arial, sans-serif; text-align: center; margin: 10px; background: #eef2f5; color: #1e293b; }
.card { background: #fff; padding: 14px; border-radius: 14px; box-shadow: 0 2px 10px rgba(0,0,0,0.07); max-width: 500px; margin: 0 auto 12px auto; }
.val { font-size: 36px; font-weight: bold; margin: 4px 0; }
.btn { display: inline-block; padding: 10px 14px; background: #007bff; color: #fff; text-decoration: none; border-radius: 8px; font-weight: bold; margin: 3px; border: none; cursor: pointer; font-size: 13px; }
.btn-green { background: #16a34a; }
.btn-amber { background: #d97706; }
.btn-red { background: #dc2626; }
.btn-gray { background: #64748b; }
.btn-dark { background: #1e293b; }
.btn-sm { padding: 6px 10px; font-size: 12px; margin: 0; }
.badge { display: inline-block; padding: 4px 10px; border-radius: 20px; font-size: 12px; font-weight: bold; margin-bottom: 6px; }
.b-idle { background: #e2e8f0; color: #475569; }
.b-rec { background: #dcfce7; color: #15803d; }
.b-fin { background: #dbeafe; color: #1d4ed8; }
.box-gray { margin-top: 10px; padding: 10px; background: #f1f5f9; border-radius: 8px; font-size: 13px; text-align: left; }
.row { display: flex; justify-content: space-between; align-items: center; margin: 8px 0; gap: 8px; }
.row-label { font-size: 13px; text-align: left; white-space: nowrap; }
.row-right { display: flex; align-items: center; gap: 6px; }
.inp { padding: 7px 8px; font-size: 14px; border: 1px solid #94a3b8; border-radius: 6px; font-weight: bold; }
.inp-sm { width: 80px; text-align: center; }
.stat-grid { display: flex; justify-content: space-around; text-align: center; background: #fff; padding: 8px; border-radius: 8px; border: 1px solid #e2e8f0; margin-top: 8px; }
.ev-item { border-bottom: 1px dashed #cbd5e1; padding: 5px 0; font-size: 12px; }
.ev-item:last-child { border-bottom: none; }
.banner { background: #fef3c7; border: 1px solid #f59e0b; color: #92400e; padding: 10px; border-radius: 8px; font-size: 13px; font-weight: bold; margin-bottom: 10px; display: none; }
</style>
</head>
<body>

<div class="banner" id="warnCurrent">⚠️ Внимание! Долгий ток > 5А. Проверьте нагрев щупов.</div>

<div class="card">
  <div class="badge b-idle" id="stateBadge">⏸ ОЖИДАНИЕ СТАРТА</div>
  <div style="font-size:13px;color:#64748b;">Часы: <b id="clockNow">--:--:--</b> | Батарея: <b id="batLevel">--%</b></div>
  <div style="margin-top:10px;">
    <button class="btn btn-green" id="btnStart" onclick="startTest()">▶ Начать замер</button>
    <button class="btn btn-red" id="btnFinish" style="display:none;" onclick="finishTest()">⏹ Завершить</button>
  </div>
</div>

<div class="card">
  <div class="val" id="volt">--.-- В</div>
  <div class="val" id="curr" style="color:#64748b;">-- мА</div>
  <div id="status" style="font-size:13px;font-weight:bold;">Ожидание данных...</div>
  <div class="stat-grid">
    <div><div style="font-size:11px;color:#64748b;">Мин. ток</div><div style="font-weight:bold;" id="sMin">0 мА</div></div>
    <div><div style="font-size:11px;color:#64748b;">Средний</div><div style="font-weight:bold;" id="sAvg">0 мА</div></div>
    <div><div style="font-size:11px;color:#64748b;">Пик тока</div><div style="font-weight:bold;" id="sMax">0 мА</div></div>
  </div>
</div>

<div class="card">
  <div style="font-weight:bold;margin-bottom:8px;">🔧 Калибровка</div>
  <div class="row"><div class="row-label">Множитель U:</div><div class="row-right"><input class="inp inp-sm" id="volCalInput" type="number" step="0.0001"><button class="btn btn-sm btn-gray" onclick="saveCal()">ОК</button></div></div>
  <div class="row"><div class="row-label">Множитель I:</div><div class="row-right"><input class="inp inp-sm" id="curCalInput" type="number" step="0.01"><button class="btn btn-sm btn-gray" onclick="saveCal()">ОК</button></div></div>
  <div class="row"><div class="row-label">Смещение (А):</div><div class="row-right"><input class="inp inp-sm" id="curOffInput" type="number" step="0.0001"><button class="btn btn-sm btn-gray" onclick="saveCal()">ОК</button></div></div>
</div>

<div class="card">
  <div style="font-weight:bold;margin-bottom:8px;">🌐 Сеть и Обновления</div>
  <div class="row"><div class="row-label">Wi-Fi (SSID):</div><div class="row-right"><input class="inp inp-sm" id="wifiSsid" type="text" style="width:120px;"></div></div>
  <div class="row"><div class="row-label">Пароль:</div><div class="row-right"><input class="inp inp-sm" id="wifiPass" type="password" style="width:120px;"><button class="btn btn-sm btn-gray" onclick="saveWifi()">OK</button></div></div>
  <div style="font-size:12px;color:#64748b;margin-top:6px;">Статус: <b id="wifiStatus">AP Only</b></div>
  <div style="margin-top:10px;">
    <button class="btn btn-amber" onclick="checkUpdates()">🔄 Проверить обновления</button>
    <a class="btn btn-dark" href="/update">⚙️ Ручное OTA</a>
  </div>
</div>

<div class="card">
  <div style="font-weight:bold;margin-bottom:6px;">📌 События</div>
  <div class="box-gray" id="eventsList">События появятся после старта...</div>
</div>

<div class="card">
  <div style="margin-top:10px;">
    <a class="btn btn-dark" href="/download">📥 Скачать отчет (.CSV)</a>
    <button class="btn btn-gray" onclick="clearLog()">🗑 Очистить лог</button>
    <button class="btn btn-red" onclick="fullReset()">💥 Полный сброс</button>
  </div>
</div>

<script>
let initDone = false;
function updateData() {
  fetch('/data').then(r => r.json()).then(d => {
    document.getElementById('volt').innerText = d.v.toFixed(2) + ' В';
    document.getElementById('curr').innerText = d.ma + ' мА';
    document.getElementById('clockNow').innerText = d.now;
    document.getElementById('batLevel').innerText = d.bat + '%';
    document.getElementById('sMin').innerText = d.sMin + ' мА';
    document.getElementById('sAvg').innerText = d.sAvg + ' мА';
    document.getElementById('sMax').innerText = d.sMax + ' мА';
    
    document.getElementById('wifiStatus').innerText = d.ip !== "" ? ('Подключен: ' + d.ip) : 'AP Only (нет интернета)';
    document.getElementById('warnCurrent').style.display = d.warnCurrent ? 'block' : 'none';

    if (!initDone) {
      initDone = true;
      if (d.vol_cal) document.getElementById('volCalInput').value = d.vol_cal;
      if (d.cur_cal) document.getElementById('curCalInput').value = d.cur_cal;
      if (d.cur_off !== undefined) document.getElementById('curOffInput').value = d.cur_off;
      if (d.ssid) document.getElementById('wifiSsid').value = d.ssid;
    }

    if (d.state === 0) { document.getElementById('stateBadge').innerText = '⏸ ОЖИДАНИЕ'; document.getElementById('btnStart').style.display='inline-block'; document.getElementById('btnFinish').style.display='none'; }
    else if (d.state === 1) { document.getElementById('stateBadge').innerText = '🔴 ИДЕТ ЗАПИСЬ'; document.getElementById('btnStart').style.display='none'; document.getElementById('btnFinish').style.display='inline-block'; }
    else { document.getElementById('stateBadge').innerText = '✅ ЗАВЕРШЕНО'; document.getElementById('btnStart').style.display='inline-block'; document.getElementById('btnFinish').style.display='none'; }
    
    if (d.events) {
      let h = '';
      for (let e of d.events) h += '<div class="ev-item"><b>' + e.t + '</b> — ' + e.k + '</div>';
      document.getElementById('eventsList').innerHTML = h || 'Нет событий';
    }
  });
}
function saveCal() {
  fetch('/setcal?vol_cal=' + document.getElementById('volCalInput').value + '&cur_cal=' + document.getElementById('curCalInput').value + '&cur_off=' + document.getElementById('curOffInput').value)
    .then(() => { updateData(); alert('Калибровка сохранена!'); });
}
function saveWifi() {
  fetch('/setwifi?ssid=' + encodeURIComponent(document.getElementById('wifiSsid').value) + '&pass=' + encodeURIComponent(document.getElementById('wifiPass').value))
    .then(() => alert('Wi-Fi сохранен! Прибор перезагрузится.'));
}
function startTest() { fetch('/start').then(()=>updateData()); }
function finishTest() { fetch('/finish').then(()=>updateData()); }
function clearLog() { if(confirm('Очистить логи (данные авто останутся)?')) fetch('/clear').then(()=>updateData()); }
function fullReset() { if(confirm('ПОЛНЫЙ СБРОС (включая данные авто)?')) fetch('/fullreset').then(()=>updateData()); }
function checkUpdates() {
  alert('Идет проверка... (Смотрите индикатор на приборе)');
  fetch('/checkupdate').then(r => r.text()).then(t => alert(t));
}
setInterval(updateData, 1000); updateData();
</script>
</body>
</html>)rawliteral";

#endif // WEB_PAGE_H