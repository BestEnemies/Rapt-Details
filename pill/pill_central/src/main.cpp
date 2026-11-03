// RAPT Pill ESP32 tool — two modes, switchable from the web UI, wireless OTA.
//
//   BRIDGE   : BLE central. Connects to the Pill and relays commands/settings
//              to the control characteristic (…0006). 60s connect loop.
//   EMULATOR : BLE peripheral. Spoofs the Pill's BLE address + name + GATT
//              services and logs everything a client (the RAPT app) writes —
//              intended to capture the owner-auth secret for YOUR OWN Pill/app
//              (power the real Pill OFF during capture so the app finds us).
//
// Mode and spoof MAC are stored in NVS and applied at boot (reboots on change).
// Web UI at http://<ip>/ ; wireless OTA at /update and via ArduinoOTA.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ElegantOTA.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <NimBLEDevice.h>

static const char* TARGET_NAME = "RAPT Pill Hydrometer";
#define CMD_CHAR_UUID "52415054-0a06-9215-08bf-f2b018840006"
static const char* RAPT_SVC[] = {
  "52415054-0a06-9215-08bf-f2b018840000",
  "52415054-0a06-9215-08bf-f2b018840100",
  "52415054-0a06-9215-08bf-f2b018840200"};
// characteristic suffixes per service and whether writable/notify
struct ChDef { const char* svc; const char* uuid; bool w; bool n; };
static ChDef CHARS[] = {
  {RAPT_SVC[0], "52415054-0a06-9215-08bf-f2b018840004", true,  false},
  {RAPT_SVC[0], "52415054-0a06-9215-08bf-f2b018840005", true,  false},
  {RAPT_SVC[0], "52415054-0a06-9215-08bf-f2b018840006", true,  false},
  {RAPT_SVC[0], "52415054-0a06-9215-08bf-f2b018840007", true,  false},
  {RAPT_SVC[1], "52415054-0a06-9215-08bf-f2b018840101", true,  false},
  {RAPT_SVC[1], "52415054-0a06-9215-08bf-f2b018840102", true,  false},
  {RAPT_SVC[1], "52415054-0a06-9215-08bf-f2b018840103", true,  false},
  {RAPT_SVC[1], "52415054-0a06-9215-08bf-f2b018840104", true,  true },
  {RAPT_SVC[1], "52415054-0a06-9215-08bf-f2b018840105", false, true },
  {RAPT_SVC[2], "52415054-0a06-9215-08bf-f2b018840201", true,  false},
  {RAPT_SVC[2], "52415054-0a06-9215-08bf-f2b018840202", true,  false},
  {RAPT_SVC[2], "52415054-0a06-9215-08bf-f2b018840203", true,  false}};

WebServer server(80);
Preferences prefs;
String   g_mode = "bridge";               // "bridge" | "emulator"
String   g_mac  = "c4:5b:be:b2:be:c2";    // spoof/target BLE address (emulator)
SemaphoreHandle_t g_mtx;

// ---- shared log (both modes) ----
static String g_result = "idle";
static volatile bool g_busy = false;
static String g_cap = "";                 // capture log (emulator)
static void lock()   { xSemaphoreTake(g_mtx, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(g_mtx); }
static String esc(const String& s){ String o; for(size_t i=0;i<s.length();i++){char c=s[i];
  if(c=='"'||c=='\\'){o+='\\';o+=c;}else if(c=='\n')o+="\\n";else if(c>=32&&c<127)o+=c;} return o; }
static void setResult(const String& s){ lock(); g_result=s; unlock(); }
static String getResult(){ lock(); String s=g_result; unlock(); return s; }
static void addCap(const String& s){ lock(); g_cap = s + "\n" + g_cap; if(g_cap.length()>24000) g_cap=g_cap.substring(0,24000); unlock();
  Serial.println("CAP: "+s); }
static String getCap(){ lock(); String s=g_cap; unlock(); return s; }

static bool parseMac(const String& m, uint8_t out[6]){
  int v[6]; if(sscanf(m.c_str(), "%x:%x:%x:%x:%x:%x",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5])!=6) return false;
  for(int i=0;i<6;i++) out[i]=(uint8_t)v[i]; return true;
}

