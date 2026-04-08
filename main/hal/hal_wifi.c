/**
 * WiFi SoftAP + HTTP config server
 *
 * - Starts 10 seconds after boot in a background task
 * - Captive portal DNS redirects all domains to 192.168.4.1
 * - If no device connects within 60 seconds, WiFi shuts down until reboot
 *
 * Endpoints:
 *   GET  /          — HTML config page
 *   GET  /config    — current g_config + odometer as JSON
 *   POST /config    — update g_config (+ optional odometer) from JSON, save to NVS
 *   POST /reset     — restore defaults, save to NVS
 *   GET  *          — captive portal redirect (any other path)
 */

#include "hal_wifi.h"
#include "config.h"
#include "hal_nvs.h"
#include "hal_i2c.h"
#include "hal_speed.h"
#include "driver/gpio.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

#define AP_SSID_DEFAULT "Dr.Agon"
#define AP_PASS_DEFAULT "cykablyat"
#define AP_CHANNEL      1
#define AP_MAX_CONN     2
#define START_DELAY_S   (g_config.wifi_start_delay_s ? g_config.wifi_start_delay_s : 10)
#define IDLE_TIMEOUT_S  (g_config.wifi_idle_timeout_s ? g_config.wifi_idle_timeout_s : 60)
#define ACTIVE_TIMEOUT_S (g_config.wifi_active_timeout_s ? g_config.wifi_active_timeout_s : 600)

static const char *TAG = "wifi";
static volatile int client_count = 0;
static volatile wifi_state_t wifi_state = WIFI_STATE_OFF;
static volatile uint16_t sim_flags = 0;
static httpd_handle_t s_server = NULL;
static volatile int dns_sock = -1;
static TaskHandle_t dns_task_handle = NULL;

extern volatile uint32_t g_odometer_km;

static live_sensors_t s_sensors;

void hal_wifi_update_sensors(const live_sensors_t *s)
{
    s_sensors = *s;
}

/* ======================================================================== */
/* HTML page (embedded)                                                      */
/* ======================================================================== */

