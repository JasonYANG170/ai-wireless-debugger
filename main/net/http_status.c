/*
 * http_status.c - HTTP server with LCD simulation + serial log
 *
 * GET /            - HTML page: centered 160x80 LCD sim (crisp) + 3 buttons below, serial log
 * GET /api/status  - JSON: wifi, ip, baud, rx, tx, uptime, tcp, page, selected
 * GET /api/data    - drain serial RX as text
 * GET /api/btn?b=1&a=press - simulate button press (b=1/2/3, a=press/long)
 * GET /api/baud?b= - set baud rate
 * POST /api/send   - send data to DUT
 */
#include "http_status.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "serial_bridge.h"
#include "wifi_manager.h"
#include "pinout.h"
#include "menu_ui.h"
#include "lcd_driver.h"
#include "pin_config.h"
#include "swd_bridge.h"
#include "pwm_mon.h"
#include "spi_mon.h"
#include "i2c_mon.h"
#include "driver/uart.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "http";

extern int tcp_server_client_count(void);

static const char s_index_html[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>串口调试器</title><style>"
"*{box-sizing:border-box}"
":root{color-scheme:light dark;--bg:#0a0a0a;--fg:#eee;--box:#161616;--bd:#2a2a2a;--in:#222;--inbd:#444;--accent:#4af;--accentfg:#111;--accent2:#08d;--ghost:#333;--ghostbd:#555;--muted:#888;--logbg:#0a0a0a;--logfg:#ccc}"
"@media (prefers-color-scheme:light){:root{--bg:#eef1f4;--fg:#1c1c1c;--box:#fff;--bd:#d7dbe0;--in:#fff;--inbd:#c4c9d2;--accent:#07a;--accentfg:#fff;--accent2:#09f;--ghost:#e7eaef;--ghostbd:#c4c9d2;--muted:#5d6470;--logbg:#0d1117;--logfg:#c9d1d9}}"
"body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--fg);margin:0;padding:14px}"
".wrap{max-width:720px;margin:0 auto}"
"h1{color:var(--accent);margin:0 0 10px;font-size:18px;text-align:center}"
".device{display:flex;flex-direction:column;align-items:center;margin-bottom:14px}"
".lcd{width:320px;height:160px;background:#000;border:2px solid var(--bd);border-radius:6px;overflow:hidden}"
".lcd canvas{display:block;width:100%;height:100%}"
".info{font-size:11px;color:var(--muted);margin:6px auto 0;text-align:center;max-width:320px;word-break:break-word}"
".btns{display:flex;flex-direction:row;gap:10px;justify-content:center;margin-top:10px}"
".btns button{width:92px;height:48px;border:none;border-radius:8px;font-size:13px;"
"font-weight:600;cursor:pointer;color:var(--accentfg);background:var(--accent);transition:background .08s}"
".btns button:active{background:var(--accent2)}"
".bottom{margin-top:4px}"
".log-box{background:var(--box);border:1px solid var(--bd);border-radius:8px;padding:10px;margin-bottom:10px}"
".log-box b{color:var(--accent)}"
"#log{white-space:pre-wrap;font-family:monospace;font-size:12px;max-height:340px;"
"overflow-y:scroll;word-break:break-all;background:var(--logbg);color:var(--logfg);padding:6px;border-radius:4px;"
"min-height:260px}"
"#log .rx{color:#0f0}"
"#log .tx{color:#fa4}"
".bar{display:flex;gap:6px;margin-top:6px;align-items:center}"
".bar input{flex:1;min-width:0;background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:5px 8px}"
".bar select{background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:5px}"
".sel{background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:4px}"
".wave{display:block;width:100%;height:90px;background:#0d1117;border:1px solid var(--bd);border-radius:4px;margin-top:6px}"
".cfg{display:flex;gap:8px;margin-top:6px;align-items:center;font-size:12px;color:var(--muted);justify-content:center}"
".wfi{display:block;width:100%;margin-top:4px;background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:6px 8px}"
".wgo{margin-top:6px;background:var(--accent);color:var(--accentfg);border:none;border-radius:4px;padding:6px 16px;font-weight:600;cursor:pointer}"
".wscan{background:var(--ghost);color:var(--fg);border:1px solid var(--ghostbd);border-radius:4px;padding:5px 12px;cursor:pointer}"
".btn-accent{background:var(--accent);color:var(--accentfg);border:none;border-radius:4px;padding:5px 14px;font-weight:600;cursor:pointer}"
".btn-ghost{background:var(--ghost);color:var(--fg);border:1px solid var(--ghostbd);border-radius:4px;padding:3px 10px;cursor:pointer}"
".ai-settings{margin-bottom:8px}"
".ai-settings input{display:block;width:100%;margin-top:4px;background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:5px 8px}"
".ai-chat{display:flex;flex-direction:column;height:420px}"
".ai-messages{flex:1;overflow-y:auto;margin:8px 0;padding:6px;background:var(--logbg);border-radius:4px;min-height:300px}"
".ai-msg-user{color:#fa4;margin:3px 0;font-size:12px}"
".ai-msg-ai{color:#0f0;margin:3px 0;font-size:12px;white-space:pre-wrap}"
".ai-typing{color:var(--muted);font-size:12px;margin:3px 0}"
".ai-typing::after{content:'';animation:blink .8s infinite}"
"@keyframes blink{0%,100%{opacity:1}50%{opacity:0}}"
".ai-bar{display:flex;gap:6px;margin-top:4px}"
".ai-bar input{flex:1;min-width:0;background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:5px 8px}"
".ai-bar button{background:var(--accent);color:var(--accentfg);border:none;border-radius:4px;padding:5px 14px;font-weight:600;cursor:pointer}"
"</style></head><body>"
"<div class='wrap'>"
"<div class='device'>"
"<div class='lcd'><canvas id='cv'></canvas></div>"
"<div class='info' id='st'>connecting...</div>"
"<div class='btns'>"
"<button onclick='btn(1)'>SW1</button>"
"<button onclick='btn(2)'>SW2</button>"
"<button onclick='btn(3)'>SW3</button>"
"</div>"
"</div>"
"<div class='bottom'>"
"<div class='log-box' id='serialbox' style='display:none'><b>串口日志</b>"
"<div id='log'>(等待数据...)</div>"
"<div class='bar'>"
"<input id='txt' placeholder='发送到设备...'>"
"<select id='nl'><option value=0>无换行</option><option value=1 selected>LF</option>"
"<option value=2>CR</option><option value=3>CRLF</option></select>"
"<button onclick='snd()' class='btn-accent'>发送</button>"
"</div>"
"<div class='cfg'>波特率:<select id='bd' onchange='setbd()'>"
"<option value=9600>9600</option><option value=115200 selected>115200</option>"
"<option value=460800>460800</option><option value=921600>921600</option></select>"
"<button onclick='clr()' class='btn-ghost'>清空</button>"
"<label><input type='checkbox' id='hex'>HEX</label>"
"</div></div>"
"<div class='log-box' id='wifibox' style='margin-top:8px;display:none'><b>WiFi 配置</b>"
"<input id='wssid' placeholder='SSID...' class='wfi'>"
"<input id='wpass' type='password' placeholder='密码...' class='wfi'>"
"<button onclick='wificonnect()' class='wgo'>连接</button>"
"<div style='margin-top:6px'><button onclick='scanwifi()' class='wscan'>扫描</button> "
"<span id='wifilist' style='font-size:12px;color:var(--muted)'></span></div>"
"</div>"
"<div class='log-box' id='pinsbox' style='margin-top:8px;display:none'><b>引脚协议分配</b>"
"<div style='font-size:11px;color:var(--muted);margin-top:2px'>5 个固定 J3 信号脚，下拉切换协议（自动交换）</div>"
"<div id='pinlist' style='margin-top:4px'></div>"
"<div style='margin-top:6px'><button onclick='savePins()' class='btn-accent'>保存</button>"
"<span id='pinmsg' style='margin-left:8px;color:var(--muted);font-size:12px'></span></div>"
"</div>"
"<div class='log-box' id='pwmbox' style='margin-top:8px;display:none'><b>PWM 监测</b> <span id='pwmval' style='color:var(--muted);font-weight:400'></span>"
"<canvas id='pwmcv' class='wave'></canvas>"
"<div style='margin-top:6px;display:flex;gap:6px;align-items:center'>"
"<b style='font-size:12px'>输出:</b>"
"<input id='pwmfreq' placeholder='频率Hz' style='width:80px;background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:4px 6px'>"
"<input id='pwmduty' placeholder='占空比%' style='width:70px;background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:4px 6px'>"
"<button onclick='pwmOut(1)' class='btn-accent'>开始</button>"
"<button onclick='pwmOut(0)' class='btn-ghost'>停止</button>"
"<span id='pwmoutval' style='font-size:11px;color:var(--muted)'></span>"
"</div></div>"
"<div class='log-box' id='spibox' style='margin-top:8px;display:none'><b>SPI 监测</b> <span id='spival' style='color:var(--muted);font-weight:400'></span>"
"<div id='spilog' style='max-height:200px;overflow-y:auto;font-family:monospace;margin-top:4px'></div>"
"<canvas id='spicv' class='wave'></canvas>"
"<div style='margin-top:6px;display:flex;gap:6px;align-items:center'>"
"<b style='font-size:12px'>发送:</b>"
"<input id='spihex' placeholder='HEX (如 AA55DEAD)' style='flex:1;min-width:0;background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:4px 6px'>"
"<button onclick='spiSend()' class='btn-accent'>发送</button>"
"</div><div id='spirx' style='font-size:11px;color:var(--muted);margin-top:4px'></div></div>"
"<div class='log-box' id='i2cbox' style='margin-top:8px;display:none'><b>I2C 监测</b> <span id='i2cval' style='color:var(--muted);font-weight:400'></span>"
"<div id='i2clog' style='max-height:200px;overflow-y:auto;font-family:monospace;margin-top:4px'></div>"
"<div style='margin-top:6px;display:flex;gap:6px;align-items:center;flex-wrap:wrap'>"
"<b style='font-size:12px'>发送:</b>"
"<input id='i2caddr' placeholder='地址(0x50)' style='width:70px;background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:4px 6px'>"
"<input id='i2chex' placeholder='HEX数据' style='flex:1;min-width:0;background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:4px 6px'>"
"<select id='i2cmode' style='background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:4px'>"
"<option value='w'>写</option><option value='r'>读</option><option value='wr'>写后读</option></select>"
"<input id='i2clen' placeholder='读长' style='width:50px;background:var(--in);color:var(--fg);border:1px solid var(--inbd);border-radius:4px;padding:4px 6px' value='1'>"
"<button onclick='i2cSend()' class='btn-accent'>发送</button>"
"</div><div id='i2crx' style='font-size:11px;color:var(--muted);margin-top:4px'></div></div>"
"<div class='log-box' id='aibox' style='margin-top:8px;display:none'><b>AI 助手</b> <button onclick='toggleAISettings()' style='float:right;background:none;border:none;color:var(--muted);cursor:pointer;font-size:11px'>⚙ 设置</button>"
"<div class='ai-settings' id='aisettings'>"
"<input id='aibase' placeholder='API Base URL (https://api.openai.com)'>"
"<input id='aikey' type='password' placeholder='API Key (sk-...)'>"
"<input id='aimodel' placeholder='模型名 (gpt-4o-mini)'>"
"<div style='display:flex;gap:6px;margin-top:6px'><button onclick='saveAI()' class='btn-accent'>保存</button> <span id='aimsg' style='color:var(--muted);font-size:12px;line-height:32px'></span></div></div>"
"<div class='ai-chat'>"
"<div class='ai-messages' id='aimsgs'><div style='color:var(--muted);font-size:12px'>输入问题开始对话 (自动采集串口/SPI/PWM数据作为上下文)</div></div>"
"<div class='ai-bar'><input id='aiinput' placeholder='发送消息...'> <button onclick='sendAI()'>发送</button></div>"
"</div></div>"
"</div>"
"</div>"
"<script>"
"var cv=document.getElementById('cv'),ctx=cv.getContext('2d');"
"function fitLCD(){var dpr=window.devicePixelRatio||1;"
"cv.width=Math.round((cv.clientWidth||320)*dpr);cv.height=Math.round((cv.clientHeight||160)*dpr);}"
"fitLCD();window.addEventListener('resize',fitLCD);"
"var log=document.getElementById('log');"
"var hexMode=false,aiHistory=[];"
"document.getElementById('hex').onchange=function(e){hexMode=e.target.checked};"
"var _dbl=300,_p=null;"
"function btn(b){if(_p&&_p.b===b){clearTimeout(_p.t);_p=null;fetch('/api/btn?b='+b+'&a=long');return;}"
"if(_p){clearTimeout(_p.t)}_p={b:b,t:setTimeout(function(){_p=null;fetch('/api/btn?b='+b+'&a=press')},_dbl)}}"
"function poll(){fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('st').innerHTML='<b>WiFi:</b>'+d.wifi+' <b>IP:</b>'+d.ip+' <b>波特率:</b>'+d.baud+' <b>接收:</b>'+d.rx+' <b>发送:</b>'+d.tx+' <b>运行:</b>'+d.uptime+'s';"
"document.getElementById('serialbox').style.display=(d.page==2?'':'none');"
"serialVisible=(d.page==2);"
"document.getElementById('wifibox').style.display=(d.page==4?'':'none');"
"document.getElementById('pinsbox').style.display=(d.page==3?'':'none');"
"document.getElementById('pwmbox').style.display=(d.page==5?'':'none');"
"document.getElementById('spibox').style.display=(d.page==6?'':'none');"
"document.getElementById('i2cbox').style.display=(d.page==7?'':'none');"
"document.getElementById('aibox').style.display=(d.page==8?'':'none');"
"fetchLCD();}).catch(e=>{})}"
"var ocv=document.createElement('canvas');ocv.width=160;ocv.height=80;"
"var octx=ocv.getContext('2d');var img=octx.createImageData(160,80);"
"function fetchLCD(){fetch('/api/lcd').then(function(r){return r.arrayBuffer()}).then(function(ab){"
"var u=new Uint8Array(ab),d=img.data;"
"for(var i=0,j=0;i<25600;i+=2,j+=4){var v=u[i]|(u[i+1]<<8);"
"var rr=(v>>11)&31,gg=(v>>5)&63,bb=v&31;"
"d[j]=(rr<<3)|(rr>>2);d[j+1]=(gg<<2)|(gg>>4);d[j+2]=(bb<<3)|(bb>>2);d[j+3]=255;}"
"octx.putImageData(img,0,0);"
"ctx.setTransform(cv.width/160,0,0,cv.height/80,0,0);"
"ctx.imageSmoothingEnabled=false;ctx.clearRect(0,0,160,80);ctx.drawImage(ocv,0,0);"
"}).catch(function(e){})}"
"poll();setInterval(poll,500);"
"var pinState=null;"
"function loadPins(){fetch('/api/pins').then(function(r){return r.json()}).then(function(d){pinState=d;renderPins()}).catch(function(e){})}"
"function assignPin(pos,func){var j=pinState.order.indexOf(func);if(j===pos){return}if(j>=0){var t=pinState.order[pos];pinState.order[pos]=pinState.order[j];pinState.order[j]=t}else{pinState.order[pos]=func}renderPins()}"
"function renderPins(){var pl=document.getElementById('pinlist');if(!pinState){return;}pl.innerHTML='';"
"pinState.order.forEach(function(k,i){var row=document.createElement('div');row.style.cssText='display:flex;gap:8px;align-items:center;margin-top:4px';"
"var lbl=document.createElement('b');lbl.textContent='IO'+pinState.pos_io[i]+':';lbl.style.cssText='width:72px';row.appendChild(lbl);"
"var sel=document.createElement('select');sel.className='sel';"
"['TX','RX','PWM','SCK','MOSI','MISO','CS','SDA','SCL'].forEach(function(f){var o=document.createElement('option');o.value=f;o.textContent=f;if(f===k){o.selected=true}sel.appendChild(o)});"
"sel.onchange=function(){assignPin(i,sel.value)};row.appendChild(sel);pl.appendChild(row)})}"
"function savePins(){if(!pinState){return};fetch('/api/pins',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'order='+encodeURIComponent(pinState.order.join(','))}).then(function(r){return r.text()}).then(function(t){var ok=true;try{ok=JSON.parse(t).ok}catch(e){}document.getElementById('pinmsg').textContent=ok?'已保存':'保存失败';loadPins()}).catch(function(e){document.getElementById('pinmsg').textContent='保存失败'})}"
"loadPins();"
"function fitWave(cv){var dpr=window.devicePixelRatio||1;cv.width=Math.round((cv.clientWidth||300)*dpr);cv.height=Math.round((cv.clientHeight||80)*dpr)}"
"function drawWaveCv(cv,duty,color){var dpr=window.devicePixelRatio||1;fitWave(cv);var ctx=cv.getContext('2d'),W=cv.width,H=cv.height;"
"ctx.fillStyle='#0d1117';ctx.fillRect(0,0,W,H);"
"ctx.strokeStyle='#262626';ctx.lineWidth=1;ctx.beginPath();ctx.moveTo(0,H/2);ctx.lineTo(W,H/2);ctx.stroke();"
"var cycles=12,cw=W/cycles,hi_top=4,hi_bot=H-8;ctx.strokeStyle=color;ctx.lineWidth=Math.max(1,dpr);ctx.beginPath();ctx.moveTo(0,hi_bot);"
"for(var i=0;i<cycles;i++){var x=i*cw,hi=cw*duty;ctx.lineTo(x,hi_top);ctx.lineTo(x+hi,hi_top);ctx.lineTo(x+hi,hi_bot);ctx.lineTo(x+cw,hi_bot)}ctx.stroke()}"
"function pollPwm(){fetch('/api/pwm').then(function(r){return r.json()}).then(function(d){document.getElementById('pwmval').textContent='  '+d.freq.toFixed(2)+' Hz   '+d.duty.toFixed(1)+'%';drawWaveCv(document.getElementById('pwmcv'),d.duty/100,'#4caf50')}).catch(function(e){})}"
"function drawSpiCv(cv,bytes){var dpr=window.devicePixelRatio||1;cv.width=Math.round((cv.clientWidth||300)*dpr);cv.height=Math.round((cv.clientHeight||90)*dpr);"
"var ctx=cv.getContext('2d'),W=cv.width,H=cv.height;ctx.fillStyle='#0d1117';ctx.fillRect(0,0,W,H);"
"if(!bytes||!bytes.length){ctx.fillStyle='#888';ctx.font=(12*dpr)+'px monospace';ctx.fillText('(暂无 SPI 事务)',12*dpr,H/2);return;}"
"var bits=Math.min(bytes.length,2)*8,bw=W/bits,sckY=H*0.35,mosiY=H*0.72,amp=H*0.16;"
"ctx.fillStyle='#888';ctx.font=(9*dpr)+'px monospace';ctx.fillText('SCK',2*dpr,sckY-amp-2*dpr);ctx.fillText('MOSI',2*dpr,mosiY-amp-2*dpr);"
"ctx.strokeStyle='#ffd54f';ctx.lineWidth=Math.max(1,dpr);ctx.beginPath();ctx.moveTo(0,sckY+amp);"
"for(var i=0;i<bits;i++){var x=i*bw;ctx.lineTo(x,sckY+amp);ctx.lineTo(x,sckY-amp);ctx.lineTo(x+bw,sckY-amp);ctx.lineTo(x+bw,sckY+amp)}ctx.stroke();"
"ctx.strokeStyle='#4caf50';ctx.beginPath();ctx.moveTo(0,mosiY+amp);var lvl=mosiY+amp;"
"for(var i=0;i<bits;i++){var byteI=Math.floor(i/8),bitI=7-(i%8);var bit=(bytes[byteI]>>bitI)&1;var tgt=bit?mosiY-amp:mosiY+amp;var x=i*bw;ctx.lineTo(x,lvl);ctx.lineTo(x,tgt);lvl=tgt;ctx.lineTo(x+bw,tgt)}ctx.stroke()}"
"function pollSpi(){fetch('/api/spi').then(function(r){return r.json()}).then(function(d){"
"var el=document.getElementById('spival');el.textContent='  '+(d.running?(d.count+' txn'):'未分配');"
"var sl=document.getElementById('spilog');sl.innerHTML='';"
"if(d.history&&d.history.length){var h=d.history;var start=Math.max(0,h.length-8);"
"for(var i=start;i<h.length;i++){var t=h[i];"
"var rd=document.createElement('div');rd.style.cssText='margin-top:2px;font-size:11px';"
"var mosi=t.mosi.map(function(b){return('0'+b.toString(16)).slice(-2).toUpperCase()}).join(' ');"
"rd.textContent='#'+t.seq+' >> '+mosi;rd.style.color='#4caf50';sl.appendChild(rd);"
"var hasMiso=t.miso.some(function(b){return b!==0});"
"if(hasMiso){var md=document.createElement('div');md.style.cssText='font-size:11px;color:#ffd54f';"
"var miso=t.miso.map(function(b){return('0'+b.toString(16)).slice(-2).toUpperCase()}).join(' ');"
"md.textContent='   << '+miso;sl.appendChild(md);}}"
"sl.scrollTop=sl.scrollHeight;"
"if(h.length){var last=h[h.length-1];drawSpiCv(document.getElementById('spicv'),last.mosi)}"
"}}).catch(function(e){})}"
"function pollI2c(){fetch('/api/i2c').then(function(r){return r.json()}).then(function(d){"
"var el=document.getElementById('i2cval');el.textContent='  '+(d.running?(d.count+' txn, ISR:'+d.isr):'未分配');"
"var sl=document.getElementById('i2clog');sl.innerHTML='';"
"if(d.history&&d.history.length){var h=d.history;var start=Math.max(0,h.length-8);"
"for(var i=start;i<h.length;i++){var t=h[i];"
"var rd=document.createElement('div');rd.style.cssText='margin-top:2px;font-size:11px';"
"var rw=t.read?'R':'W';var addr=('0'+t.addr.toString(16)).slice(-2).toUpperCase();"
"var data=t.data.map(function(b){return('0'+b.toString(16)).slice(-2).toUpperCase()}).join(' ');"
"rd.textContent='#'+t.seq+' '+addr+rw+' '+data;rd.style.color=t.read?'#4af':'#0f0';sl.appendChild(rd)}"
"sl.scrollTop=sl.scrollHeight}}).catch(function(e){})}"
"function pwmOut(en){var f=document.getElementById('pwmfreq').value,d=document.getElementById('pwmduty').value||'50';"
"fetch('/api/pwm/out',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'freq='+f+'&duty='+d+'&enable='+en}).then(function(r){return r.json()}).then(function(d){"
"document.getElementById('pwmoutval').textContent=d.running?(d.freq.toFixed(1)+'Hz '+d.duty.toFixed(0)+'%'):''}).catch(function(e){})}"
"function spiSend(){var h=document.getElementById('spihex').value.replace(/\\s/g,'');if(!h)return;"
"fetch('/api/spi/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'hex='+h}).then(function(r){return r.json()}).then(function(d){"
"var el=document.getElementById('spirx');if(d.ok){var rx=d.rx.map(function(b){return('0'+b.toString(16)).slice(-2).toUpperCase()}).join(' ');"
"el.textContent='MISO: '+rx}else el.textContent='错误: '+(d.err||'unknown')}).catch(function(e){document.getElementById('spirx').textContent='错误: '+e.message})}"
"function i2cSend(){var a=document.getElementById('i2caddr').value,h=document.getElementById('i2chex').value.replace(/\\s/g,'');"
"var m=document.getElementById('i2cmode').value,l=document.getElementById('i2clen').value||'1';if(!a)return;"
"fetch('/api/i2c/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'addr='+a+'&hex='+h+'&mode='+m+'&len='+l}).then(function(r){return r.json()}).then(function(d){"
"var el=document.getElementById('i2crx');if(d.ok){if(d.rx.length){var rx=d.rx.map(function(b){return('0'+b.toString(16)).slice(-2).toUpperCase()}).join(' ');"
"el.textContent='读取: '+rx}else el.textContent='写入成功'}else el.textContent='错误: '+d.err}).catch(function(e){document.getElementById('i2crx').textContent='错误: '+e.message})}"
"setInterval(function(){pollPwm();pollSpi();pollI2c()},300);"
"function addLine(c,t){if(log.firstChild&&log.firstChild.nodeType===3)log.innerHTML='';"
"var d=document.createElement('div');d.className=c;d.textContent=t;log.appendChild(d);"
"while(log.childNodes.length>400)log.removeChild(log.firstChild);log.scrollTop=log.scrollHeight}"
"function polldata(){fetch('/api/data').then(r=>r.text()).then(t=>{"
"if(t.length>0){if(hexMode){var h='';for(var i=0;i<t.length;i++)h+=('0'+t.charCodeAt(i).toString(16)).slice(-2)+' ';addLine('rx','> '+h.trim())}"
"else addLine('rx','> '+t)}}).catch(e=>{})}"
"var serialVisible=false;"
"setInterval(function(){if(serialVisible)polldata()},200);"
"function snd(){var t=document.getElementById('txt').value;if(!t)return;"
"var nl=document.getElementById('nl').value;"
"if(nl==1)t+='\\n';else if(nl==2)t+='\\r';else if(nl==3)t+='\\r\\n';"
"addLine('tx','<< '+t);"
"fetch('/api/send',{method:'POST',body:t});document.getElementById('txt').value=''}"
"document.getElementById('txt').addEventListener('keydown',e=>{if(e.key=='Enter')snd()});"
"function setbd(){fetch('/api/baud?b='+document.getElementById('bd').value)}"
"function clr(){log.innerHTML=''}"
"function scanwifi(){var wl=document.getElementById('wifilist');wl.textContent='扫描中...';"
"fetch('/api/wifi').then(function(r){return r.json()}).then(function(l){wl.innerHTML='';"
"l.forEach(function(a){var b=document.createElement('button');b.textContent=a.ssid+'('+a.rssi+')';"
"b.style.cssText='margin:2px;background:var(--in);color:var(--accent);border:1px solid var(--inbd);border-radius:3px;padding:3px 6px;cursor:pointer;font-size:11px';"
"b.onclick=function(){document.getElementById('wssid').value=a.ssid};wl.appendChild(b)})}).catch(function(e){wl.textContent='扫描失败'})}"
"function wificonnect(){var s=document.getElementById('wssid').value,p=document.getElementById('wpass').value;"
"if(!s){alert('请输入 SSID');return}"
"fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)}).then(function(r){return r.text()}).then(function(t){alert('正在连接 '+s)})}"
"var aiConfig={base:'',key:'',model:''};"
"function toggleAISettings(){var el=document.getElementById('aisettings');el.style.display=el.style.display==='none'?'block':'none'}"
"function loadAI(){var c=localStorage.getItem('aiConfig');if(c){try{aiConfig=JSON.parse(c)}catch(e){}}"
"document.getElementById('aibase').value=aiConfig.base;document.getElementById('aikey').value=aiConfig.key?'***':'';"
"document.getElementById('aimodel').value=aiConfig.model}"
"function saveAI(){var b=document.getElementById('aibase').value,k=document.getElementById('aikey').value,m=document.getElementById('aimodel').value;"
"if(k!=='***')aiConfig.key=k;aiConfig.base=b;aiConfig.model=m;"
"localStorage.setItem('aiConfig',JSON.stringify(aiConfig));"
"fetch('/api/ai/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'base='+encodeURIComponent(b)+'&model='+encodeURIComponent(m)});"
"document.getElementById('aimsg').textContent='已保存'}"
"function appendAI(role,text){var el=document.getElementById('aimsgs');var d=document.createElement('div');d.className='ai-msg-'+role;d.textContent=(role==='user'?'> ':'')+text;el.appendChild(d);el.scrollTop=el.scrollHeight;return d}"
"function collectContext(){return Promise.all([fetch('/api/data').then(function(r){return r.text()}).catch(function(){return''}),"
"fetch('/api/pwm').then(function(r){return r.json()}).catch(function(){return{freq:0,duty:0}}),"
"fetch('/api/spi').then(function(r){return r.json()}).catch(function(){return{running:false,count:0,history:[]}})]).then(function(a){"
"var serial=a[0],pwm=a[1],spi=a[2];var ctx='你是嵌入式调试助手，分析设备数据并回答问题。\\n';"
"ctx+='串口最近数据: '+(serial.length?serial.slice(-500):'(无)')+'\\n';"
"ctx+='PWM: '+pwm.freq.toFixed(2)+'Hz, '+pwm.duty.toFixed(1)+'%\\n';"
"ctx+='SPI: '+(spi.running?'运行中':'未启动')+', '+spi.count+'笔事务\\n';"
"if(spi.history&&spi.history.length){var h=spi.history.slice(-3);h.forEach(function(t,i){"
"ctx+='  SPI#'+t.seq+': MOSI='+t.mosi.map(function(b){return('0'+b.toString(16)).slice(-2)}).join(' ')+'\\n'})}"
"return ctx})}"
"function sendAI(){var inp=document.getElementById('aiinput');var text=inp.value.trim();if(!text||aiBusy)return;inp.value='';"
"if(!aiConfig.base||!aiConfig.key){appendAI('ai','请先配置API地址和Key');return}"
"aiHistory.push({role:'user',content:text});appendAI('user',text);aiBusy=true;"
"var typingEl=appendAI('ai','思考中');typingEl.className='ai-typing';"
"collectContext().then(function(ctx){var msgs=[{role:'system',content:ctx}];"
"var start=Math.max(0,aiHistory.length-10);for(var i=start;i<aiHistory.length;i++)msgs.push(aiHistory[i]);"
"var url=aiConfig.base.replace(/\\/+$/,'')+'/v1/chat/completions';"
"return fetch(url,{method:'POST',headers:{'Content-Type':'application/json','Authorization':'Bearer '+aiConfig.key},"
"body:JSON.stringify({model:aiConfig.model||'gpt-4o-mini',messages:msgs,max_tokens:2048})})}).then(function(r){return r.json()}).then(function(d){"
"aiBusy=false;typingEl.remove();var reply='';if(d.choices&&d.choices[0])reply=d.choices[0].message.content;"
"else if(d.error)reply='API错误: '+(d.error.message||d.error);else reply='(无响应)';"
"aiHistory.push({role:'assistant',content:reply});appendAI('ai',reply)}).catch(function(e){"
"aiBusy=false;typingEl.remove();appendAI('ai','错误: '+e.message)})}"
"var aiBusy=false;loadAI();"
"document.getElementById('aiinput').addEventListener('keydown',function(e){if(e.key==='Enter')sendAI()});"
"</script></body></html>";

/* GET / - serve HTML page */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* HTML lives in firmware flash; never let the browser serve a stale copy */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, s_index_html, -1);
}