// =================== BRIDGE (central) ===================
struct Job { char uuid[48]; char payload[220]; };
static QueueHandle_t g_queue;
static String g_lastNotify = "";
static void notifyCB(NimBLERemoteCharacteristic* c, uint8_t* d, size_t n, bool){
  String s; for(size_t i=0;i<n;i++){char ch=d[i]; s+=(ch>=32&&ch<127)?ch:'.';}
  g_lastNotify = String(c->getUUID().toString().substr(28).c_str())+"="+s;
}
static String bleDo(const String& uuid, const String& payload){
  g_lastNotify="";
  NimBLEScan* scan=NimBLEDevice::getScan(); scan->setActiveScan(true);
  NimBLEClient* c=NimBLEDevice::createClient();
  bool up=false; int round=0; bool seen=false;
  unsigned long deadline=millis()+60000;
  Serial.println("bleDo: scan+connect up to 60s...");
  while(!up && (long)(deadline-millis())>0){
    round++;
    NimBLEScanResults r=scan->start(5,false);
    int idx=-1; NimBLEAdvertisedDevice dev;
    for(int i=0;i<r.getCount();i++){ NimBLEAdvertisedDevice d=r.getDevice(i);
      if(d.getAddress().toString()==g_mac.c_str() || (d.haveName()&&d.getName()==TARGET_NAME)){dev=d;idx=i;break;} }
    scan->clearResults();
    if(idx<0){ Serial.printf("  round %d: not seen\n",round); continue; }
    seen=true; Serial.printf("  round %d: connecting...\n",round);
    up=c->connect(&dev); if(!up){ delay(400); }
  }
  if(!up){ NimBLEDevice::deleteClient(c);
    return seen?"seen but no connect window in 60s (try again)":"Pill not found in 60s (power-cycle it)"; }
  c->secureConnection();
  for(auto s:*c->getServices(true)) if(s->getUUID().toString().rfind("52415054",0)==0)
    for(auto ch:*s->getCharacteristics(true)) if(ch->canNotify()) ch->subscribe(true,notifyCB);
  NimBLERemoteCharacteristic* ch=nullptr;
  for(auto s:*c->getServices()) for(auto x:*s->getCharacteristics())
    if(String(x->getUUID().toString().c_str()).equalsIgnoreCase(uuid)){ch=x;break;}
  if(!ch){ c->disconnect(); NimBLEDevice::deleteClient(c); return "characteristic not found: "+uuid; }
  bool ok=ch->writeValue((uint8_t*)payload.c_str(),payload.length(),true);
  delay(1800); bool held=c->isConnected();
  c->disconnect(); NimBLEDevice::deleteClient(c);
  String tail=g_lastNotify.length()?("  notify: "+g_lastNotify):"";
  if(ok&&held) return "ACCEPTED: "+payload+tail;
  if(ok&&!held) return "written, link dropped (refused / owner-auth?): "+payload+tail;
  return "REJECTED: "+payload+" (owner-auth enabled? power-cycle the Pill)"+tail;
}
static void bleTask(void*){ Job job;
  for(;;){ if(xQueueReceive(g_queue,&job,portMAX_DELAY)==pdTRUE){
    g_busy=true; setResult(bleDo(String(job.uuid),String(job.payload))); g_busy=false; } } }
static bool enqueue(const String& uuid,const String& payload){
  if(g_busy||uxQueueMessagesWaiting(g_queue)>0) return false;
  Job j; j.uuid[0]=0; j.payload[0]=0;
  strncpy(j.uuid,uuid.c_str(),sizeof(j.uuid)-1); strncpy(j.payload,payload.c_str(),sizeof(j.payload)-1);
  return xQueueSend(g_queue,&j,0)==pdTRUE;
}