static const char config_html[] =
"<!DOCTYPE html>"
"<html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>T4 Dashboard</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,system-ui,sans-serif;background:#1a1a1a;color:#e0e0e0;padding:12px;max-width:480px;margin:0 auto}"
"h1{text-align:center;color:#ff9800;margin:12px 0 16px;font-size:1.3em}"
".g{margin-bottom:8px;border:1px solid #333;border-radius:6px;overflow:hidden}"
".gh{padding:12px 14px;background:#252525;cursor:pointer;display:flex;justify-content:space-between;align-items:center;user-select:none;-webkit-user-select:none}"
".gh:active{background:#303030}"
".gh h2{font-size:.95em;font-weight:600;color:#ccc}"
".gh .arr{color:#666;transition:transform .2s;font-size:.8em}"
".g.open .gh .arr{transform:rotate(180deg)}"
".gc{max-height:0;overflow:hidden;transition:max-height .3s ease}"
".g.open .gc{max-height:2000px}"
".gci{padding:10px 14px;background:#1e1e1e}"
".f{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #2a2a2a;flex-wrap:wrap}"
".f:last-child{border-bottom:none}"
".f label{font-size:.85em;color:#aaa;flex:1;padding-right:8px}"
".fc label{min-width:100%}"
".f label small{display:block;color:#666;font-size:.8em}"
".f input{width:100px;padding:5px 8px;border:1px solid #444;border-radius:4px;background:#2a2a2a;color:#fff;text-align:right;font-size:.85em}"
".cpv{display:inline-block;width:24px;height:24px;border-radius:4px;border:1px solid #555;flex-shrink:0}"
".f .rgb{width:42px;text-align:center}"
".f input:focus{border-color:#ff9800;outline:none}"
".f .ro{color:#666;background:#222}"
".f .hint{font-size:.7em;color:#555;margin-left:4px;white-space:nowrap}"
".rst{background:none;border:none;color:#555;cursor:pointer;font-size:1.1em;padding:0 3px}"
".rst:active{color:#ff9800}"
".f .hex{width:72px;font-family:monospace;font-size:.8em;text-align:center;text-transform:uppercase}"
".hp{color:#666;font-family:monospace;font-size:.8em}"
".pal{padding:8px 0 4px;border-bottom:1px solid #333;margin-bottom:4px}"
".pal h3{font-size:.75em;color:#666;margin-bottom:6px}"
".sw{display:inline-flex;align-items:center;margin:2px 4px 2px 0;padding:3px 6px;border-radius:4px;cursor:pointer;background:#252525;border:1px solid #333;font-size:.7em;color:#999}"
".sw:active{border-color:#ff9800}"
".sw i{display:inline-block;width:16px;height:16px;border-radius:3px;margin-right:5px;border:1px solid #555;flex-shrink:0}"
".tg{position:relative;width:44px;height:24px;border-radius:12px;background:#333;cursor:pointer;border:none;padding:0;flex-shrink:0;transition:background .2s}"
".tg.on{background:#ff9800}"
".tg::after{content:'';position:absolute;top:2px;left:2px;width:20px;height:20px;border-radius:50%;background:#aaa;transition:transform .2s,background .2s}"
".tg.on::after{transform:translateX(20px);background:#fff}"
".btns{margin:16px 0;display:flex;gap:8px}"
".btns button{flex:1;padding:12px;border:none;border-radius:6px;font-size:.9em;font-weight:600;cursor:pointer}"
"#saveBtn{background:#ff9800;color:#000}"
"#saveBtn:active{background:#e68a00}"
"#resetBtn{background:#333;color:#aaa;border:1px solid #444}"
"#resetBtn:active{background:#444}"
".btns2{margin:0 0 16px;display:flex;gap:8px}"
".btns2 button,.btns2 label{flex:1;padding:10px;border:none;border-radius:6px;font-size:.85em;font-weight:600;cursor:pointer;text-align:center}"
"#dlBtn{background:#2a4a2a;color:#4caf50}"
".rlbl{background:#2a2a4a;color:#6688cc;display:block}"
".rlbl input{display:none}"
".msg{text-align:center;padding:8px;margin:8px 0;border-radius:4px;font-size:.85em;display:none}"
".msg.ok{display:block;background:#1b3a1b;color:#4caf50}"
".msg.err{display:block;background:#3a1b1b;color:#f44336}"
".lv{float:right;font-size:.7em;color:#6a6;font-weight:400;letter-spacing:.5px}"
"</style></head><body>"
"<h1>Dr.Agon Dashboard</h1>"
"<div id='msg' class='msg'></div>"
"<div id='groups'></div>"
"<div class='btns'>"
"<button id='saveBtn' onclick='save()'>Save</button>"
"<button id='resetBtn' onclick='reset()'>Reset Defaults</button>"
"</div>"
"<div class='btns2'>"
"<button id='dlBtn' onclick='backup()'>Download Backup</button>"
"<label class='rlbl'>Restore Backup<input type='file' accept='.json' onchange='restore(this)'></label>"
"</div>"
"<script>"
"var F=["
"['Odometer',["
"['odometer_km','Odometer (km)',0,'i']"
"]],"
"['Voltage',["
"['voltage_multiplier','Divider ratio',5.02,'f'],"
"['voltage_ema_alpha','EMA alpha',0.03,'f'],"
"['voltage_settle_count','Settle samples',8,'i'],"
"['voltage_display_hyst','Display hysteresis (V)',0.07,'f']"
"]],"
"['Voltage Thresholds',["
"['color_red_enter_v','Red enter (V)',11.3,'f'],"
"['color_red_exit_v','Red exit (V)',11.5,'f'],"
"['color_yellow_enter_v','Yellow enter (V)',15.2,'f'],"
"['color_yellow_exit_v','Yellow exit (V)',14.8,'f']"
"]],"
"['Voltage Warnings',["
"['warn_low_enter_v','Low trigger (V)',11.0,'f'],"
"['warn_low_exit_v','Low clear (V)',11.4,'f'],"
"['warn_low_enter_ms','Low delay (ms)',8000,'i'],"
"['warn_low_exit_ms','Low clear delay (ms)',3000,'i'],"
"['warn_high_enter_v','High trigger (V)',15.2,'f'],"
"['warn_high_exit_v','High clear (V)',14.8,'f'],"
"['warn_high_enter_ms','High delay (ms)',5000,'i'],"
"['warn_high_exit_ms','High clear delay (ms)',3000,'i']"
"]],"
"['Fuel Calibration',["
"['fuel_empty_v','Empty voltage',1.15,'f'],"
"['fuel_full_v','Full voltage',0.13,'f'],"
"['fuel_learned_empty_v','Learned empty (V)',-1,'r'],"
"['fuel_learned_full_v','Learned full (V)',-1,'r'],"
"['fuel_emergency_pct','Emergency diff (%)',25,'i'],"
"['fuel_warn_on_pct','Low fuel warn ON (%)',10,'i'],"
"['fuel_warn_off_pct','Low fuel warn OFF (%)',15,'i']"
"]],"
"['Fuel Sampling',["
"['fuel_buf_size','Buffer size',20,'i','2 - 30'],"
"['fuel_sample_moving_ms','Moving interval (ms)',2000,'i'],"
"['fuel_sample_stopped_ms','Stopped interval (ms)',500,'i'],"
"['fuel_stop_delay_ms','Stop detect delay (ms)',30000,'i'],"
"['fuel_emergency_time_ms','Emergency time (ms)',30000,'i'],"
"['fuel_hyst_threshold','Hysteresis (%)',1,'i'],"
"['fuel_hyst_cycles','Confirm cycles',3,'i']"
"]],"
"['Temperature Sensor',["
"['temp_beta','NTC beta',3435,'f'],"
"['temp_r_nominal','R nominal (ohm)',2500,'f'],"
"['temp_min_c','Gauge 0% (C)',40,'f'],"
"['temp_max_c','Gauge 100% (C)',120,'f'],"
"['temp_overheat_pct','Overheat warning (%)',80,'i'],"
"['temp_overheat_delay_ms','Overheat delay (ms)',2000,'i']"
"]],"
"['Temperature Display',["
"['temp_cold_on_pct','Enter COLD (%)',30,'i'],"
"['temp_cold_off_pct','Exit COLD (%)',33,'i'],"
"['temp_hot_on_pct','Enter HOT (%)',70,'i'],"
"['temp_hot_off_pct','Exit HOT (%)',67,'i'],"
"['temp_display_hyst','Display hysteresis (%)',2,'i']"
"]],"
"['Brightness',["
"['light_ema_alpha','Light EMA alpha',0.15,'f'],"
"['brightness_min','Min PWM',15,'i'],"
"['brightness_max','Max PWM',255,'i'],"
"['brightness_dark_lux','Dark threshold (lux)',0.2,'f'],"
"['brightness_bright_lux','Bright threshold (lux)',0.7,'f'],"
"['brightness_dark_pwm','Dark PWM',70,'i'],"
"['brightness_bright_pwm','Bright PWM',150,'i']"
"]],"
"['Warning Blink',["
"['blink_on_ms','On phase (ms)',1000,'i'],"
"['blink_off_ms','Off phase (ms)',500,'i']"
"]],"
"['Speed',["
"['pulses_per_km','Pulses per km',539,'i'],"
"['speed_max_kmh','Arc max (km/h)',180,'i'],"
"['speed_text_interval_ms','Label update (ms)',500,'i'],"
"['speed_slow_threshold','Slow threshold (km/h)',10,'i'],"
"['speed_slow_interval_ms','Slow update (ms)',1000,'i'],"
"['speed_text_hyst','Label hysteresis (km/h)',2,'i'],"
"['speed_arc_smooth','Arc smoothing (0-1)',0.4,'f'],"
"['speed_filter_size','Median filter depth (1-9)',5,'i'],"
"['speed_max_accel','Max accel (km/h/s, 0=off)',30,'i'],"
"['speed_confirm_count','Confirm samples to override',3,'i']"
"]],"
"['Tachometer',["
"['tach_enabled','Enabled',0,'i','0 = off, 1 = on'],"
"['tach_pulses_per_rev','Pulses per revolution',1,'i'],"
"['tach_max_rpm','Max RPM',6000,'i']"
"]],"
"['Colors',["
"['color_normal','Normal icons',0xFFFFFF,'c'],"
"['color_red','Warning / danger',0xFF0814,'c'],"
"['color_green','Blinkers / tach',0x00FF00,'c'],"
"['color_fog','Fog light',0x00FF00,'c'],"
"['color_yellow','Low fuel / caution',0xFFCC00,'c'],"
"['color_cold','Temperature COLD',0x00BFFF,'c'],"
"['color_temp_ok','Temperature OK',0xFFFFFF,'c'],"
"['color_high_beam','High beam',0x0066FF,'c'],"
"['color_glow_plug','Glow plug',0xFFAA00,'c'],"
"['color_dim','Inactive icons',0x080808,'c'],"
"['color_disabled','Disabled functions',0x080808,'c'],"
"['color_wifi_active','WiFi active (no client)',0x0066FF,'c'],"
"['color_wifi_connected','WiFi connected',0x00FF00,'c'],"
"['color_wifi_off','WiFi off',0x080808,'c'],"
"['color_bar_bg','Fuel/temp bar background',0x080808,'c'],"
"['color_speedo_bg','Speedometer grid tint',0x1E1E1E,'c'],"
"['color_speedo_ind','Speedometer arc',0xFFFFFF,'c']"
"]],"
"['Pin Mapping',["
"['pin_ignition','Ignition',0,'i','255 = off (stays ON)'],"
"['pin_blinker_l','Left blinker',6,'i','255 = off'],"
"['pin_blinker_r','Right blinker',5,'i','255 = off'],"
"['pin_high_beam','High beam',3,'i','255 = off'],"
"['pin_glow_plug','Glow plug',4,'i','255 = off'],"
"['pin_brake','Brake',255,'i','255 = off'],"
"['pin_oil','Oil pressure',255,'i','255 = off'],"
"['pin_fog_light','Fog light',255,'i','255 = off']"
"]],"
"['ADC Channels',["
"['adc_voltage_ch','Voltage',2,'i','0-3, 255 = off'],"
"['adc_fuel_ch','Fuel',0,'i','0-3, 255 = off'],"
"['adc_temp_ch','Temperature',1,'i','0-3, 255 = off']"
"]],"
"['Power',["
"['shutdown_delay_s','Turn-off delay (sec)',5,'i']"
"]],"
"['WiFi',["
"['wifi_ssid','Network name','Dr.Agon','s'],"
"['wifi_pass','Password','cykablyat','s','min 8 chars'],"
"['wifi_start_delay_s','Start delay (sec)',10,'i'],"
"['wifi_idle_timeout_s','No-connect timeout (sec)',60,'i','shutdown if nobody connects'],"
"['wifi_active_timeout_s','Post-session timeout (sec)',600,'i','shutdown after last disconnect']"
"]],"
"['Simulation',["
"['sim_low_battery','Low Battery',0,'t'],"
"['sim_overvoltage','Overvoltage',1,'t'],"
"['sim_overheating','Overheating',2,'t'],"
"['sim_brake','Brake',3,'t'],"
"['sim_oil','Oil Pressure',4,'t'],"
"['sim_fuel','Low Fuel',5,'t'],"
"['sim_glow','Glow Plug',6,'t'],"
"['sim_hbeam','High Beam',7,'t'],"
"['sim_bl','Left Blinker',8,'t'],"
"['sim_br','Right Blinker',9,'t']"
"]]"
"];"
"var D={};"
"function iToH(n){return '#'+('000000'+(n>>>0).toString(16)).slice(-6)}"
"function hToRgb(h){var n=parseInt(h.slice(1),16);return[(n>>16)&255,(n>>8)&255,n&255]}"
"function setRgb(id,hex){var c=hToRgb(hex),r=document.getElementById(id+'_r'),g=document.getElementById(id+'_g'),b=document.getElementById(id+'_b'),p=document.getElementById(id+'_p');if(r)r.value=c[0];if(g)g.value=c[1];if(b)b.value=c[2];if(p)p.style.background=hex;}"
"function rsync(id){var r=parseInt(document.getElementById(id+'_r').value)||0,g=parseInt(document.getElementById(id+'_g').value)||0,b=parseInt(document.getElementById(id+'_b').value)||0;r=Math.max(0,Math.min(255,r));g=Math.max(0,Math.min(255,g));b=Math.max(0,Math.min(255,b));var hex=iToH((r<<16)|(g<<8)|b);var hx=document.getElementById(id+'_h');if(hx)hx.value=hex.slice(1).toUpperCase();var p=document.getElementById(id+'_p');if(p)p.style.background=hex;buildPal();}"
"function hsync(id){var hx=document.getElementById(id+'_h');if(!hx)return;var v='#'+hx.value.trim();if(/^#[0-9a-fA-F]{6}$/.test(v)){setRgb(id,v);buildPal();}}"
"function applySw(hex){var v=hex.replace('#','');var t=document.createElement('textarea');t.value=v;t.style.position='fixed';t.style.opacity='0';document.body.appendChild(t);t.select();document.execCommand('copy');document.body.removeChild(t);msg('Copied '+v,'ok');}"
"function buildPal(){var pe=document.getElementById('pal');if(!pe)return;var m={};F.forEach(function(g){g[1].forEach(function(f){if(f[3]!=='c')return;var hx=document.getElementById(f[0]+'_h');var hex=hx?'#'+hx.value.toUpperCase():iToH(f[2]).toUpperCase();if(!m[hex])m[hex]=[];m[hex].push(f[1]);});});var ph='<h3>Project colors (tap to copy hex)</h3>';Object.keys(m).forEach(function(k){ph+='<div class=\"sw\" onclick=\"applySw(\\''+k+'\\')\"><i style=\"background:'+k+'\"></i>'+m[k].join(', ')+'</div>';});pe.innerHTML=ph;}"
"function simTog(id,bit){var el=document.getElementById(id);if(!el)return;var on=!el.classList.contains('on');el.classList.toggle('on');fetch('/sim',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({bit:bit,on:on})});}"
"function build(){"
"var h='';F.forEach(function(g,gi){"
"h+='<div class=\"g\" id=\"g'+gi+'\">';"
"h+='<div class=\"gh\" onclick=\"tog('+gi+')\">';"
"h+='<h2>'+g[0]+'<span class=\"lv\" id=\"lv'+gi+'\"></span></h2><span class=\"arr\">\\u25BC</span></div>';"
"h+='<div class=\"gc\"><div class=\"gci\">';"
"if(g[0]==='Colors')h+='<div id=\"pal\" class=\"pal\"></div>';"
"g[1].forEach(function(f){"
"var ro=f[3]==='r',ic=f[3]==='c';"
"h+='<div class=\"f'+(ic?' fc':'')+'\">';"
"h+='<label>'+f[1];"
"if(f[4])h+='<small>'+f[4]+'</small>';"
"h+='</label>';"
"if(ic){"
"var dh=iToH(f[2]),dc=hToRgb(dh);"
"h+='<span id=\"'+f[0]+'_p\" class=\"cpv\" style=\"background:'+dh+'\"></span>';"
"h+='<input id=\"'+f[0]+'_r\" type=\"number\" class=\"rgb\" style=\"border-bottom:3px solid #f44\" min=\"0\" max=\"255\" value=\"'+dc[0]+'\" oninput=\"rsync(\\''+f[0]+'\\')\" onfocus=\"this.select()\">';"
"h+='<input id=\"'+f[0]+'_g\" type=\"number\" class=\"rgb\" style=\"border-bottom:3px solid #4c4\" min=\"0\" max=\"255\" value=\"'+dc[1]+'\" oninput=\"rsync(\\''+f[0]+'\\')\" onfocus=\"this.select()\">';"
"h+='<input id=\"'+f[0]+'_b\" type=\"number\" class=\"rgb\" style=\"border-bottom:3px solid #48f\" min=\"0\" max=\"255\" value=\"'+dc[2]+'\" oninput=\"rsync(\\''+f[0]+'\\')\" onfocus=\"this.select()\">';"
"h+='<span class=\"hp\">#</span><input id=\"'+f[0]+'_h\" type=\"text\" class=\"hex\" value=\"'+dh.slice(1).toUpperCase()+'\" oninput=\"hsync(\\''+f[0]+'\\')\" onfocus=\"this.select()\">';"
"D[f[0]]=dh;"
"h+='<button class=\"rst\" onclick=\"rst(\\''+f[0]+'\\')\">&#8634;</button>';"
"}else if(f[3]==='t'){"
"h+='<button class=\"tg\" id=\"'+f[0]+'\" onclick=\"simTog(\\''+f[0]+'\\','+f[2]+')\"></button>';"
"}else{"
"var tp=f[3]==='s'?'text':'number';"
"h+='<input id=\"'+f[0]+'\" type=\"'+tp+'\"';"
"if(tp==='number')h+=' step=\"any\"';"
"if(ro)h+=' class=\"ro\" readonly';"
"h+=' value=\"\">';"
"if(!ro){D[f[0]]=f[2];"
"h+='<button class=\"rst\" onclick=\"rst(\\''+f[0]+'\\')\">&#8634;</button>';}"
"h+='<span class=\"hint\">'+f[2]+'</span>';}"
"h+='</div>';"
"});"
"h+='</div></div></div>';});"
"document.getElementById('groups').innerHTML=h;}"
"function tog(i){"
"var els=document.querySelectorAll('.g');"
"els.forEach(function(e,j){"
"if(j===i)e.classList.toggle('open');"
"else e.classList.remove('open');});}"
"function rst(id){var hx=document.getElementById(id+'_h');if(hx){hx.value=D[id].slice(1).toUpperCase();setRgb(id,D[id]);buildPal();}else{var e=document.getElementById(id);if(e)e.value=D[id];}}"
"function load(){"
"fetch('/config').then(function(r){return r.json()}).then(function(d){"
"F.forEach(function(g){g[1].forEach(function(f){"
"if(f[3]==='t')return;"
"if(f[3]==='c'){if(d[f[0]]!==undefined){var hex=iToH(d[f[0]]);var hx=document.getElementById(f[0]+'_h');if(hx)hx.value=hex.slice(1).toUpperCase();setRgb(f[0],hex);}}"
"else{var el=document.getElementById(f[0]);if(el&&d[f[0]]!==undefined)el.value=d[f[0]];}"
"});});buildPal();"
"}).catch(function(e){msg('Failed to load config','err')});}"
"function save(){"
"var o={};F.forEach(function(g){g[1].forEach(function(f){"
"if(f[3]==='r'||f[3]==='t')return;"
"if(f[3]==='c'){var hx=document.getElementById(f[0]+'_h');if(hx)o[f[0]]=parseInt(hx.value,16);return;}"
"var el=document.getElementById(f[0]);"
"if(el){"
"if(f[3]==='i')o[f[0]]=parseInt(el.value);"
"else if(f[3]==='s')o[f[0]]=el.value;"
"else o[f[0]]=parseFloat(el.value);}"
"})});"
"fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify(o)}).then(function(r){"
"if(r.ok)msg('Saved!','ok');else msg('Save failed','err');"
"}).catch(function(e){msg('Network error','err')});}"
"function reset(){"
"if(!confirm('Reset all settings to factory defaults?'))return;"
"fetch('/reset',{method:'POST'}).then(function(r){"
"if(r.ok){msg('Defaults restored','ok');load();}else msg('Reset failed','err');"
"}).catch(function(e){msg('Network error','err')});}"
"function backup(){window.open('/backup','_blank');}"
"function restore(inp){"
"if(!inp.files[0])return;"
"var r=new FileReader();r.onload=function(){"
"if(!confirm('Restore from backup? Current settings will be overwritten.'))return;"
"fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:r.result}).then(function(res){"
"if(res.ok){msg('Restored!','ok');load();}else msg('Restore failed','err');"
"}).catch(function(e){msg('Network error','err')});"
"};r.readAsText(inp.files[0]);inp.value='';}"
"function msg(t,c){"
"var m=document.getElementById('msg');m.textContent=t;m.className='msg '+c;"
"setTimeout(function(){m.className='msg'},3000);}"
"var LG={'Voltage':function(s){return s.v!==undefined?s.v.toFixed(1)+'V':''},"
"'Fuel Calibration':function(s){return s.fp!==undefined?s.fp+'%  '+s.fv.toFixed(2)+'V':''},"
"'Temperature Sensor':function(s){return s.tc!==undefined?s.tc.toFixed(0)+'\\u00B0C  '+s.tp+'%':''},"
"'Speed':function(s){return s.spd!==undefined?s.spd+' km/h':''},"
"'Tachometer':function(s){return s.rpm!==undefined?s.rpm+' RPM':''},"
"'Brightness':function(s){return s.lux!==undefined?s.lux.toFixed(0)+' lx \\u2192 '+s.br:''}};"
"function pollS(){fetch('/sensors').then(function(r){return r.json()}).then(function(s){"
"F.forEach(function(g,i){var fn=LG[g[0]];if(fn){var el=document.getElementById('lv'+i);"
"if(el)el.textContent=fn(s)}})}).catch(function(){});"
"setTimeout(pollS,2000)};"
"build();load();pollS();"
"</script></body></html>";

