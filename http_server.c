#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "mbedtls/base64.h"
#include "router_config.h"
#include "wifi_config.h"
#include "http_server.h"

extern esp_netif_t *wifiAP;
extern esp_netif_t *wifiSTA;
extern void router_reconnect_uplink(void);
extern esp_err_t router_apply_ap_config(void);

static volatile bool dns_started = false;
/* Serialize deferred AP reconfiguration. Multiple fast clicks must not
 * create competing tasks that race esp_wifi_set_config() with each other.
 * ap_apply_dirty closes a narrow window the plain pending-flag version
 * left open: a save landing while apply_ap_task is *inside*
 * router_apply_ap_config() (not just during its 1s delay) would see
 * pending still true and skip scheduling - and the running task would
 * then apply the config it read before that save, clear pending, and
 * exit, silently dropping the newest settings until the next unrelated
 * save. Setting dirty=true in that window makes the task loop and
 * re-apply once more instead of exiting, so no save is ever dropped. */
static portMUX_TYPE ap_apply_mux = portMUX_INITIALIZER_UNLOCKED;
static bool ap_apply_pending = false;
static bool ap_apply_dirty = false;
static const char *TAG = "http_server";

/* ------------------------------------------------------------------ */
/* Premium single-page dashboard/config UI. Fully self-contained: no  */
/* external CSS/JS/fonts, so it still works from the captive portal   */
/* before the router has any internet uplink at all.                 */
/* ------------------------------------------------------------------ */
static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
"<title>ESP32-S3 NAT Router</title>"
"<style>"
":root{--bg1:#0b0f1a;--bg2:#131b2e;--accent:#6ee7ff;--accent2:#a78bfa;--good:#34d399;--bad:#fb7185;--warn:#fbbf24;"
"--card:rgba(255,255,255,.05);--card-b:rgba(255,255,255,.09);--text:#e7ecf5;--dim:#8b93a7;}"
"*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}"
"html,body{margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;color:var(--text);"
"background:radial-gradient(1200px 600px at 15% -10%,#1b2a4a 0%,transparent 60%),"
"radial-gradient(1000px 500px at 110% 10%,#3a1f57 0%,transparent 55%),linear-gradient(180deg,var(--bg1),var(--bg2));"
"min-height:100vh;padding:22px 16px 90px}"
".wrap{max-width:760px;margin:0 auto}"
"header{display:flex;align-items:center;gap:14px;margin-bottom:22px}"
".logo{width:46px;height:46px;border-radius:14px;display:flex;align-items:center;justify-content:center;font-size:22px;"
"background:linear-gradient(135deg,var(--accent),var(--accent2));box-shadow:0 6px 24px rgba(110,231,255,.25)}"
"h1{font-size:19px;margin:0;font-weight:700;letter-spacing:.2px}"
".sub{color:var(--dim);font-size:12.5px;margin-top:2px}"
".card{background:var(--card);border:1px solid var(--card-b);backdrop-filter:blur(18px);-webkit-backdrop-filter:blur(18px);"
"border-radius:18px;padding:18px;margin-bottom:16px;box-shadow:0 10px 30px rgba(0,0,0,.25)}"
".card h2{font-size:13px;text-transform:uppercase;letter-spacing:1.2px;color:var(--dim);margin:0 0 14px;font-weight:700}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px}"
".stat{background:rgba(255,255,255,.03);border:1px solid var(--card-b);border-radius:14px;padding:12px 14px}"
".stat .k{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.8px}"
".stat .v{font-size:17px;font-weight:700;margin-top:4px;word-break:break-all}"
".pill{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;border-radius:999px;font-size:12px;font-weight:600}"
".pill.on{background:rgba(52,211,153,.15);color:var(--good)}"
".pill.off{background:rgba(251,113,133,.15);color:var(--bad)}"
".dot{width:7px;height:7px;border-radius:50%;background:currentColor;box-shadow:0 0 8px currentColor}"
".sigbars{display:inline-flex;align-items:flex-end;gap:2px;height:14px;vertical-align:middle;margin-right:6px}"
".sigbars i{width:3px;background:var(--dim);border-radius:1px;display:block}"
".sigbars i:nth-child(1){height:25%}.sigbars i:nth-child(2){height:50%}.sigbars i:nth-child(3){height:75%}.sigbars i:nth-child(4){height:100%}"
".sigbars.a1 i:nth-child(1){background:var(--accent)}"
".sigbars.a2 i:nth-child(1),.sigbars.a2 i:nth-child(2){background:var(--accent)}"
".sigbars.a3 i:nth-child(1),.sigbars.a3 i:nth-child(2),.sigbars.a3 i:nth-child(3){background:var(--accent)}"
".sigbars.a4 i{background:var(--accent)}"
"label{display:block;font-size:12px;color:var(--dim);margin:12px 0 6px;font-weight:600}"
"input{width:100%;padding:12px 13px;border-radius:12px;border:1px solid var(--card-b);background:rgba(255,255,255,.04);"
"color:var(--text);font-size:14.5px;outline:none;transition:border-color .15s,background .15s}"
"input:focus{border-color:var(--accent);background:rgba(255,255,255,.07)}"
"input::placeholder{color:#59617a}"
"button{cursor:pointer;border:none;border-radius:12px;padding:12px 16px;font-size:14px;font-weight:700;"
"transition:transform .1s,opacity .15s}"
"button:active{transform:scale(.97)}"
"button:disabled{opacity:.5;cursor:not-allowed}"
".btn-primary{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#0b0f1a;width:100%;margin-top:14px}"
".btn-ghost{background:rgba(255,255,255,.06);color:var(--text);border:1px solid var(--card-b)}"
".row{display:flex;gap:10px}"
".netlist{margin-top:10px;display:flex;flex-direction:column;gap:6px;max-height:230px;overflow-y:auto}"
".netitem{display:flex;align-items:center;justify-content:space-between;background:rgba(255,255,255,.03);"
"border:1px solid var(--card-b);border-radius:12px;padding:9px 12px;font-size:13.5px}"
".netitem span.ssid{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1;margin-right:8px}"
".netitem button{padding:6px 12px;font-size:12px;background:rgba(110,231,255,.15);color:var(--accent)}"
".hint{color:var(--dim);font-size:12px;margin-top:8px;line-height:1.5}"
".banner{background:linear-gradient(135deg,rgba(251,191,36,.15),rgba(251,191,36,.05));border:1px solid rgba(251,191,36,.3);"
"border-radius:14px;padding:13px 15px;margin-bottom:16px;font-size:13.5px;color:#fde68a;display:flex;gap:10px;align-items:flex-start}"
".modal-bg{position:fixed;inset:0;background:rgba(5,7,14,.72);backdrop-filter:blur(4px);display:none;"
"align-items:center;justify-content:center;z-index:50;padding:20px}"
".modal-bg.show{display:flex}"
".modal{background:var(--bg2);border:1px solid var(--card-b);border-radius:20px;padding:22px;width:100%;max-width:360px;"
"box-shadow:0 20px 60px rgba(0,0,0,.5)}"
".modal h3{margin:0 0 4px;font-size:16px}"
".modal p{color:var(--dim);font-size:12.5px;margin:0 0 10px}"
"#toasts{position:fixed;left:0;right:0;bottom:18px;display:flex;flex-direction:column;align-items:center;gap:8px;z-index:99;pointer-events:none}"
".toast{pointer-events:auto;max-width:90vw;background:#161f36;border:1px solid var(--card-b);color:var(--text);"
"padding:11px 16px;border-radius:12px;font-size:13.5px;box-shadow:0 10px 30px rgba(0,0,0,.4);"
"animation:up .18s ease-out}"
".toast.err{border-color:rgba(251,113,133,.4)}.toast.ok{border-color:rgba(52,211,153,.4)}"
"@keyframes up{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}"
".lock-badge{font-size:11px;color:var(--dim);display:flex;align-items:center;gap:5px}"
"footer{text-align:center;color:var(--dim);font-size:11.5px;margin-top:22px}"
"</style></head><body>"
"<div class='wrap'>"
"<header><div class='logo'>&#128225;</div><div><h1>ESP32-S3 NAT Router</h1>"
"<div class='sub'>Wi-Fi uplink &middot; NAT/AP &middot; live status</div></div></header>"
"<div id='setupBanner' class='banner' style='display:none'>&#9888;&#65039;&nbsp;<div><b>Finish setup:</b> set an admin "
"password to protect your Wi-Fi settings from anyone on this network. <button class='btn-ghost' style='margin-top:8px;padding:8px 12px;font-size:12px' onclick=\"openModal('setupModal')\">Set admin password</button></div></div>"
"<div class='card'><h2>Status</h2><div class='grid' id='statGrid'>"
"<div class='stat'><div class='k'>Uplink</div><div class='v' id='sUplink'>&mdash;</div></div>"
"<div class='stat'><div class='k'>IP Address</div><div class='v' id='sIp'>&mdash;</div></div>"
"<div class='stat'><div class='k'>Signal</div><div class='v' id='sRssi'>&mdash;</div></div>"
"<div class='stat'><div class='k'>Uptime</div><div class='v' id='sUptime'>&mdash;</div></div>"
"<div class='stat'><div class='k'>Downloaded</div><div class='v' id='sRx'>&mdash;</div></div>"
"<div class='stat'><div class='k'>Uploaded</div><div class='v' id='sTx'>&mdash;</div></div>"
"<div class='stat'><div class='k'>AP Clients</div><div class='v' id='sClients'>&mdash;</div></div>"
"<div class='stat'><div class='k'>AP Name</div><div class='v' id='sApName'>&mdash;</div></div>"
"</div></div>"
"<div class='card'><h2>Internet Uplink</h2>"
"<div class='row'><button class='btn-ghost' style='flex:1' onclick='scan()'>&#128260;&nbsp;Scan networks</button></div>"
"<div class='netlist' id='netlist'></div>"
"<form onsubmit='connectWifi(event)'>"
"<label>Network name (SSID)</label><input id='ssid' maxlength='32' placeholder='Enter or pick from scan above' autocomplete='off'>"
"<label>Password</label><input id='pass' type='password' maxlength='64' placeholder='Wi-Fi password' autocomplete='off'>"
"<button class='btn-primary' type='submit'>Connect &amp; Save</button>"
"</form><div class='hint' id='wmsg'></div></div>"
"<div class='card'><h2>Access Point</h2>"
"<form onsubmit='saveAP(event)'>"
"<label>AP name (broadcast to your devices)</label><input id='apssid' maxlength='32' placeholder='AP SSID' autocomplete='off'>"
"<label>AP password</label><input id='appass' type='password' maxlength='64' placeholder='8+ characters, or blank for open' autocomplete='off'>"
"<button class='btn-primary' type='submit'>Save Access Point</button>"
"</form><div class='hint' id='amsg'></div></div>"
"<div class='card'><h2>Security</h2>"
"<div class='lock-badge' id='authBadge'>&#128274;&nbsp;<span id='authState'>Not signed in</span></div>"
"<div class='row' style='margin-top:12px'>"
"<button class='btn-ghost' style='flex:1' id='signInBtn' onclick=\"openModal('loginModal')\">Sign in</button>"
"<button class='btn-ghost' style='flex:1' onclick=\"openModal('changeModal')\">Change password</button>"
"</div></div>"
"<footer>Served locally by the router &middot; no data leaves your network</footer>"
"</div>"
"<div class='modal-bg' id='setupModal'><div class='modal'><h3>Set admin password</h3>"
"<p>Required once, protects the Uplink and AP settings above from other devices on this network.</p>"
"<label>Username</label><input id='setupUser' value='admin' maxlength='32'>"
"<label>Password (min 8 characters)</label><input id='setupPass' type='password' maxlength='64'>"
"<button class='btn-primary' onclick='doSetup()'>Save &amp; sign in</button>"
"<button class='btn-ghost' style='width:100%;margin-top:8px' onclick=\"closeModal('setupModal')\">Cancel</button></div></div>"
"<div class='modal-bg' id='loginModal'><div class='modal'><h3>Sign in</h3>"
"<p>Enter your admin credentials to change Uplink or AP settings.</p>"
"<label>Username</label><input id='loginUser' maxlength='32' autocomplete='username'>"
"<label>Password</label><input id='loginPass' type='password' maxlength='64' autocomplete='current-password'>"
"<button class='btn-primary' onclick='doLogin()'>Sign in</button>"
"<button class='btn-ghost' style='width:100%;margin-top:8px' onclick=\"closeModal('loginModal')\">Cancel</button></div></div>"
"<div class='modal-bg' id='changeModal'><div class='modal'><h3>Change admin password</h3>"
"<p>You must already be signed in.</p>"
"<label>New username</label><input id='chgUser' maxlength='32'>"
"<label>New password (min 8 characters)</label><input id='chgPass' type='password' maxlength='64'>"
"<button class='btn-primary' onclick='doChangeAdmin()'>Update</button>"
"<button class='btn-ghost' style='width:100%;margin-top:8px' onclick=\"closeModal('changeModal')\">Cancel</button></div></div>"
"<div id='toasts'></div>"
"<script>"
"const $=id=>document.getElementById(id);"
"function openModal(id){$(id).classList.add('show')}"
"function closeModal(id){$(id).classList.remove('show')}"
"function toast(msg,kind){const t=document.createElement('div');t.className='toast '+(kind||'');t.textContent=msg;"
"$('toasts').appendChild(t);setTimeout(()=>t.remove(),3400)}"
"function creds(){try{return JSON.parse(sessionStorage.getItem('auth')||'null')}catch(e){return null}}"
"function setCreds(u,p){sessionStorage.setItem('auth',JSON.stringify({u,p}));renderAuth()}"
"function clearCreds(){sessionStorage.removeItem('auth');renderAuth()}"
"function renderAuth(){const c=creds();$('authState').textContent=c?('Signed in as '+c.u):'Not signed in';"
"$('signInBtn').textContent=c?'Sign out':'Sign in';$('signInBtn').onclick=c?clearCreds:()=>openModal('loginModal')}"
"async function api(u,o,auth){o=o||{};o.headers=o.headers||{};if(auth){const c=creds();if(c)o.headers['Authorization']='Basic '+btoa(c.u+':'+c.p)}"
"const r=await fetch(u,o);let x=null;try{x=await r.json()}catch(e){}"
"if(r.status===401){if(auth)clearCreds();throw new Error((x&&x.message)||'Sign in required')}"
"if(!r.ok)throw new Error((x&&x.message)||'Request failed');return x}"
"let apInitialized=false;"
"function sigClass(rssi){if(rssi>=-55)return'a4';if(rssi>=-67)return'a3';if(rssi>=-78)return'a2';return'a1'}"
"function sigBars(rssi){return '<span class=\"sigbars '+sigClass(rssi)+'\"><i></i><i></i><i></i><i></i></span>'}"
"async function refreshStatus(){try{const x=await api('/api/status');"
"const up=x.uplink==='Connected';$('sUplink').innerHTML=(up?'<span class=\"pill on\"><span class=\"dot\"></span>Connected</span>':'<span class=\"pill off\"><span class=\"dot\"></span>Disconnected</span>');"
"$('sIp').textContent=x.ip;$('sRssi').innerHTML=up?(sigBars(x.rssi)+x.rssi+' dBm'):'&mdash;';"
"$('sUptime').textContent=x.uptime;$('sRx').textContent=x.rx;$('sTx').textContent=x.tx;"
"$('sClients').textContent=x.clients;$('sApName').textContent=x.ap_ssid;"
"$('setupBanner').style.display=x.admin_set?'none':'flex';"
"if(!apInitialized){$('apssid').value=x.ap_ssid;apInitialized=true}}catch(e){}}"
"function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;')}"
"async function scan(){$('netlist').innerHTML='<div class=\"hint\">Scanning…</div>';"
"try{const x=await api('/api/scan',{method:'POST'},true);"
"$('netlist').innerHTML=x.networks.map(n=>n.hidden?'<div class=\"netitem\"><span class=\"ssid\">Hidden network</span>'+sigBars(n.rssi)+n.rssi+' dBm</div>'"
":'<div class=\"netitem\"><span class=\"ssid\">'+esc(n.ssid)+'</span>'+sigBars(n.rssi)+n.rssi+' dBm&nbsp;&nbsp;<button data-s=\"'+encodeURIComponent(n.ssid)+'\">Use</button></div>').join('')"
"||'<div class=\"hint\">No networks found</div>';"
"document.querySelectorAll('[data-s]').forEach(b=>b.onclick=()=>{$('ssid').value=decodeURIComponent(b.dataset.s);$('pass').focus()})}"
"catch(e){$('netlist').innerHTML='';toast('Scan failed: '+e.message,'err')}}"
"async function connectWifi(e){e.preventDefault();const b=new URLSearchParams();b.set('ssid',$('ssid').value.trim());b.set('pass',$('pass').value);"
"try{const x=await api('/api/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b},true);"
"toast(x.message,'ok');setTimeout(refreshStatus,1200)}catch(e){toast(e.message,'err')}}"
"async function saveAP(e){e.preventDefault();const b=new URLSearchParams();b.set('ssid',$('apssid').value.trim());b.set('pass',$('appass').value);"
"try{const x=await api('/api/ap',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b},true);"
"toast(x.message,'ok');apInitialized=true}catch(e){toast(e.message,'err')}}"
"async function doSetup(){const u=$('setupUser').value.trim(),p=$('setupPass').value;if(p.length<8){toast('Password needs 8+ characters','err');return}"
"const b=new URLSearchParams();b.set('user',u);b.set('pass',p);"
"try{await api('/api/setup',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});"
"setCreds(u,p);closeModal('setupModal');toast('Admin password set — you are signed in','ok');refreshStatus()}catch(e){toast(e.message,'err')}}"
"function doLogin(){const u=$('loginUser').value.trim(),p=$('loginPass').value;if(!u||!p){toast('Enter username and password','err');return}"
"setCreds(u,p);closeModal('loginModal');toast('Signed in as '+u,'ok')}"
"async function doChangeAdmin(){const u=$('chgUser').value.trim(),p=$('chgPass').value;if(p.length<8){toast('Password needs 8+ characters','err');return}"
"const b=new URLSearchParams();b.set('user',u);b.set('pass',p);"
"try{await api('/api/setup',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b},true);"
"setCreds(u,p);closeModal('changeModal');toast('Admin credentials updated','ok')}catch(e){toast(e.message,'err')}}"
"renderAuth();refreshStatus();setInterval(refreshStatus,3000);"
"</script></body></html>";