// =================== EMULATOR (peripheral) ===================
class CapCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c){
    std::string v=c->getValue();
    String hex,asc; for(size_t i=0;i<v.size();i++){ char b=v[i]; char h[4]; sprintf(h,"%02x",(uint8_t)b); hex+=h;
      asc+=(b>=32&&b<127)?(char)b:'.'; }
    addCap(String(c->getUUID().toString().substr(28).c_str())+" <- ["+String((int)v.size())+"] "+hex+"  '"+asc+"'");
  }
  void onRead(NimBLECharacteristic* c){
    addCap(String("READ ")+c->getUUID().toString().substr(28).c_str());
  }
};
static CapCallbacks capCb;
class SrvCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* d){ addCap("** client CONNECTED **"); }
  void onDisconnect(NimBLEServer* s){ addCap("** client disconnected **"); NimBLEDevice::startAdvertising(); }
  void onAuthenticationComplete(ble_gap_conn_desc* d){ addCap(d->sec_state.encrypted?"** PAIRED (encrypted) **":"** pairing done (not encrypted) **"); }
};
static SrvCallbacks srvCb;
static void startEmulator(){
  addCap("emulator starting as "+g_mac+" / '"+String(TARGET_NAME)+"'");
  NimBLEDevice::setSecurityAuth(true,false,false);          // bonding, no MITM
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT); // Just Works
  NimBLEServer* srv=NimBLEDevice::createServer();
  srv->setCallbacks(&srvCb);
  // group chars by service
  for(int si=0; si<3; si++){
    NimBLEService* svc=srv->createService(NimBLEUUID(RAPT_SVC[si]));
    for(auto& cd:CHARS){ if(strcmp(cd.svc,RAPT_SVC[si])!=0) continue;
      uint32_t props=NIMBLE_PROPERTY::READ;
      if(cd.w) props|=NIMBLE_PROPERTY::WRITE|NIMBLE_PROPERTY::WRITE_NR;
      if(cd.n) props|=NIMBLE_PROPERTY::NOTIFY;
      NimBLECharacteristic* ch=svc->createCharacteristic(NimBLEUUID(cd.uuid),props);
      ch->setCallbacks(&capCb); ch->setValue((uint8_t*)"\x00",1);
    }
    svc->start();
  }
  NimBLEAdvertising* adv=NimBLEDevice::getAdvertising();
  adv->setName(TARGET_NAME);
  adv->addServiceUUID(NimBLEUUID(RAPT_SVC[0]));
  // RAPT-style manufacturer data (company 0x4152 + a device-info-ish payload)
  std::string md; md.push_back(0x52); md.push_back(0x41); // company id "RA"
  const char* tail="PT\xb7"; md.append(tail,3);
  uint8_t mb[6]; if(parseMac(g_mac,mb)){ mb[5]-=0; for(int i=0;i<6;i++) md.push_back(mb[i]); }
  adv->setManufacturerData(md);
  adv->setScanResponse(true);
  NimBLEDevice::startAdvertising();
  addCap("advertising; power the REAL Pill OFF, then open the app.");
}