/* ======================================================================== */
/* JSON serialization                                                        */
/* ======================================================================== */

static void add_f2(cJSON *j, const char *name, float v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", (double)v);
    char *p = buf + strlen(buf) - 1;
    while (p > buf && *p == '0') p--;
    if (*p == '.') p--;
    p[1] = '\0';
    cJSON_AddItemToObject(j, name, cJSON_CreateRaw(buf));
}

#define ADD_F(j, name) add_f2(j, #name, g_config.name)
#define ADD_I(j, name) cJSON_AddNumberToObject(j, #name, g_config.name)

static char *config_to_json(void)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;

    cJSON_AddNumberToObject(j, "odometer_km", g_odometer_km);

    ADD_F(j, voltage_multiplier);
    ADD_F(j, voltage_ema_alpha);
    ADD_I(j, voltage_settle_count);
    ADD_F(j, voltage_display_hyst);

    ADD_F(j, color_red_enter_v);
    ADD_F(j, color_red_exit_v);
    ADD_F(j, color_yellow_enter_v);
    ADD_F(j, color_yellow_exit_v);

    ADD_F(j, warn_low_enter_v);
    ADD_F(j, warn_low_exit_v);
    ADD_I(j, warn_low_enter_ms);
    ADD_I(j, warn_low_exit_ms);
    ADD_F(j, warn_high_enter_v);
    ADD_F(j, warn_high_exit_v);
    ADD_I(j, warn_high_enter_ms);
    ADD_I(j, warn_high_exit_ms);

    ADD_F(j, fuel_empty_v);
    ADD_F(j, fuel_full_v);
    ADD_F(j, fuel_learned_empty_v);
    ADD_F(j, fuel_learned_full_v);
    ADD_I(j, fuel_emergency_pct);
    ADD_I(j, fuel_warn_on_pct);
    ADD_I(j, fuel_warn_off_pct);

    ADD_I(j, fuel_buf_size);
    ADD_I(j, fuel_sample_moving_ms);
    ADD_I(j, fuel_sample_stopped_ms);
    ADD_I(j, fuel_stop_delay_ms);
    ADD_I(j, fuel_emergency_time_ms);
    ADD_I(j, fuel_hyst_threshold);
    ADD_I(j, fuel_hyst_cycles);

    ADD_F(j, temp_beta);
    ADD_F(j, temp_r_nominal);
    ADD_F(j, temp_min_c);
    ADD_F(j, temp_max_c);
    ADD_I(j, temp_overheat_pct);
    ADD_I(j, temp_overheat_delay_ms);

    ADD_I(j, temp_cold_on_pct);
    ADD_I(j, temp_cold_off_pct);
    ADD_I(j, temp_hot_on_pct);
    ADD_I(j, temp_hot_off_pct);
    ADD_I(j, temp_display_hyst);

    ADD_F(j, light_ema_alpha);
    ADD_I(j, brightness_min);
    ADD_I(j, brightness_max);
    ADD_F(j, brightness_dark_lux);
    ADD_F(j, brightness_bright_lux);
    ADD_I(j, brightness_dark_pwm);
    ADD_I(j, brightness_bright_pwm);

    ADD_I(j, blink_on_ms);
    ADD_I(j, blink_off_ms);

    ADD_I(j, pulses_per_km);
    ADD_I(j, speed_max_kmh);
    ADD_I(j, speed_text_interval_ms);
    ADD_I(j, speed_slow_threshold);
    ADD_I(j, speed_slow_interval_ms);
    ADD_I(j, speed_text_hyst);
    ADD_F(j, speed_arc_smooth);
    ADD_I(j, speed_filter_size);
    ADD_I(j, speed_max_accel);
    ADD_I(j, speed_confirm_count);

    ADD_I(j, tach_enabled);
    ADD_I(j, tach_pulses_per_rev);
    ADD_I(j, tach_max_rpm);

    ADD_I(j, color_normal);
    ADD_I(j, color_red);
    ADD_I(j, color_green);
    ADD_I(j, color_yellow);
    ADD_I(j, color_cold);
    ADD_I(j, color_high_beam);
    ADD_I(j, color_glow_plug);
    ADD_I(j, color_dim);
    ADD_I(j, color_disabled);
    ADD_I(j, color_temp_ok);
    ADD_I(j, color_fog);
    ADD_I(j, color_wifi_active);
    ADD_I(j, color_wifi_connected);
    ADD_I(j, color_wifi_off);
    ADD_I(j, color_bar_bg);
    ADD_I(j, color_speedo_bg);
    ADD_I(j, color_speedo_ind);

    ADD_I(j, pin_ignition);
    ADD_I(j, pin_blinker_l);
    ADD_I(j, pin_blinker_r);
    ADD_I(j, pin_high_beam);
    ADD_I(j, pin_glow_plug);
    ADD_I(j, pin_brake);
    ADD_I(j, pin_oil);
    ADD_I(j, pin_fog_light);

    ADD_I(j, adc_voltage_ch);
    ADD_I(j, adc_fuel_ch);
    ADD_I(j, adc_temp_ch);

    ADD_I(j, shutdown_delay_s);

    cJSON_AddStringToObject(j, "wifi_ssid", g_config.wifi_ssid);
    cJSON_AddStringToObject(j, "wifi_pass", g_config.wifi_pass);
    ADD_I(j, wifi_start_delay_s);
    ADD_I(j, wifi_idle_timeout_s);
    ADD_I(j, wifi_active_timeout_s);

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return str;
}