/* GET /api/status - return JSON status */
static esp_err_t status_api_handler(httpd_req_t *req)
{
    char ipbuf[16] = {0};
    wifi_manager_get_ip_str(ipbuf, sizeof(ipbuf));
    wifi_state_t st = wifi_manager_get_state();
    const char *wstate = "disconnected";
    if (st == WIFI_STATE_CONNECTED_STA) wstate = "connected";
    else if (st == WIFI_STATE_AP_MODE) wstate = "ap";

    uint32_t baud = 0;
    uart_get_baudrate(UART1_PORT_NUM, &baud);

    /* Get current menu page (extern from menu_ui) */
    extern int menu_get_current_page(void);
    extern int menu_get_selected(void);
    int page = menu_get_current_page();
    int sel = menu_get_selected();

    char json[320];
    snprintf(json, sizeof(json),
             "{\"wifi\":\"%s\",\"ip\":\"%s\",\"baud\":%lu,\"rx\":%lu,\"tx\":%lu,"
             "\"uptime\":%lu,\"tcp\":%d,\"page\":%d,\"sel\":%d}",
             wstate, ipbuf, (unsigned long)baud,
             (unsigned long)serial_bridge_get_rx_count(),
             (unsigned long)serial_bridge_get_tx_count(),
             (unsigned long)(esp_timer_get_time() / 1000000),
             tcp_server_client_count(), page, sel);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/data - drain received serial data as plain text */
static esp_err_t data_api_handler(httpd_req_t *req)
{
    /* Static buffer so MCP / pollers can drain up to 4KB per call without
     * stressing the httpd stack. */
    static uint8_t buf[4096];
    size_t n = serial_bridge_read(buf, sizeof(buf) - 1, 0);
    if (n == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "", 0);
    }
    buf[n] = '\0';
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, (char *)buf, n);
}