// =================== web UI ===================
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Pill Bridge</title><style>
:root{color-scheme:light dark}body{font-family:system-ui,sans-serif;max-width:760px;margin:0 auto;padding:16px}
h1{font-size:1.25rem}button{font-size:1rem;padding:.5rem .8rem;margin:.2rem;border-radius:8px;border:1px solid #888;cursor:pointer}
.row{margin:.6rem 0}label{display:inline-block;min-width:9rem}input,select,textarea{font-size:1rem;padding:.4rem;border-radius:6px;border:1px solid #888}
#log,#cap{white-space:pre-wrap;background:#0001;padding:.6rem;border-radius:8px;min-height:3rem;max-height:55vh;overflow:auto;font-family:ui-monospace,monospace;font-size:.82rem}
fieldset{border:1px solid #8884;border-radius:10px;margin:.8rem 0}.danger{color:#c33}.pill{padding:.1rem .5rem;border-radius:999px;background:#88f3}
</style></head><body>
<h1>RAPT Pill tool &mdash; <span id=modebadge class=pill>…</span></h1>
<fieldset><legend>Mode &amp; identity</legend>
<div class=row>Mode: <b id=mode>…</b>
 <button onclick="setMode('bridge')">Bridge (control)</button>
 <button onclick="setMode('emulator')">Emulator (capture)</button></div>
<div class=row><label>Spoof BLE MAC:</label><input id=mac size=20> <button onclick="setMac()">Save MAC &amp; reboot</button></div>
<div class=row><small>Changing mode or MAC reboots the ESP32. Emulator spoofs this address; power the real Pill OFF during capture.</small></div>
</fieldset>
<div id=bridgeui>
<p>Commands land only while the Pill's owner-auth is off &mdash; power-cycle the Pill right before sending.</p>
<fieldset><legend>Quick</legend>
<button onclick="cmd('report_now')">report_now</button>
<button onclick="cmd('keep_awake')">keep_awake</button>
<button onclick="cmd('upload_log')">upload_log</button></fieldset>
<fieldset><legend>Command</legend>
<div class=row><select id=name onchange="renderFields()"></select> <span id=cmdnote></span></div>
<div id=fields></div>
<div class=row><button onclick="sendCmd()">Send</button></div></fieldset>
<fieldset><legend>Settings (desired)</legend>
<div class=row><label>BLE interval (s):</label><input id=s_ble size=6></div>
<div class=row><label>WiFi interval (s):</label><input id=s_wifi size=6></div>
<div class=row><label>Telemetry method:</label><select id=s_method><option value="">(unchanged)</option><option>ble</option><option>wifi</option></select></div>
<div class=row><label>Smart mode:</label><select id=s_smart><option value="">(unchanged)</option><option value="true">on</option><option value="false">off</option></select></div>
<div class=row><label>extra desired JSON:</label><input id=s_extra size=36 placeholder='{"gesture_wake":true}'></div>
<div class=row><button onclick="sendDesired()">Apply settings</button></div></fieldset>
<fieldset><legend>Advanced: raw write</legend>
<div class=row><label>char UUID:</label><input id=ruuid size=38 value="52415054-0a06-9215-08bf-f2b018840006"></div>
<div class=row><label>text:</label><input id=rtext size=38 placeholder='{"desired":{...}}'></div>
<div class=row><button onclick="raw()">Raw write</button></div></fieldset>
<div class=row><b>Status:</b></div><div id=log>idle</div>
</div>
<div id=emuui style=display:none>
<fieldset><legend>Emulator capture</legend>
<p>Impersonating the Pill. Power the real Pill OFF, open the RAPT app, let it connect/onboard. Writes (incl. any secret) appear below.</p>
<div class=row><button onclick="clearCap()">Clear</button></div>
<div id=cap>(waiting)</div></fieldset>
</div>
<script>
const CMDS={report_now:[],keep_awake:[],upload_log:[],stop_automation:[],cancel_calibration:[],clear_calibration:[],
start_calibration_water:[{k:'gravity',t:'num',d:'1.000'}],start_calibration_wort:[{k:'gravity',t:'num',d:'1.057'}],
start_automation:[{k:'frontendId',t:'text',d:''}],set_dev_options:[{k:'dev_options',t:'bool',d:1}],
set_beta_updates:[{k:'beta_updates',t:'bool',d:1},{k:'check',t:'bool',d:0}],
export:[{k:'what',t:'sel',o:['gesture_log','gravity_data','log']},{k:'page',t:'num',d:'0'}],
clear_wifi:[],clear_registration:[],factory_reset:[{k:'mac',t:'text',d:'c4:5b:be:b2:be:c0'}]};
const DANGER=['clear_wifi','clear_registration','factory_reset'];
const gel=id=>document.getElementById(id);
function initNames(){let s=gel('name');for(let n in CMDS){let o=document.createElement('option');o.textContent=n;s.appendChild(o);}renderFields();}
function renderFields(){let n=gel('name').value,f=gel('fields');f.innerHTML='';
 gel('cmdnote').className=DANGER.includes(n)?'danger':'';gel('cmdnote').textContent=DANGER.includes(n)?'(destructive)':'';
 for(let a of CMDS[n]){let row=document.createElement('div');row.className='row';
  let lab=document.createElement('label');lab.textContent=a.k+': ';row.appendChild(lab);let el;
  if(a.t=='bool'){el=document.createElement('input');el.type='checkbox';el.checked=!!a.d;}
  else if(a.t=='sel'){el=document.createElement('select');for(let o of a.o){let op=document.createElement('option');op.textContent=o;el.appendChild(op);}}
  else{el=document.createElement('input');el.type=(a.t=='num'?'number':'text');el.step='any';if(a.d!==undefined)el.value=a.d;}
  el.id='f_'+a.k;row.appendChild(el);f.appendChild(row);}}
function buildArgs(){let n=gel('name').value,o={};for(let a of CMDS[n]){let el=gel('f_'+a.k);if(!el)continue;
  if(a.t=='bool')o[a.k]=el.checked; else if(a.t=='num'){if(el.value!=='')o[a.k]=parseFloat(el.value);} else {if(el.value!=='')o[a.k]=el.value;}}return o;}
function sendCmd(){let n=gel('name').value,args=buildArgs();let a=Object.keys(args).length?JSON.stringify(args):'';cmd(n,a);}
async function cmd(n,a){ if(DANGER.includes(n)&&!confirm(n+' is destructive. Proceed?'))return;
  let u='/cmd?name='+encodeURIComponent(n); if(a)u+='&args='+encodeURIComponent(a); await fetch(u); poll(); }
function sendDesired(){ let d={}, g=id=>gel(id).value;
  if(g('s_ble'))d.telemetry_interval_ble=+g('s_ble'); if(g('s_wifi'))d.telemetry_interval_wifi=+g('s_wifi');
  if(g('s_method'))d.telemetry_method=g('s_method'); if(g('s_smart'))d.smart_mode=(g('s_smart')==='true');
  if(g('s_extra').trim()){try{Object.assign(d,JSON.parse(g('s_extra').trim()));}catch(e){alert('bad extra JSON');return;}}
  fetch('/raw?uuid=52415054-0a06-9215-08bf-f2b018840006&text='+encodeURIComponent(JSON.stringify({desired:d}))).then(poll); }
async function raw(){ await fetch('/raw?uuid='+encodeURIComponent(gel('ruuid').value)+'&text='+encodeURIComponent(gel('rtext').value)); poll(); }
async function poll(){ for(let i=0;i<75;i++){ await new Promise(r=>setTimeout(r,1000));
  try{let s=await(await fetch('/status')).json(); gel('log').textContent=s.result; if(!s.busy)break;}catch(e){} } }
async function setMode(m){ if(confirm('Switch to '+m+' and reboot?')) await fetch('/mode?set='+m); }
async function setMac(){ if(confirm('Save MAC and reboot?')) await fetch('/setmac?mac='+encodeURIComponent(gel('mac').value)); }
async function clearCap(){ await fetch('/clearcap'); }
async function refresh(){ try{let s=await(await fetch('/status')).json();
  gel('mode').textContent=s.mode; gel('modebadge').textContent=s.mode; if(document.activeElement!==gel('mac'))gel('mac').value=s.mac;
  gel('bridgeui').style.display=s.mode=='bridge'?'block':'none'; gel('emuui').style.display=s.mode=='emulator'?'block':'none';
  if(s.mode=='bridge'&&!s.busy)gel('log').textContent=s.result;
  if(s.mode=='emulator'){let c=await(await fetch('/captures')).text(); gel('cap').textContent=c||'(waiting)';}
 }catch(e){} }
setInterval(refresh,2000); initNames(); refresh();
</script></body></html>)HTML";

void handleRoot(){ server.send_P(200,"text/html",PAGE); }
void handleStatus(){ server.send(200,"application/json",
  String("{\"busy\":")+(g_busy?"true":"false")+",\"mode\":\""+g_mode+"\",\"mac\":\""+g_mac+"\",\"result\":\""+esc(getResult())+"\"}"); }
void handleCaptures(){ server.send(200,"text/plain",getCap()); }
void handleClearCap(){ lock(); g_cap=""; unlock(); server.send(200,"text/plain","ok"); }
void handleCmd(){
  if(g_mode!="bridge"){ server.send(409,"application/json","{\"ok\":false,\"detail\":\"not in bridge mode\"}"); return; }
  String name=server.arg("name"),args=server.arg("args");
  if(!name.length()){ server.send(400,"application/json","{\"ok\":false}"); return; }
  String p="{\"cmd\":\""+name+"\""; if(args.length())p+=",\"args\":"+args; p+="}";
  bool q=enqueue(CMD_CHAR_UUID,p); if(q)setResult("queued: "+p);
  server.send(200,"application/json",String("{\"ok\":")+(q?"true":"false")+"}"); }
void handleRaw(){
  if(g_mode!="bridge"){ server.send(409,"application/json","{\"ok\":false}"); return; }
  bool q=enqueue(server.arg("uuid"),server.arg("text"));
  if(q)setResult("queued raw: "+server.arg("uuid")+" <- "+server.arg("text"));
  server.send(200,"application/json",String("{\"ok\":")+(q?"true":"false")+"}"); }
void handleMode(){ String m=server.arg("set"); if(m!="bridge"&&m!="emulator"){ server.send(400,"text/plain","bad"); return; }
  prefs.begin("cfg",false); prefs.putString("mode",m); prefs.end();
  server.send(200,"text/plain","rebooting into "+m); delay(300); ESP.restart(); }
void handleSetMac(){ uint8_t mb[6]; String m=server.arg("mac");
  if(!parseMac(m,mb)){ server.send(400,"text/plain","bad mac (aa:bb:cc:dd:ee:ff)"); return; }
  prefs.begin("cfg",false); prefs.putString("mac",m); prefs.end();
  server.send(200,"text/plain","saved "+m+", rebooting"); delay(300); ESP.restart(); }

void setup(){
  Serial.begin(115200); delay(300);
  g_mtx=xSemaphoreCreateMutex();
  prefs.begin("cfg",true); g_mode=prefs.getString("mode","bridge"); g_mac=prefs.getString("mac","c4:5b:be:b2:be:c2"); prefs.end();
  Serial.printf("\n== Pill tool == mode=%s spoofMAC=%s\n", g_mode.c_str(), g_mac.c_str());

  // Apply spoof MAC (emulator): BT address = base MAC + 2, so base = target - 2.
  if(g_mode=="emulator"){ uint8_t mb[6]; if(parseMac(g_mac,mb)){ uint8_t base[6]; memcpy(base,mb,6);
    base[5]=(uint8_t)(base[5]-2); esp_base_mac_addr_set(base); Serial.println("base MAC set for spoofing"); } }

  WiFiManager wm; wm.setConfigPortalTimeout(180);
  if(!wm.autoConnect("PillBridge-setup")){ ESP.restart(); }
  Serial.printf("WiFi OK, http://%s/\n", WiFi.localIP().toString().c_str());

  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  if(g_mode=="emulator"){
    startEmulator();
  } else {
    NimBLEDevice::setSecurityAuth(true,false,true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    g_queue=xQueueCreate(4,sizeof(Job));
    xTaskCreatePinnedToCore(bleTask,"bleTask",20480,nullptr,1,nullptr,1);
  }

  ArduinoOTA.setHostname("pill-bridge"); ArduinoOTA.begin();
  ElegantOTA.begin(&server);
  server.on("/",handleRoot); server.on("/status",handleStatus);
  server.on("/cmd",handleCmd); server.on("/raw",handleRaw);
  server.on("/captures",handleCaptures); server.on("/clearcap",handleClearCap);
  server.on("/mode",handleMode); server.on("/setmac",handleSetMac);
  server.begin();
  Serial.println("web + OTA up.");
}
void loop(){ server.handleClient(); ElegantOTA.loop(); ArduinoOTA.handle(); delay(2); }