#define JSON_SET_FLOAT(name) do { \
    cJSON *item = cJSON_GetObjectItem(j, #name); \
    if (item && cJSON_IsNumber(item)) g_config.name = (float)item->valuedouble; \
} while(0)

#define JSON_SET_U8(name) do { \
    cJSON *item = cJSON_GetObjectItem(j, #name); \
    if (item && cJSON_IsNumber(item)) g_config.name = (uint8_t)item->valuedouble; \
} while(0)

#define JSON_SET_U16(name) do { \
    cJSON *item = cJSON_GetObjectItem(j, #name); \
    if (item && cJSON_IsNumber(item)) g_config.name = (uint16_t)item->valuedouble; \
} while(0)

#define JSON_SET_U32(name) do { \
    cJSON *item = cJSON_GetObjectItem(j, #name); \
    if (item && cJSON_IsNumber(item)) g_config.name = (uint32_t)item->valuedouble; \
} while(0)

static bool config_from_json(const char *buf)
{
    cJSON *j = cJSON_Parse(buf);
    if (!j) return false;

    // Odometer (optional — present in backups)
    cJSON *odo = cJSON_GetObjectItem(j, "odometer_km");
    if (odo && cJSON_IsNumber(odo)) {
        uint32_t new_km = (uint32_t)odo->valuedouble;
        g_odometer_km = new_km;
        hal_nvs_save_odometer(new_km);
    }

    JSON_SET_FLOAT(voltage_multiplier);
    JSON_SET_FLOAT(voltage_ema_alpha);
    JSON_SET_U8(voltage_settle_count);
    JSON_SET_FLOAT(voltage_display_hyst);

    JSON_SET_FLOAT(color_red_enter_v);
    JSON_SET_FLOAT(color_red_exit_v);
    JSON_SET_FLOAT(color_yellow_enter_v);
    JSON_SET_FLOAT(color_yellow_exit_v);

    JSON_SET_FLOAT(warn_low_enter_v);
    JSON_SET_FLOAT(warn_low_exit_v);
    JSON_SET_U16(warn_low_enter_ms);
    JSON_SET_U16(warn_low_exit_ms);
    JSON_SET_FLOAT(warn_high_enter_v);
    JSON_SET_FLOAT(warn_high_exit_v);
    JSON_SET_U16(warn_high_enter_ms);
    JSON_SET_U16(warn_high_exit_ms);

    JSON_SET_FLOAT(fuel_empty_v);
    JSON_SET_FLOAT(fuel_full_v);
    JSON_SET_U8(fuel_emergency_pct);
    JSON_SET_U8(fuel_warn_on_pct);
    JSON_SET_U8(fuel_warn_off_pct);

    JSON_SET_U8(fuel_buf_size);
    JSON_SET_U16(fuel_sample_moving_ms);
    JSON_SET_U16(fuel_sample_stopped_ms);
    JSON_SET_U16(fuel_stop_delay_ms);
    JSON_SET_U16(fuel_emergency_time_ms);
    JSON_SET_U8(fuel_hyst_threshold);
    JSON_SET_U8(fuel_hyst_cycles);

    JSON_SET_FLOAT(temp_beta);
    JSON_SET_FLOAT(temp_r_nominal);
    JSON_SET_FLOAT(temp_min_c);
    JSON_SET_FLOAT(temp_max_c);
    JSON_SET_U8(temp_overheat_pct);
    JSON_SET_U16(temp_overheat_delay_ms);

    JSON_SET_U8(temp_cold_on_pct);
    JSON_SET_U8(temp_cold_off_pct);
    JSON_SET_U8(temp_hot_on_pct);
    JSON_SET_U8(temp_hot_off_pct);
    JSON_SET_U8(temp_display_hyst);

    JSON_SET_FLOAT(light_ema_alpha);
    JSON_SET_U8(brightness_min);
    JSON_SET_U8(brightness_max);
    JSON_SET_FLOAT(brightness_dark_lux);
    JSON_SET_FLOAT(brightness_bright_lux);
    JSON_SET_U8(brightness_dark_pwm);
    JSON_SET_U8(brightness_bright_pwm);

    JSON_SET_U16(blink_on_ms);
    JSON_SET_U16(blink_off_ms);

    JSON_SET_U16(pulses_per_km);
    JSON_SET_U16(speed_max_kmh);
    JSON_SET_U16(speed_text_interval_ms);
    JSON_SET_U16(speed_slow_threshold);
    JSON_SET_U16(speed_slow_interval_ms);
    JSON_SET_U8(speed_text_hyst);
    JSON_SET_FLOAT(speed_arc_smooth);
    JSON_SET_U8(speed_filter_size);
    JSON_SET_U8(speed_max_accel);
    JSON_SET_U8(speed_confirm_count);

    JSON_SET_U8(tach_enabled);
    JSON_SET_U8(tach_pulses_per_rev);
    JSON_SET_U16(tach_max_rpm);

    JSON_SET_U32(color_normal);
    JSON_SET_U32(color_red);
    JSON_SET_U32(color_green);
    JSON_SET_U32(color_yellow);
    JSON_SET_U32(color_cold);
    JSON_SET_U32(color_high_beam);
    JSON_SET_U32(color_glow_plug);
    JSON_SET_U32(color_dim);
    JSON_SET_U32(color_disabled);
    JSON_SET_U32(color_temp_ok);
    JSON_SET_U32(color_fog);
    JSON_SET_U32(color_wifi_active);
    JSON_SET_U32(color_wifi_connected);
    JSON_SET_U32(color_wifi_off);
    JSON_SET_U32(color_bar_bg);
    JSON_SET_U32(color_speedo_bg);
    JSON_SET_U32(color_speedo_ind);

    JSON_SET_U8(pin_ignition);
    JSON_SET_U8(pin_blinker_l);
    JSON_SET_U8(pin_blinker_r);
    JSON_SET_U8(pin_high_beam);
    JSON_SET_U8(pin_glow_plug);
    JSON_SET_U8(pin_brake);
    JSON_SET_U8(pin_oil);
    JSON_SET_U8(pin_fog_light);

    JSON_SET_U8(adc_voltage_ch);
    JSON_SET_U8(adc_fuel_ch);
    JSON_SET_U8(adc_temp_ch);

    JSON_SET_U8(shutdown_delay_s);

    {
        cJSON *s = cJSON_GetObjectItem(j, "wifi_ssid");
        if (s && cJSON_IsString(s)) {
            strncpy(g_config.wifi_ssid, s->valuestring, sizeof(g_config.wifi_ssid) - 1);
            g_config.wifi_ssid[sizeof(g_config.wifi_ssid) - 1] = '\0';
        }
        s = cJSON_GetObjectItem(j, "wifi_pass");
        if (s && cJSON_IsString(s) && strlen(s->valuestring) >= 8) {
            strncpy(g_config.wifi_pass, s->valuestring, sizeof(g_config.wifi_pass) - 1);
            g_config.wifi_pass[sizeof(g_config.wifi_pass) - 1] = '\0';
        }
    }
    JSON_SET_U8(wifi_start_delay_s);
    JSON_SET_U8(wifi_idle_timeout_s);
    JSON_SET_U16(wifi_active_timeout_s);

    cJSON_Delete(j);
    return true;
}