/* ------------------------------------------------------------------ */
/* Auth helpers                                                        */
/* ------------------------------------------------------------------ */

static bool constant_time_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    volatile uint8_t diff = (uint8_t)(la != lb);
    size_t n = la > lb ? la : lb;
    for (size_t i = 0; i < n; ++i) {
        char ca = i < la ? a[i] : 0;
        char cb = i < lb ? b[i] : 0;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff == 0;
}

/* Returns true if the request carries valid admin Basic-Auth credentials. */
static bool check_auth(httpd_req_t *req)
{
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (strncmp(hdr, "Basic ", 6) != 0) return false;

    unsigned char decoded[128] = {0};
    size_t out_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                               (const unsigned char *)hdr + 6, strlen(hdr + 6)) != 0) {
        return false;
    }
    decoded[out_len] = '\0';

    char *sep = strchr((char *)decoded, ':');
    if (!sep) return false;
    *sep = '\0';
    const char *user = (const char *)decoded;
    const char *pass = sep + 1;

    wifi_config_lock();
    bool ok = admin_pass && admin_pass[0] &&
              constant_time_eq(user, admin_user) &&
              constant_time_eq(pass, admin_pass);
    wifi_config_unlock();
    return ok;
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    /* Tiny fixed delay blunts trivial online brute-forcing without the
     * complexity of a persistent lockout counter. */
    vTaskDelay(pdMS_TO_TICKS(300));
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"router\"");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"message\":\"Sign in required\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Returns true and lets the caller proceed; sends the 401 response and
 * returns false otherwise. */