/* GET /api/mcp/data - drain MCP's independent serial buffer (no web/TCP competition) */
static esp_err_t mcp_data_handler(httpd_req_t *req)
{
    static uint8_t buf[4096];
    size_t n = serial_bridge_read_mcp(buf, sizeof(buf) - 1, 0);
    if (n == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "", 0);
    }
    buf[n] = '\0';
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, (char *)buf, n);
}

/* GET /api/btn?b=1&a=press - simulate button */
static esp_err_t btn_api_handler(httpd_req_t *req)
{
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char bv[8] = {0}, av[8] = {0};
    httpd_query_key_value(query, "b", bv, sizeof(bv));
    httpd_query_key_value(query, "a", av, sizeof(av));
    if (bv[0]) {
        int btn = atoi(bv);
        menu_simulate_button(btn, av[0] ? av : "press");
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

/* GET /api/lcd - raw 160x80 RGB565 framebuffer for screen mirroring */
static esp_err_t lcd_api_handler(httpd_req_t *req)
{
    const uint16_t *fb = lcd_get_fb();
    httpd_resp_set_type(req, "application/octet-stream");
    if (!fb) {
        return httpd_resp_send(req, "", 0);   /* LCD not ready yet -> empty */
    }
    return httpd_resp_send(req, (const char *)fb,
                           LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
}

/* GET /api/pins - current function-per-position order + the fixed IOs */
static esp_err_t pins_get_handler(httpd_req_t *req)
{
    char json[160];
    int n = 0;
    n += snprintf(json + n, sizeof(json) - n, "{\"order\":[");
    for (int i = 0; i < PIN_NUM_SIGNAL_SLOTS; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s\"%s\"",
                      i ? "," : "", pin_config_kind_label(pin_config_pos_func(i)));
    }
    n += snprintf(json + n, sizeof(json) - n, "],\"pos_io\":[");
    for (int i = 0; i < PIN_NUM_SIGNAL_SLOTS; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s%d",
                      i ? "," : "", pin_config_pos_io(i));
    }
    n += snprintf(json + n, sizeof(json) - n, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* Minimal URL-decode in place (%XX and '+'). httpd_query_key_value does not
 * decode the value, so a form-encoded body (e.g. commas as %2C from the
 * browser) must be decoded explicitly. */
static void url_decode_inplace(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' '; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static pin_kind_t parse_pin_kind(const char *s)
{
    if (!s) return PIN_KIND_COUNT;
    if (!strcmp(s, "TX"))    return PIN_TX;
    if (!strcmp(s, "RX"))    return PIN_RX;
    if (!strcmp(s, "PWM"))   return PIN_PWM;
    if (!strcmp(s, "SCK"))   return PIN_SPI_SCK;
    if (!strcmp(s, "MOSI"))  return PIN_SPI_MOSI;
    if (!strcmp(s, "MISO"))  return PIN_SPI_MISO;
    if (!strcmp(s, "CS"))    return PIN_SPI_CS;
    if (!strcmp(s, "SDA"))   return PIN_I2C_SDA;
    if (!strcmp(s, "SCL"))   return PIN_I2C_SCL;
    return PIN_KIND_COUNT;
}

/* POST /api/pins - update the function-per-position order.
 * Body (form-encoded): order=SWCLK,RX,NRST,SWDIO,TX  (a permutation of the 5
 * signal kinds). Re-applies the mapping to SWD and UART. */
static esp_err_t pins_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';

    char order_v[128] = {0};
    esp_err_t pin_err = httpd_query_key_value(body, "order", order_v, sizeof(order_v));
    ESP_LOGI(TAG, "pins POST raw body=[%s] query_err=%s order_v=[%s]",
             body, esp_err_to_name(pin_err), order_v);
    url_decode_inplace(order_v);   /* handle browser-encoded commas (%2C) */
    ESP_LOGI(TAG, "pins POST decoded order_v=[%s]", order_v);

    esp_err_t err = ESP_OK;
    if (order_v[0]) {
        pin_kind_t ord[PIN_NUM_SIGNAL_SLOTS];
        int cnt = 0;
        char *tok = strtok(order_v, ",");
        while (tok && cnt < PIN_NUM_SIGNAL_SLOTS) {
            ord[cnt] = parse_pin_kind(tok);
            ESP_LOGI(TAG, "pins POST tok[%d]=[%s] -> kind=%d", cnt, tok, (int)ord[cnt]);
            cnt++;
            tok = strtok(NULL, ",");
        }
        int o_pw   = pin_config_pwm();
        int o_sck  = pin_config_spi_sck(), o_mosi = pin_config_spi_mosi();
        int o_miso = pin_config_spi_miso(), o_cs   = pin_config_spi_cs();
        int o_sda  = pin_config_i2c_sda(), o_scl  = pin_config_i2c_scl();
        ESP_LOGI(TAG, "pins POST cnt=%d (need %d)", cnt, PIN_NUM_SIGNAL_SLOTS);
        err = (cnt == PIN_NUM_SIGNAL_SLOTS) ? pin_config_set_signal_order(ord)
                                            : ESP_ERR_INVALID_ARG;
        ESP_LOGI(TAG, "pins POST set_signal_order returned %s", esp_err_to_name(err));
        if (err == ESP_OK) {
            /* re-apply the new function->GPIO mapping to every subsystem */
            swd_init();
            serial_bridge_repin();
            int n_pw = pin_config_pwm();
            if (n_pw != o_pw) {
                pwm_mon_stop();
                if (n_pw >= 0) pwm_mon_start(n_pw);
            }
            int n_sck = pin_config_spi_sck(), n_mosi = pin_config_spi_mosi();
            int n_miso = pin_config_spi_miso(), n_cs = pin_config_spi_cs();
            if (n_sck != o_sck || n_mosi != o_mosi || n_miso != o_miso || n_cs != o_cs) {
                spi_mon_stop();
                if (n_sck >= 0 && n_mosi >= 0 && n_miso >= 0 && n_cs >= 0) {
                    spi_mon_start(n_sck, n_mosi, n_miso, n_cs, 0);
                }
            }
            int n_sda = pin_config_i2c_sda(), n_scl = pin_config_i2c_scl();
            if (n_sda != o_sda || n_scl != o_scl) {
                i2c_mon_stop();
                if (n_sda >= 0 && n_scl >= 0) {
                    i2c_mon_start(n_sda, n_scl, 0);
                }
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":true}", -1);
    }
    ESP_LOGW(TAG, "pins POST rejected: %s", esp_err_to_name(err));
    return httpd_resp_send(req, "{\"ok\":false}", -1);
}

/* GET /api/pwm - latest PWM frequency + duty + diagnostic info */
static esp_err_t pwm_api_handler(httpd_req_t *req)
{
    float f, d;
    pwm_mon_get(&f, &d);
    int pwm_pin = pin_config_pwm();
    int lvl = (pwm_pin >= 0) ? gpio_get_level(pwm_pin) : -1;
    char json[120];
    snprintf(json, sizeof(json),
             "{\"freq\":%.2f,\"duty\":%.2f,\"isr\":%lu,\"pin\":%d,\"level\":%d}",
             (double)f, (double)d, (unsigned long)pwm_mon_isr_count(), pwm_pin, lvl);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/spi - captured SPI transaction history */
static esp_err_t spi_api_handler(httpd_req_t *req)
{
    /* Return up to 30 recent transactions so MCP can fetch deep history */
    spi_txn_t txns[30];
    int n = spi_mon_get_history(txns, 30);

    static char json[8192];
    int p = 0;
    p += snprintf(json + p, sizeof(json) - p,
                  "{\"running\":%s,\"count\":%lu,\"timeouts\":%lu,\"history\":[",
                  spi_mon_running() ? "true" : "false",
                  (unsigned long)spi_mon_get_count(),
                  (unsigned long)spi_mon_get_timeouts());
    for (int i = 0; i < n && p < (int)sizeof(json) - 500; i++) {
        int show = txns[i].len;
        if (show > 16) show = 16;
        p += snprintf(json + p, sizeof(json) - p, "%s{\"mosi\":[", i ? "," : "");
        for (int b = 0; b < show; b++)
            p += snprintf(json + p, sizeof(json) - p, "%s%d", b ? "," : "", txns[i].mosi[b]);
        p += snprintf(json + p, sizeof(json) - p, "],\"miso\":[");
        for (int b = 0; b < show; b++)
            p += snprintf(json + p, sizeof(json) - p, "%s%d", b ? "," : "", txns[i].miso[b]);
        p += snprintf(json + p, sizeof(json) - p, "],\"seq\":%lu}", (unsigned long)txns[i].seq);
    }
    p += snprintf(json + p, sizeof(json) - p, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/i2c - captured I2C transaction history */
static esp_err_t i2c_api_handler(httpd_req_t *req)
{
    i2c_txn_t txns[30];
    int n = i2c_mon_get_history(txns, 30);
    static char json[6144];
    int p = 0;
    p += snprintf(json + p, sizeof(json) - p,
                  "{\"running\":%s,\"mode\":\"%s\",\"count\":%lu,\"isr\":%lu,\"history\":[",
                  i2c_mon_running() ? "true" : "false",
                  i2c_mon_mode() == I2C_MON_SLAVE ? "slave" : "passive",
                  (unsigned long)i2c_mon_get_count(),
                  (unsigned long)i2c_mon_get_isr_count());
    for (int i = 0; i < n && p < (int)sizeof(json) - 200; i++) {
        p += snprintf(json + p, sizeof(json) - p,
                      "%s{\"addr\":%d,\"read\":%s,\"data\":[",
                      i ? "," : "", txns[i].addr >> 1,
                      txns[i].read ? "true" : "false");
        for (int b = 0; b < txns[i].len && p < (int)sizeof(json) - 40; b++)
            p += snprintf(json + p, sizeof(json) - p, "%s%d", b ? "," : "", txns[i].data[b]);
        p += snprintf(json + p, sizeof(json) - p, "],\"seq\":%lu}", (unsigned long)txns[i].seq);
    }
    p += snprintf(json + p, sizeof(json) - p, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/capacity - free heap + per-buffer usage (n/max) */
static esp_err_t capacity_api_handler(httpd_req_t *req)
{
    extern int menu_get_rx_hist_n(void);
    extern int menu_get_rx_hist_max(void);
    char json[256];
    snprintf(json, sizeof(json),
             "{\"free_heap\":%lu,"
             "\"serial\":{\"n\":%lu,\"max\":%lu},"
             "\"spi\":{\"n\":%d,\"max\":%d},"
             "\"i2c\":{\"n\":%d,\"max\":%d},"
             "\"rx\":{\"n\":%d,\"max\":%d}}",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)serial_bridge_get_buffered(),
             (unsigned long)serial_bridge_get_bufsize_current(),
             spi_mon_get_hist_n(), spi_mon_get_history_max(),
             i2c_mon_get_hist_n(), i2c_mon_get_history_max(),
             menu_get_rx_hist_n(), menu_get_rx_hist_max());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/clear?what=serial|spi|i2c|rx|all */
static esp_err_t clear_api_handler(httpd_req_t *req)
{
    extern void menu_clear_rx(void);
    char query[24] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char what[8] = {0};
    httpd_query_key_value(query, "what", what, sizeof(what));

    if (!strcmp(what, "serial") || !strcmp(what, "all")) serial_bridge_clear();
    if (!strcmp(what, "spi")    || !strcmp(what, "all")) spi_mon_clear();
    if (!strcmp(what, "i2c")    || !strcmp(what, "all")) i2c_mon_clear();
    if (!strcmp(what, "rx")     || !strcmp(what, "all")) menu_clear_rx();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

/* POST /api/cfg?key=buf|spihist|i2chist|rxhist&val=N — adjust buffer sizes.
 * Safety bounds enforced to avoid heap/partition exhaustion. */
static esp_err_t cfg_set_handler(httpd_req_t *req)
{
    extern void menu_set_rx_hist_max(int);
    char query[40] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char key[12] = {0}, valv[12] = {0};
    httpd_query_key_value(query, "key", key, sizeof(key));
    httpd_query_key_value(query, "val", valv, sizeof(valv));
    int v = atoi(valv);

    if (!strcmp(key, "buf")) {
        /* serial buffer: 512 .. 16384 bytes */
        if (v < 512) v = 512;
        if (v > 16384) v = 16384;
        serial_bridge_set_bufsize((size_t)v);
    } else if (!strcmp(key, "spihist")) {
        if (v < 2) v = 2;
        if (v > SPI_MON_MAX) v = SPI_MON_MAX;
        spi_mon_set_history_max(v);
    } else if (!strcmp(key, "i2chist")) {
        if (v < 2) v = 2;
        if (v > I2C_MON_MAX) v = I2C_MON_MAX;
        i2c_mon_set_history_max(v);
    } else if (!strcmp(key, "rxhist")) {
        menu_set_rx_hist_max(v);   /* clamped inside */
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad key");
        return ESP_FAIL;
    }

    char json[80];
    snprintf(json, sizeof(json), "{\"ok\":true,\"key\":\"%s\",\"val\":%d}", key, v);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/pwm/out - start/stop/set PWM output. Body: freq=&duty=&enable=1 */
static esp_err_t pwm_out_handler(httpd_req_t *req)
{
    char body[128] = {0};
    int total = req->content_len;
    if (total > 0 && total < (int)sizeof(body)) {
        int got = 0;
        while (got < total) {
            int n = httpd_req_recv(req, body + got, total - got);
            if (n <= 0) break;
            got += n;
        }
        body[got] = '\0';
    }

    char freq_v[16] = {0}, duty_v[16] = {0}, en_v[8] = {0};
    httpd_query_key_value(body, "freq", freq_v, sizeof(freq_v));
    httpd_query_key_value(body, "duty", duty_v, sizeof(duty_v));
    httpd_query_key_value(body, "enable", en_v, sizeof(en_v));

    if (en_v[0] == '0') {
        pwm_out_stop();
    } else if (freq_v[0]) {
        float freq = strtof(freq_v, NULL);
        float duty = duty_v[0] ? strtof(duty_v, NULL) : 50.0f;
        if (pwm_out_running()) {
            pwm_out_set(freq, duty);
        } else {
            int pin = pin_config_pwm();
            if (pin >= 0) pwm_out_start(pin, freq, duty);
        }
    }

    float f = 0, d = 0;
    pwm_out_get(&f, &d);
    char json[96];
    snprintf(json, sizeof(json), "{\"running\":%s,\"freq\":%.2f,\"duty\":%.1f}",
             pwm_out_running() ? "true" : "false", (double)f, (double)d);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/spi/send - send hex bytes via SPI master. Body: hex=AABBCC&mode=0&clk=1000000 */
static esp_err_t spi_send_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';

    char hex_v[256] = {0}, mode_v[8] = {0}, clk_v[16] = {0};
    httpd_query_key_value(body, "hex", hex_v, sizeof(hex_v));
    httpd_query_key_value(body, "mode", mode_v, sizeof(mode_v));
    httpd_query_key_value(body, "clk", clk_v, sizeof(clk_v));

    /* Parse hex string to bytes */
    uint8_t tx_buf[128];
    int tx_len = 0;
    for (int i = 0; hex_v[i] && hex_v[i + 1] && tx_len < (int)sizeof(tx_buf); i += 2) {
        char byte[3] = { hex_v[i], hex_v[i + 1], 0 };
        tx_buf[tx_len++] = (uint8_t)strtol(byte, NULL, 16);
    }

    if (tx_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no data");
        return ESP_FAIL;
    }

    int mode = mode_v[0] ? atoi(mode_v) : 0;
    int clk = clk_v[0] ? atoi(clk_v) : 1000000;

    /* Start SPI master if not running, or restart if mode/clock changed */
    if (!spi_send_running()) {
        int sck = pin_config_spi_sck(), mosi = pin_config_spi_mosi();
        int miso = pin_config_spi_miso(), cs = pin_config_spi_cs();
        if (sck < 0 || mosi < 0 || cs < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SPI pins not assigned");
            return ESP_FAIL;
        }
        /* Stop slave monitor if running on same pins */
        if (spi_mon_running()) spi_mon_stop();
        esp_err_t e = spi_send_start(sck, mosi, miso, cs, mode, clk);
        if (e != ESP_OK) {
            char emsg[64];
            snprintf(emsg, sizeof(emsg), "SPI init failed: %s", esp_err_to_name(e));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, emsg);
            return ESP_FAIL;
        }
    }

    uint8_t rx_buf[128] = {0};
    esp_err_t e = spi_send_bytes(tx_buf, rx_buf, tx_len);

    char json[512];
    int p = 0;
    p += snprintf(json + p, sizeof(json) - p, "{\"ok\":%s,\"rx\":[",
                  e == ESP_OK ? "true" : "false");
    if (e == ESP_OK) {
        for (int i = 0; i < tx_len; i++)
            p += snprintf(json + p, sizeof(json) - p, "%s%d", i ? "," : "", rx_buf[i]);
    }
    p += snprintf(json + p, sizeof(json) - p, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/i2c/send - send/receive I2C data.
 * Body: addr=0x50&hex=AABB&mode=w|r|wr */
static esp_err_t i2c_send_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';

    char addr_v[8] = {0}, hex_v[256] = {0}, mode_v[8] = {0}, len_v[8] = {0};
    httpd_query_key_value(body, "addr", addr_v, sizeof(addr_v));
    httpd_query_key_value(body, "hex", hex_v, sizeof(hex_v));
    httpd_query_key_value(body, "mode", mode_v, sizeof(mode_v));
    httpd_query_key_value(body, "len", len_v, sizeof(len_v));

    if (!addr_v[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need addr");
        return ESP_FAIL;
    }
    uint8_t addr = (uint8_t)strtol(addr_v, NULL, 0);

    if (!i2c_send_running()) {
        int sda = pin_config_i2c_sda(), scl = pin_config_i2c_scl();
        if (sda < 0 || scl < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "I2C pins not assigned");
            return ESP_FAIL;
        }
        if (i2c_mon_running()) i2c_mon_stop();
        esp_err_t e = i2c_send_start(sda, scl, 400000);
        if (e != ESP_OK) {
            char emsg[64];
            snprintf(emsg, sizeof(emsg), "I2C init failed: %s", esp_err_to_name(e));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, emsg);
            return ESP_FAIL;
        }
    }

    /* Parse hex data */
    uint8_t data_buf[128];
    int data_len = 0;
    for (int i = 0; hex_v[i] && hex_v[i + 1] && data_len < (int)sizeof(data_buf); i += 2) {
        char byte[3] = { hex_v[i], hex_v[i + 1], 0 };
        data_buf[data_len++] = (uint8_t)strtol(byte, NULL, 16);
    }

    esp_err_t e = ESP_OK;
    uint8_t rx_buf[128] = {0};
    int rx_len = 0;

    if (strcmp(mode_v, "r") == 0) {
        int rlen = len_v[0] ? atoi(len_v) : 1;
        if (rlen > 128) rlen = 128;
        e = i2c_read(addr, rx_buf, rlen);
        rx_len = (e == ESP_OK) ? rlen : 0;
    } else if (strcmp(mode_v, "wr") == 0 && data_len > 0) {
        int rlen = len_v[0] ? atoi(len_v) : 1;
        if (rlen > 128) rlen = 128;
        e = i2c_write_read(addr, data_buf, data_len, rx_buf, rlen);
        rx_len = (e == ESP_OK) ? rlen : 0;
    } else {
        if (data_len > 0) e = i2c_write(addr, data_buf, data_len);
    }

    char json[512];
    int p = 0;
    p += snprintf(json + p, sizeof(json) - p, "{\"ok\":%s,\"err\":\"%s\",\"rx\":[",
                  e == ESP_OK ? "true" : "false",
                  e == ESP_OK ? "" : esp_err_to_name(e));
    for (int i = 0; i < rx_len; i++)
        p += snprintf(json + p, sizeof(json) - p, "%s%d", i ? "," : "", rx_buf[i]);
    p += snprintf(json + p, sizeof(json) - p, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* ---------- AI config (NVS "aiconf") ---------- */
static void ai_nvs_load(char *base, int blen, char *model, int mlen, bool *has_key)
{
    nvs_handle_t h;
    *base = '\0'; *model = '\0'; *has_key = false;
    if (nvs_open("aiconf", NVS_READONLY, &h) != ESP_OK) return;
    size_t l = blen;  nvs_get_str(h, "base", base, &l);
    l = mlen;         nvs_get_str(h, "model", model, &l);
    l = 0;
    if (nvs_get_str(h, "key", NULL, &l) == ESP_OK && l > 1) *has_key = true;
    nvs_close(h);
}

static void ai_nvs_save(const char *base, const char *key, const char *model)
{
    nvs_handle_t h;
    if (nvs_open("aiconf", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "base", base);
    if (key && key[0]) nvs_set_str(h, "key", key);
    nvs_set_str(h, "model", model);
    nvs_commit(h);
    nvs_close(h);
}

/* GET /api/ai/config - return AI settings (key masked) */
static esp_err_t ai_config_get_handler(httpd_req_t *req)
{
    char base[128] = {0}, model[64] = {0};
    bool has_key = false;
    ai_nvs_load(base, sizeof(base), model, sizeof(model), &has_key);

    char json[256];
    snprintf(json, sizeof(json), "{\"base\":\"%s\",\"model\":\"%s\",\"hasKey\":%s}",
             base, model, has_key ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/ai/config - save AI settings (form: base=...&key=...&model=...) */
static esp_err_t ai_config_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';

    char base[128] = {0}, key[256] = {0}, model[64] = {0};
    httpd_query_key_value(body, "base", base, sizeof(base));
    httpd_query_key_value(body, "key", key, sizeof(key));
    httpd_query_key_value(body, "model", model, sizeof(model));
    url_decode_inplace(base);
    url_decode_inplace(key);
    url_decode_inplace(model);

    ai_nvs_save(base, key, model);
    ESP_LOGI(TAG, "AI config saved: base=%s model=%s", base, model);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

/* GET /api/swd/debug - SWD diagnostic. ?test=clk toggles SWCLK ~2kHz. */
static esp_err_t swd_debug_handler(httpd_req_t *req)
{
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char test[8] = {0};
    httpd_query_key_value(query, "test", test, sizeof(test));

    if (strcmp(test, "clk") == 0) {
        /* Toggle SWCLK for ~2 seconds so the user can measure it */
        int swclk = pin_config_swclk();
        gpio_set_direction(swclk, GPIO_MODE_OUTPUT);
        for (int i = 0; i < 4000; i++) {
            gpio_set_level(swclk, i & 1);
            vTaskDelay(1);  /* ~1ms per toggle with FreeRTOS 1kHz tick */
        }
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "SWCLK toggled 2s on IO, check with multimeter", -1);
    }
    if (strcmp(test, "io") == 0) {
        /* Drive SWDIO high/low so the user can measure */
        int swdio = pin_config_swdio();
        gpio_set_direction(swdio, GPIO_MODE_OUTPUT);
        for (int i = 0; i < 2000; i++) {
            gpio_set_level(swdio, i & 1);
            vTaskDelay(2);
        }
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "SWDIO toggled 4s, check with multimeter", -1);
    }

    uint32_t ack = swd_get_last_ack();
    const char *desc = "?";
    switch (ack) {
    case 1: desc = "OK"; break;
    case 2: desc = "WAIT"; break;
    case 4: desc = "FAULT"; break;
    case 0: case 7: desc = "NO_RESPONSE"; break;
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ack\":%lu,\"desc\":\"%s\"}",
             (unsigned long)ack, desc);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/wifi - scan and return SSID list as JSON array */
static esp_err_t wifi_scan_api_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { 0 };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > 20) ap_num = 20;

    wifi_ap_record_t aps[20];
    esp_wifi_scan_get_ap_records(&ap_num, aps);

    char json[2048];
    int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos, "[");

    /* Deduplicate SSIDs */
    char seen[20][33];
    int seen_n = 0;

    for (int i = 0; i < ap_num && pos < (int)sizeof(json) - 100; i++) {
        bool dup = false;
        for (int j = 0; j < seen_n; j++) {
            if (strcmp(seen[j], (char *)aps[i].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (seen_n < 20) {
            strncpy(seen[seen_n], (char *)aps[i].ssid, 32);
            seen[seen_n][32] = '\0';
            seen_n++;
        }
        if (pos > 1) pos += snprintf(json + pos, sizeof(json) - pos, ",");
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d}",
                        (char *)aps[i].ssid, aps[i].rssi);
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "]");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/wifi - set WiFi credentials and connect.
 * Body: "ssid=xxx&pass=yyy" (URL-encoded form data) */
static esp_err_t wifi_set_api_handler(httpd_req_t *req)
{
    char buf[256];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, buf + received, total - received);
        if (n <= 0) break;
        received += n;
    }
    buf[received] = '\0';

    char ssid[33] = {0}, pass[65] = {0};
    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "pass", pass, sizeof(pass));

    if (ssid[0]) {
        wifi_manager_set_credentials(ssid, pass);
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "OK", 2);
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ssid");
    return ESP_FAIL;
}

/* GET /api/baud?b=115200 - set baud rate */
static esp_err_t baud_api_handler(httpd_req_t *req)
{
    char buf[16] = {0};
    httpd_req_get_url_query_str(req, buf, sizeof(buf));
    char val[16] = {0};
    httpd_query_key_value(buf, "b", val, sizeof(val));
    if (val[0]) {
        uint32_t b = (uint32_t)strtoul(val, NULL, 10);
        serial_bridge_set_baud(b);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

/* POST /api/send - send data to DUT via UART */
static esp_err_t send_api_handler(httpd_req_t *req)
{
    char buf[512];
    int total = req->content_len;
    if (total <= 0 || total > (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, buf + received, total - received);
        if (n <= 0) break;
        received += n;
    }
    if (received > 0) {
        serial_bridge_write((uint8_t *)buf, received);
        /* Echo sent data onto the hardware LCD's RX monitor ('<' prefix) */
        menu_push_tx_data((uint8_t *)buf, received);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

esp_err_t http_status_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 25;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_index = { .uri="/", .method=HTTP_GET, .handler=index_handler };
    httpd_uri_t uri_status = { .uri="/api/status", .method=HTTP_GET, .handler=status_api_handler };
    httpd_uri_t uri_data = { .uri="/api/data", .method=HTTP_GET, .handler=data_api_handler };
    httpd_uri_t uri_mcp_data = { .uri="/api/mcp/data", .method=HTTP_GET, .handler=mcp_data_handler };
    httpd_uri_t uri_btn = { .uri="/api/btn", .method=HTTP_GET, .handler=btn_api_handler };
    httpd_uri_t uri_baud = { .uri="/api/baud", .method=HTTP_GET, .handler=baud_api_handler };
    httpd_uri_t uri_send = { .uri="/api/send", .method=HTTP_POST, .handler=send_api_handler };
    httpd_uri_t uri_wscan = { .uri="/api/wifi", .method=HTTP_GET, .handler=wifi_scan_api_handler };
    httpd_uri_t uri_wset = { .uri="/api/wifi", .method=HTTP_POST, .handler=wifi_set_api_handler };
    httpd_uri_t uri_lcd = { .uri="/api/lcd", .method=HTTP_GET, .handler=lcd_api_handler };
    httpd_uri_t uri_pins_get = { .uri="/api/pins", .method=HTTP_GET, .handler=pins_get_handler };
    httpd_uri_t uri_pins_post = { .uri="/api/pins", .method=HTTP_POST, .handler=pins_post_handler };
    httpd_uri_t uri_pwm = { .uri="/api/pwm", .method=HTTP_GET, .handler=pwm_api_handler };
    httpd_uri_t uri_spi = { .uri="/api/spi", .method=HTTP_GET, .handler=spi_api_handler };
    httpd_uri_t uri_i2c = { .uri="/api/i2c", .method=HTTP_GET, .handler=i2c_api_handler };
    httpd_uri_t uri_pwm_out = { .uri="/api/pwm/out", .method=HTTP_POST, .handler=pwm_out_handler };
    httpd_uri_t uri_spi_send = { .uri="/api/spi/send", .method=HTTP_POST, .handler=spi_send_handler };
    httpd_uri_t uri_i2c_send = { .uri="/api/i2c/send", .method=HTTP_POST, .handler=i2c_send_handler };
    httpd_uri_t uri_swd_debug = { .uri="/api/swd/debug", .method=HTTP_GET, .handler=swd_debug_handler };
    httpd_uri_t uri_capacity = { .uri="/api/capacity", .method=HTTP_GET, .handler=capacity_api_handler };
    httpd_uri_t uri_clear = { .uri="/api/clear", .method=HTTP_POST, .handler=clear_api_handler };
    httpd_uri_t uri_cfg = { .uri="/api/cfg", .method=HTTP_POST, .handler=cfg_set_handler };
    httpd_uri_t uri_ai_cfg_get = { .uri="/api/ai/config", .method=HTTP_GET, .handler=ai_config_get_handler };
    httpd_uri_t uri_ai_cfg_post = { .uri="/api/ai/config", .method=HTTP_POST, .handler=ai_config_post_handler };

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_data);
    httpd_register_uri_handler(server, &uri_mcp_data);
    httpd_register_uri_handler(server, &uri_btn);
    httpd_register_uri_handler(server, &uri_baud);
    httpd_register_uri_handler(server, &uri_send);
    httpd_register_uri_handler(server, &uri_wscan);
    httpd_register_uri_handler(server, &uri_wset);
    httpd_register_uri_handler(server, &uri_lcd);
    httpd_register_uri_handler(server, &uri_pins_get);
    httpd_register_uri_handler(server, &uri_pins_post);
    httpd_register_uri_handler(server, &uri_pwm);
    httpd_register_uri_handler(server, &uri_spi);
    httpd_register_uri_handler(server, &uri_i2c);
    httpd_register_uri_handler(server, &uri_pwm_out);
    httpd_register_uri_handler(server, &uri_spi_send);
    httpd_register_uri_handler(server, &uri_i2c_send);
    httpd_register_uri_handler(server, &uri_swd_debug);
    httpd_register_uri_handler(server, &uri_capacity);
    httpd_register_uri_handler(server, &uri_clear);
    httpd_register_uri_handler(server, &uri_cfg);
    httpd_register_uri_handler(server, &uri_ai_cfg_get);
    httpd_register_uri_handler(server, &uri_ai_cfg_post);

    ESP_LOGI(TAG, "HTTP status page on :80");
    return ESP_OK;
}