/* ======================================================================== */
/* HTTP handlers                                                             */
/* ======================================================================== */

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, config_html, sizeof(config_html) - 1);
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    char *json = config_to_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t handle_backup(httpd_req_t *req)
{
    char *json = config_to_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"t4_backup.json\"");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length");
        return ESP_FAIL;
    }

    char *buf = malloc(total + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv error");
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total] = '\0';

    if (!config_from_json(buf)) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Parse error");
        return ESP_FAIL;
    }
    free(buf);

    hal_nvs_save_config();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t handle_post_reset(httpd_req_t *req)
{
    config_set_defaults(&g_config);
    hal_nvs_save_config();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t handle_sim(httpd_req_t *req)
{
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }

    cJSON *bit = cJSON_GetObjectItem(j, "bit");
    cJSON *on  = cJSON_GetObjectItem(j, "on");
    if (bit && on && cJSON_IsNumber(bit) && cJSON_IsBool(on)) {
        uint16_t mask = (uint16_t)(1 << (int)bit->valuedouble);
        if (cJSON_IsTrue(on))
            sim_flags |= mask;
        else
            sim_flags &= ~mask;
    }

    cJSON_Delete(j);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

uint16_t hal_wifi_get_sim_flags(void)
{
    return sim_flags;
}

static esp_err_t handle_sensors(httpd_req_t *req)
{
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"spd\":%u,\"rpm\":%u,\"v\":%.2f,\"fv\":%.3f,\"fp\":%u,"
        "\"tc\":%.1f,\"tp\":%u,\"lux\":%.1f,\"br\":%u,"
        "\"mcp\":%u,\"mcp_ok\":%d,\"ign\":%d}",
        s_sensors.speed_kmh, s_sensors.rpm,
        s_sensors.voltage, s_sensors.fuel_v, s_sensors.fuel_pct,
        s_sensors.temp_c, s_sensors.temp_pct,
        s_sensors.lux, s_sensors.brightness,
        s_sensors.mcp_inputs, s_sensors.mcp_ok, s_sensors.ignition);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t handle_diag(httpd_req_t *req)
{
    char buf[128];
    int len = snprintf(buf, sizeof(buf),
        "{\"mcp\":%u,\"mcp_ok\":%d,\"gpio43\":%d,\"spd\":%u,\"dist\":%lu}",
        hal_i2c_read_inputs(), hal_i2c_mcp_ok(),
        gpio_get_level(43),
        hal_speed_get_kmh(),
        (unsigned long)hal_speed_get_distance_m());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t handle_captive(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* ======================================================================== */
/* Captive portal DNS server                                                 */
/* ======================================================================== */

static void dns_task_fn(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { dns_task_handle = NULL; vTaskDelete(NULL); return; }
    dns_sock = sock;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[512];
    while (dns_sock >= 0) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &clen);
        if (len < 12) continue;

        uint8_t resp[512];
        if (len > (int)(sizeof(resp) - 16)) continue;
        memcpy(resp, buf, len);

        resp[2] = 0x81; resp[3] = 0x80;
        resp[6] = 0x00; resp[7] = 0x01;

        int off = len;
        resp[off++] = 0xC0; resp[off++] = 0x0C;
        resp[off++] = 0x00; resp[off++] = 0x01;
        resp[off++] = 0x00; resp[off++] = 0x01;
        resp[off++] = 0x00; resp[off++] = 0x00;
        resp[off++] = 0x00; resp[off++] = 0x3C;
        resp[off++] = 0x00; resp[off++] = 0x04;
        resp[off++] = 192;  resp[off++] = 168;
        resp[off++] = 4;    resp[off++] = 1;

        sendto(sock, resp, off, 0, (struct sockaddr *)&client, clen);
    }

    dns_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ======================================================================== */
/* WiFi event handler                                                        */
/* ======================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_STACONNECTED) {
            client_count++;
            wifi_state = WIFI_STATE_CONNECTED;
            ESP_LOGI(TAG, "Client connected (%d total)", client_count);
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            if (client_count > 0) client_count--;
            wifi_state = client_count > 0 ? WIFI_STATE_CONNECTED : WIFI_STATE_ACTIVE;
            ESP_LOGI(TAG, "Client disconnected (%d remaining)", client_count);
        }
    }
}

/* ======================================================================== */
/* Server + WiFi init (runs in background task)                              */
/* ======================================================================== */

static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = handle_root
    };
    static const httpd_uri_t uri_get_config = {
        .uri = "/config", .method = HTTP_GET, .handler = handle_get_config
    };
    static const httpd_uri_t uri_post_config = {
        .uri = "/config", .method = HTTP_POST, .handler = handle_post_config
    };
    static const httpd_uri_t uri_backup = {
        .uri = "/backup", .method = HTTP_GET, .handler = handle_backup
    };
    static const httpd_uri_t uri_post_reset = {
        .uri = "/reset", .method = HTTP_POST, .handler = handle_post_reset
    };
    static const httpd_uri_t uri_post_sim = {
        .uri = "/sim", .method = HTTP_POST, .handler = handle_sim
    };
    static const httpd_uri_t uri_sensors = {
        .uri = "/sensors", .method = HTTP_GET, .handler = handle_sensors
    };
    static const httpd_uri_t uri_diag = {
        .uri = "/diag", .method = HTTP_GET, .handler = handle_diag
    };
    static const httpd_uri_t uri_captive = {
        .uri = "/*", .method = HTTP_GET, .handler = handle_captive
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_get_config);
    httpd_register_uri_handler(s_server, &uri_backup);
    httpd_register_uri_handler(s_server, &uri_post_config);
    httpd_register_uri_handler(s_server, &uri_post_reset);
    httpd_register_uri_handler(s_server, &uri_post_sim);
    httpd_register_uri_handler(s_server, &uri_sensors);
    httpd_register_uri_handler(s_server, &uri_diag);
    httpd_register_uri_handler(s_server, &uri_captive);

    ESP_LOGI(TAG, "HTTP server started");
}