static bool require_auth(httpd_req_t *req)
{
    if (check_auth(req)) return true;
    send_unauthorized(req);
    return false;
}

/* ------------------------------------------------------------------ */

/* Escapes a string for embedding as a JSON string value. The previous
 * version of this handled only " and \ - any control byte (e.g. a
 * crafted/malformed SSID containing a raw newline or tab) would land
 * in the response unescaped and corrupt the JSON. This covers the full
 * control-character range per the JSON spec (RFC 8259 section 7). */
static size_t json_escape(const char *in, char *out, size_t out_cap)
{
    size_t w = 0;
    if (!out_cap) return 0;
    for (size_t i = 0; in && in[i] && w + 1 < out_cap; ++i) {
        unsigned char c = (unsigned char)in[i];
        char rep[8];
        const char *r = NULL;
        switch (c) {
            case '"':  r = "\\\""; break;
            case '\\': r = "\\\\"; break;
            case '\n': r = "\\n";  break;
            case '\r': r = "\\r";  break;
            case '\t': r = "\\t";  break;
            case '\b': r = "\\b";  break;
            case '\f': r = "\\f";  break;
            default:
                if (c < 0x20) {
                    snprintf(rep, sizeof(rep), "\\u%04x", c);
                    r = rep;
                }
        }
        if (r) {
            size_t rl = strlen(r);
            if (w + rl >= out_cap) break;
            memcpy(out + w, r, rl);
            w += rl;
        } else {
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
    return w;
}

static void json_error(httpd_req_t *req, int code, const char *msg)
{
    char out[192];
    snprintf(out, sizeof(out), "{\"ok\":false,\"message\":\"%s\"}", msg);
    httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap = {0};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    char uptime[32];
    format_uptime(get_uptime_seconds(), uptime, sizeof(uptime));

    char ipbuf[16] = "-";
    if (ap_connect) {
        ip4addr_ntoa_r((const ip4_addr_t *)&my_ip, ipbuf, sizeof(ipbuf));
    }

    wifi_config_lock();
    char apbuf[200];
    json_escape(ap_ssid, apbuf, sizeof(apbuf));
    bool admin_set = admin_pass && admin_pass[0];
    wifi_config_unlock();

    char out[750];
    snprintf(out, sizeof(out),
             "{\"uplink\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"uptime\":\"%s\",\"rx\":\"%.2f MB\",\"tx\":\"%.2f MB\",\"clients\":%u,\"ap_ssid\":\"%s\",\"admin_set\":%s}",
             ap_connect ? "Connected" : "Disconnected", ipbuf, rssi, uptime,
             (double)get_sta_bytes_received() / 1048576.0,
             (double)get_sta_bytes_sent() / 1048576.0,
             connect_count, apbuf, admin_set ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static bool url_decode(const char *src, size_t len, char *out, size_t out_len)
{
    size_t w = 0;
    if (!out || out_len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (w + 1 >= out_len) return false;
        if (src[i] == '+') {
            out[w++] = ' ';
        } else if (src[i] == '%' && i + 2 < len) {
            unsigned v = 0;
            if (sscanf(src + i + 1, "%2x", &v) != 1) return false;
            out[w++] = (char)v;
            i += 2;
        } else {
            out[w++] = src[i];
        }
    }
    out[w] = '\0';
    return true;
}

#define FORM_BODY_MAX 400
#define FORM_RECV_TIMEOUT_US (5 * 1000 * 1000)

/* Generic "two urlencoded fields" form reader, reused for ssid/pass and
 * user/pass bodies. The receive loop is time-bounded so a stalled or
 * malicious slow client cannot pin down the single HTTP worker task
 * indefinitely (a prior version looped on RECV timeouts with no cap). */
static bool read_form_fields(httpd_req_t *req,
                              const char *field1, char *out1, size_t out1_len,
                              const char *field2, char *out2, size_t out2_len)
{
    int len = req->content_len;
    if (len <= 0 || len > FORM_BODY_MAX) return false;

    char body[FORM_BODY_MAX + 1];
    int got = 0;
    int64_t deadline = esp_timer_get_time() + FORM_RECV_TIMEOUT_US;
    while (got < len) {
        if (esp_timer_get_time() > deadline) return false;
        int n = httpd_req_recv(req, body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return false;
        got += n;
    }
    body[len] = '\0';

    bool have1 = false, have2 = false;
    char *save = NULL;
    for (char *field = strtok_r(body, "&", &save);
         field;
         field = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(field, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *value = eq + 1;
        size_t value_len = strlen(value);
        if (strcmp(field, field1) == 0) {
            have1 = url_decode(value, value_len, out1, out1_len);
        } else if (strcmp(field, field2) == 0) {
            have2 = url_decode(value, value_len, out2, out2_len);
        }
    }
    return have1 && have2;
}

static esp_err_t connect_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char s[33], p[65];
    if (!read_form_fields(req, "ssid", s, sizeof(s), "pass", p, sizeof(p))) {
        json_error(req, 400, "SSID and password are required");
        return ESP_OK;
    }

    esp_err_t err = wifi_config_save_sta(s, p);
    if (err != ESP_OK) {
        json_error(req, 400, "Invalid Wi-Fi settings");
        return ESP_OK;
    }

    router_reconnect_uplink();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Uplink saved; connecting...\"}");
}

static void apply_ap_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Let the HTTP response leave before credentials are applied. */
        vTaskDelay(pdMS_TO_TICKS(1000));

        esp_err_t err = router_apply_ap_config();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AP configuration apply failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "AP configuration applied successfully");
        }

        portENTER_CRITICAL(&ap_apply_mux);
        if (ap_apply_dirty) {
            /* A newer save landed while we were applying - loop and
             * pick it up instead of exiting and dropping it. */
            ap_apply_dirty = false;
            portEXIT_CRITICAL(&ap_apply_mux);
            continue;
        }
        ap_apply_pending = false;
        portEXIT_CRITICAL(&ap_apply_mux);
        break;
    }
    vTaskDelete(NULL);
}