static void wifi_shutdown(void)
{
    ESP_LOGI(TAG, "Shutting down WiFi");
    wifi_state = WIFI_STATE_OFF;
    sim_flags = 0;

    if (dns_sock >= 0) {
        int s = dns_sock;
        dns_sock = -1;
        close(s);
    }
    if (dns_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
        dns_task_handle = NULL;
    }

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
}

static void wifi_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(START_DELAY_S * 1000));

    ESP_LOGI(TAG, "Starting WiFi SoftAP...");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               wifi_event_handler, NULL);

    const char *ssid = g_config.wifi_ssid[0] ? g_config.wifi_ssid : AP_SSID_DEFAULT;
    const char *pass = g_config.wifi_pass[0] ? g_config.wifi_pass : AP_PASS_DEFAULT;

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ssid);
    strncpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP \"%s\" started", ssid);
    wifi_state = WIFI_STATE_ACTIVE;

    start_http_server();
    xTaskCreate(dns_task_fn, "dns", 4096, NULL, 5, &dns_task_handle);

    int idle_seconds = 0;
    bool had_client = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (client_count > 0) {
            idle_seconds = 0;
            had_client = true;
        } else {
            idle_seconds++;
            int timeout = had_client ? ACTIVE_TIMEOUT_S : IDLE_TIMEOUT_S;
            if (idle_seconds >= timeout) {
                wifi_shutdown();
                break;
            }
        }
    }

    vTaskDelete(NULL);
}

wifi_state_t hal_wifi_get_state(void)
{
    return wifi_state;
}

void hal_wifi_init(void)
{
    xTaskCreate(wifi_task, "wifi_init", 4096, NULL, 5, NULL);
}