static esp_err_t ap_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    char s[33], p[65];
    if (!read_form_fields(req, "ssid", s, sizeof(s), "pass", p, sizeof(p))) {
        json_error(req, 400, "AP SSID and password are required");
        return ESP_OK;
    }

    esp_err_t err = wifi_config_save_ap(s, p);
    if (err != ESP_OK) {
        json_error(req, 400, "AP SSID is required; password must be empty or at least 8 characters");
        return ESP_OK;
    }

    /* Schedule the apply BEFORE reporting success.  Previously, if task
     * creation failed, the handler had already sent a success response even
     * though the new credentials would not become active until a reboot. */
    bool create_task = false;
    portENTER_CRITICAL(&ap_apply_mux);
    if (ap_apply_pending) {
        /* A task is already scheduled or running; make sure it performs one
         * more pass after the latest NVS save. */
        ap_apply_dirty = true;
    } else {
        ap_apply_pending = true;
        create_task = true;
    }
    portEXIT_CRITICAL(&ap_apply_mux);

    if (create_task && xTaskCreate(apply_ap_task, "apply_ap", 2048, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&ap_apply_mux);
        ap_apply_pending = false;
        ap_apply_dirty = false;
        portEXIT_CRITICAL(&ap_apply_mux);
        ESP_LOGE(TAG, "Could not schedule AP configuration update");
        json_error(req, 500, "AP settings were saved but could not be applied");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
        "{\"ok\":true,\"message\":\"AP settings saved; reconnect to the new AP name if it changes\"}");
}

/* One-time bootstrap: usable without auth ONLY until an admin password
 * exists, then requires auth to rotate credentials. This lets a fresh
 * router be claimed by whoever gets there first, while preventing an
 * attacker from re-claiming or resetting an already-configured router
 * over the network. */
static esp_err_t setup_handler(httpd_req_t *req)
{
    bool already_configured = wifi_config_admin_configured();
    if (already_configured && !require_auth(req)) return ESP_OK;

    char u[33], p[65];
    if (!read_form_fields(req, "user", u, sizeof(u), "pass", p, sizeof(p))) {
        json_error(req, 400, "Username and password are required");
        return ESP_OK;
    }

    esp_err_t err = wifi_config_save_admin(u, p);
    if (err != ESP_OK) {
        json_error(req, 400, "Username required; password must be 8-64 characters");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Admin credentials saved\"}");
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;

    if (!wifi_scan_try_begin()) {
        json_error(req, 409, "Scan already running");
        return ESP_OK;
    }
    wifi_scan_config_t scan_cfg = {0};
    scan_cfg.show_hidden = true;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 250;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        wifi_scan_end();
        json_error(req, 500, "Wi-Fi scan failed");
        return ESP_OK;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 32) count = 32;

    wifi_ap_record_t *list = count ? calloc(count, sizeof(*list)) : NULL;
    if (count && !list) {
        wifi_scan_end();
        json_error(req, 500, "Out of memory");
        return ESP_OK;
    }

    if (count) {
        esp_wifi_scan_get_ap_records(&count, list);
    }

    char *out = malloc(4096);
    if (!out) {
        free(list);
        wifi_scan_end();
        json_error(req, 500, "Out of memory");
        return ESP_OK;
    }

    size_t pos = 0;
    size_t written = snprintf(out, 4096, "{\"networks\":[");
    if (written >= 4096) written = 4095;
    pos = written;

    bool first = true;
    for (uint16_t i = 0; i < count && pos < 3900; ++i) {
        if (list[i].ssid[0] == 0) {
            int n = snprintf(out + pos, 4096 - pos,
                             "%s{\"ssid\":\"\",\"rssi\":%d,\"hidden\":true}",
                             first ? "" : ",", list[i].rssi);
            if (n < 0 || (size_t)n >= 4096 - pos) break;
            pos += (size_t)n;
            first = false;
            continue;
        }

        char raw[34];
        size_t rl = 0;
        for (; rl < sizeof(list[i].ssid) && list[i].ssid[rl]; ++rl) raw[rl] = (char)list[i].ssid[rl];
        raw[rl] = '\0';

        char esc[200];
        json_escape(raw, esc, sizeof(esc));

        int n = snprintf(out + pos, 4096 - pos, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"hidden\":false}",
                         first ? "" : ",", esc, list[i].rssi);
        if (n < 0 || (size_t)n >= 4096 - pos) break;
        pos += (size_t)n;
        first = false;
    }

    snprintf(out + pos, 4096 - pos, "]}");
    free(list);
    wifi_scan_end();

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return err;
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        dns_started = false;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(sock);
        dns_started = false;
        vTaskDelete(NULL);
        return;
    }

    while (!ap_connect) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &client_len);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;
        if (buf[4] != 0 || buf[5] != 1) continue;
        if (n > (int)sizeof(buf) - 16) continue;

        /* Captive DNS: resolve every requested name to the router itself. */
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = 0;
        buf[7] = 1;
        int pos = n;
        buf[pos++] = 0xC0;
        buf[pos++] = 0x0C;
        buf[pos++] = 0;
        buf[pos++] = 1;
        buf[pos++] = 0;
        buf[pos++] = 1;
        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = 30;
        buf[pos++] = 0;
        buf[pos++] = 4;
        memcpy(buf + pos, &my_ap_ip, 4);
        pos += 4;
        sendto(sock, buf, pos, 0, (struct sockaddr *)&client, client_len);
    }

    close(sock);
    dns_started = false;
    vTaskDelete(NULL);
}

void captive_portal_start(void)
{
    if (ap_connect || dns_started) return;
    dns_started = true;
    if (xTaskCreate(dns_task, "captive_dns", 2048, NULL, 3, NULL) != pdPASS) {
        dns_started = false;
    }
}

httpd_handle_t start_webserver(uint16_t port)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) return NULL;

    httpd_uri_t u = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler
    };
    httpd_register_uri_handler(server, &u);

    u.uri = "/api/status";
    u.handler = status_handler;
    httpd_register_uri_handler(server, &u);

    u.uri = "/api/scan";
    u.method = HTTP_POST;
    u.handler = scan_handler;
    httpd_register_uri_handler(server, &u);

    u.uri = "/api/connect";
    u.method = HTTP_POST;
    u.handler = connect_handler;
    httpd_register_uri_handler(server, &u);

    u.uri = "/api/ap";
    u.method = HTTP_POST;
    u.handler = ap_handler;
    httpd_register_uri_handler(server, &u);

    u.uri = "/api/setup";
    u.method = HTTP_POST;
    u.handler = setup_handler;
    httpd_register_uri_handler(server, &u);

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "HTTP server started on port %u", (unsigned)port);
    return server;
}
