#ifndef EVDSET_H
#define EVDSET_H

#include <Arduino.h>
#include <pgmspace.h>

#ifndef SPEAKER_CONFIG_STRUCT
#define SPEAKER_CONFIG_STRUCT
struct SpeakerConfig {
    char name[32];
    int volume;
    int low_freq;
    int high_freq;
    int source;            // 0 = Left, 1 = Right, 2 = Mono
};
#endif

// ---------------------------------------------------------------------------
//  JSON string escaping into a fixed buffer. No String, no heap.
// ---------------------------------------------------------------------------
inline size_t evdJsonEscape(const char* src, char* dst, size_t dstCap) {
    size_t o = 0;
    if (!src || dstCap == 0) { if (dstCap) dst[0] = '\0'; return 0; }
    for (const char* p = src; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        const char* esc = nullptr;
        char ubuf[7];
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            case '<':  esc = "\\u003C"; break;
            case '>':  esc = "\\u003E"; break;
            case '&':  esc = "\\u0026"; break;
            default:
                if (c < 0x20) { snprintf(ubuf, sizeof(ubuf), "\\u%04X", c); esc = ubuf; }
                break;
        }
        if (esc) {
            size_t l = strlen(esc);
            if (o + l >= dstCap) break;
            memcpy(dst + o, esc, l);
            o += l;
        } else {
            if (o + 1 >= dstCap) break;
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

// ---------------------------------------------------------------------------
//  Page shell, part 1: everything up to the config blob.
// ---------------------------------------------------------------------------
// [v1.1.2-cfg-fetch]
// Single static page, no runtime substitution. The config blob that used to
// be spliced into this block at serve time is now delivered separately by
// GET /api/cfg (handleApiCfg in EVDPWeb.h) and fetched by the JS at boot.
// This is what makes the page gzippable: a gzip stream cannot have raw JSON
// spliced into the middle of it, but a standalone static HTML block can.
static const char EVDSET_PAGE_HTML[] PROGMEM = R"EVD(<!DOCTYPE html><html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>Enkelvoud Node</title>
<style>
:root{
  --bg:#050510;
  --text-main:#fff;
  --text-muted:#cbd5e1;
  --accent-primary:#00d2ff;
  --accent-secondary:#8e2de2;
  --glass-bg:linear-gradient(135deg, rgba(255,255,255,0.12) 0%, rgba(255,255,255,0.02) 100%);
  --glass-border-light:rgba(255,255,255,0.3);
  --glass-border-dim:rgba(255,255,255,0.05);
  --danger:#ef4444;
  --success:#10b981;
}
*{margin:0;padding:0;box-sizing:border-box;font-family:'Inter',system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
html{-webkit-text-size-adjust:100%}
body{
  background-color:var(--bg);
  color:var(--text-main);
  min-height:100vh;
  display:flex;justify-content:center;align-items:flex-start;
  overflow-x:hidden;-webkit-overflow-scrolling:touch;
  position:relative;padding:20px;
}

/* -------- Galaxy background ------------------------------------------- */
.galaxy-bg{position:fixed;top:0;left:0;width:100vw;height:100vh;overflow:hidden;z-index:1;pointer-events:none}
.orb{position:absolute;border-radius:50%;filter:blur(70px);opacity:.75;will-change:transform}
.orb-1{width:400px;height:400px;background:#4a00e0;top:-100px;left:-100px;
       animation:float-a 18s infinite ease-in-out alternate}
.orb-2{width:500px;height:500px;background:#8e2de2;bottom:-150px;right:-100px;
       animation:float-b 22s infinite ease-in-out alternate;animation-delay:-6s}
.orb-3{width:300px;height:300px;background:#00d2ff;top:40%;left:45%;
       animation:float-c 15s infinite ease-in-out alternate;animation-delay:-9s}
@keyframes float-a{0%{transform:translate(0,0) scale(1)}50%{transform:translate(120px,-90px) scale(1.15)}100%{transform:translate(-40px,60px) scale(.95)}}
@keyframes float-b{0%{transform:translate(0,0) scale(1)}50%{transform:translate(-140px,80px) scale(1.1)}100%{transform:translate(60px,-40px) scale(.9)}}
@keyframes float-c{0%{transform:translate(-50%,-50%) scale(1)}50%{transform:translate(calc(-50% + 100px), calc(-50% - 70px)) scale(1.2)}100%{transform:translate(calc(-50% - 50px), calc(-50% + 50px)) scale(.85)}}

.stars,.stars::before,.stars::after{
  position:absolute;inset:0;background-repeat:repeat;
  animation:twinkle 3.6s infinite ease-in-out;
}
.stars{background-image:
  radial-gradient(1px 1px at 20px 30px, #fff, transparent),
  radial-gradient(1px 1px at 40px 70px, #fff, transparent),
  radial-gradient(1.5px 1.5px at 90px 40px, #fff, transparent);
  background-size:200px 200px;opacity:.55}
.stars::before{content:'';background-image:
  radial-gradient(1.5px 1.5px at 50px 160px, #fff, transparent),
  radial-gradient(1px 1px at 130px 80px, #fff, transparent),
  radial-gradient(2px 2px at 160px 120px, #fff, transparent);
  background-size:250px 250px;opacity:.35;animation-duration:5.2s;animation-delay:-1.4s}
.stars::after{content:'';background-image:
  radial-gradient(1px 1px at 15px 90px, #fff, transparent),
  radial-gradient(1px 1px at 110px 20px, #fff, transparent),
  radial-gradient(1.5px 1.5px at 70px 200px, #fff, transparent);
  background-size:180px 180px;opacity:.45;animation-duration:6.5s;animation-delay:-3.1s}
@keyframes twinkle{0%,100%{opacity:.2}50%{opacity:.85}}

/* -------- Dashboard container ----------------------------------------- */
.dashboard-container{
  position:relative;z-index:10;width:100%;max-width:900px;
  background:var(--glass-bg);
  backdrop-filter:blur(20px) saturate(180%);
  -webkit-backdrop-filter:blur(20px) saturate(180%);
  border-radius:24px;
  border-top:1px solid var(--glass-border-light);
  border-left:1px solid var(--glass-border-light);
  border-bottom:1px solid var(--glass-border-dim);
  border-right:1px solid var(--glass-border-dim);
  box-shadow:0 25px 50px rgba(0,0,0,0.5), inset 0 0 15px rgba(255,255,255,0.05);
  overflow:clip;display:flex;flex-direction:column;
  margin:20px 0;
}

/* -------- Sticky shell ------------------------------------------------ */
.sticky-shell{
  position:sticky;top:0;z-index:30;
  background:linear-gradient(180deg,
              rgba(10,10,22,0.96) 0%,
              rgba(10,10,22,0.88) 70%,
              rgba(10,10,22,0.78) 100%);
  backdrop-filter:blur(16px) saturate(180%);
  -webkit-backdrop-filter:blur(16px) saturate(180%);
  border-bottom:1px solid rgba(255,255,255,0.08);
}

.top-bar{
  display:flex;align-items:center;
  padding:14px 30px 6px;gap:14px;flex-wrap:wrap;
}
.brand{flex:0 0 auto;min-width:0}
.page-title{
  font-size:1.6rem;font-weight:700;letter-spacing:.5px;
  background:linear-gradient(to right, #00d2ff, #8e2de2);
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;
  text-shadow:0 2px 10px rgba(0,210,255,0.3);
  cursor: pointer;
  user-select: none;
  display: flex;
  align-items: center;
  gap: 8px;
}
.page-title svg {
  color: var(--text-muted);
  opacity: 0.5;
  transition: opacity 0.2s, color 0.2s;
  fill: currentColor;
}
.page-title:hover svg {
  opacity: 1 !important;
  color: #fff;
}
.node-sub{color:var(--text-muted);font-size:.78rem;margin-top:2px;word-break:break-word}
.top-actions{display:flex;align-items:center;gap:8px;flex:0 0 auto;flex-wrap:nowrap;margin-left:auto}

.menu-bar{
  display:flex;gap:10px;align-items:center;flex-wrap:wrap;
  flex:1 1 auto;padding:0;
}
.nav-btn{
  background:rgba(255,255,255,0.03);
  border:1px solid rgba(255,255,255,0.1);
  color:var(--text-muted);
  padding:10px 20px;border-radius:30px;
  font-size:.9rem;font-weight:500;
  cursor:pointer;transition:all .3s ease;
  display:flex;align-items:center;gap:8px;
  backdrop-filter:blur(5px);font-family:inherit;
}
.nav-btn:hover{background:rgba(255,255,255,0.1);color:var(--text-main)}
.nav-btn.active{
  background:linear-gradient(135deg, rgba(255,255,255,0.2), rgba(255,255,255,0.05));
  color:#fff;
  border-top:1px solid rgba(255,255,255,0.6);
  border-left:1px solid rgba(255,255,255,0.6);
  box-shadow:0 0 15px rgba(142,45,226,0.4);
  text-shadow:0 1px 5px rgba(0,210,255,0.5);
}

.nav-btn-primary{
  background:linear-gradient(135deg, rgba(0,210,255,0.35), rgba(142,45,226,0.35));
  color:#fff;
  border-color:rgba(255,255,255,0.28);
  box-shadow:0 0 12px rgba(0,210,255,0.3);
}
.nav-btn-primary:hover{
  background:linear-gradient(135deg, rgba(0,210,255,0.55), rgba(142,45,226,0.55));
}

.control-row{
  display:flex;align-items:center;gap:12px;
  padding:2px 30px 12px;flex-wrap:wrap;
}
.master-label{
  font-size:.85rem;color:var(--text-muted);
  text-transform:uppercase;letter-spacing:1px;white-space:nowrap;
}
.master-label b{color:var(--text-main);margin-left:6px}
.control-row #gvol{
  flex:1 1 180px;min-width:120px;
  -webkit-appearance:none;
  height:6px;background:rgba(255,255,255,0.2);
  border-radius:3px;outline:none;
  box-shadow:inset 0 1px 3px rgba(0,0,0,0.5);
}
.control-row #gvol::-webkit-slider-thumb{
  -webkit-appearance:none;
  width:18px;height:18px;border-radius:50%;
  background:#fff;cursor:pointer;
  border:2px solid var(--accent-secondary);
  box-shadow:0 0 15px rgba(0,210,255,0.8);
}


/* ---- CPU / RAM / PSRAM stat pills (sticky shell row 3) -------------- */
.stat-row{display:flex;gap:8px;padding:0 30px 10px;flex-wrap:wrap}
.stat-pill{
  background:rgba(255,255,255,0.06);
  border:1px solid rgba(255,255,255,0.12);
  color:var(--text-muted);
  padding:4px 12px;border-radius:12px;
  font-size:.72rem;font-weight:600;
  font-variant-numeric:tabular-nums;
  letter-spacing:.5px;white-space:nowrap;
}
.stat-pill.warn{color:#fbbf24;border-color:rgba(251,191,36,0.4)}
.stat-pill.crit{color:var(--danger);border-color:rgba(239,68,68,0.5)}

/* -------- Icon buttons ------------------------------------------------ */
.icon-btn{
  width:40px;height:40px;padding:0;border-radius:50%;
  background:rgba(255,255,255,0.06);
  border:1px solid rgba(255,255,255,0.14);
  color:var(--text-main);
  display:inline-flex;align-items:center;justify-content:center;
  cursor:pointer;text-decoration:none;font-size:1.15rem;line-height:1;
  transition:background .2s ease, border-color .2s ease;
  flex:0 0 auto;
}
.icon-btn:hover{background:rgba(255,255,255,0.14)}

/* -------- Tabs (content) --------------------------------------------- */
.tab-content{display:none;padding:24px 30px;animation:fadeIn .4s ease forwards}
.tab-content.active{display:block}
@keyframes fadeIn{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:translateY(0)}}

.section-title{
  font-size:1.1rem;margin:0 0 16px 0;font-weight:600;letter-spacing:.5px;
  background:linear-gradient(to right, #00d2ff, #8e2de2);
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;
  text-shadow:0 2px 10px rgba(0,210,255,0.2);
}

/* -------- Grid / cards ----------------------------------------------- */
.grid{display:grid;grid-template-columns:1fr;gap:20px}
.control-card{
  background:rgba(255,255,255,0.03);
  border:1px solid rgba(255,255,255,0.1);
  border-top:1px solid rgba(255,255,255,0.2);
  border-radius:16px;padding:20px;
  box-shadow:inset 0 2px 10px rgba(255,255,255,0.02);
  margin-bottom:16px;
}
.control-card h2{font-size:.95rem;font-weight:600;margin-bottom:12px;color:var(--text-main)}
.row{display:flex;justify-content:space-between;align-items:center;gap:12px}
.label{color:var(--text-muted);font-size:.82rem;text-transform:uppercase;letter-spacing:.5px}
.value{font-variant-numeric:tabular-nums;font-weight:600}
.field{margin-top:14px}
.field:first-child{margin-top:0}
.field > label{display:block;color:var(--text-muted);font-size:.82rem;margin-bottom:6px;text-transform:uppercase;letter-spacing:.5px}

input[type=text],input[type=password],input[type=number],select{
  width:100%;padding:11px 12px;
  background:rgba(0,0,0,.42);
  border:1px solid rgba(255,255,255,0.12);
  border-radius:10px;color:var(--text-main);
  font:inherit;font-size:16px;min-height:44px;
  transition:all .2s ease;
}
input:focus,select:focus{outline:none;border-color:var(--accent-primary);box-shadow:0 0 0 3px rgba(0,210,255,.18)}
select{
  appearance:none;-webkit-appearance:none;
  background-image:
    linear-gradient(45deg,transparent 50%,var(--text-muted) 50%),
    linear-gradient(135deg,var(--text-muted) 50%,transparent 50%);
  background-position:calc(100% - 17px) 50%,calc(100% - 12px) 50%;
  background-size:5px 5px,5px 5px;background-repeat:no-repeat;
  padding-right:36px;
}
select option{background:#111116;color:#f6f6f8}

.check{display:flex;align-items:center;gap:10px;min-height:44px;cursor:pointer}
.check input{width:20px;height:20px;accent-color:var(--accent-primary);flex:0 0 auto}

/* -------- Sliders ----------------------------------------------------- */
.slider{position:relative;height:44px;touch-action:pan-y;margin-top:8px}
.track,.fill{position:absolute;top:50%;margin-top:-3px;height:6px;border-radius:3px}
.track{left:0;right:0;background:rgba(255,255,255,.16);box-shadow:inset 0 1px 3px rgba(0,0,0,.5)}
.fill{background:linear-gradient(90deg,var(--accent-primary),var(--accent-secondary))}
.slider input[type=range]{
  position:absolute;left:0;right:0;top:0;width:100%;height:44px;margin:0;padding:0;
  background:none;-webkit-appearance:none;appearance:none;outline:none;
}
.slider input[type=range]::-webkit-slider-runnable-track{height:44px;background:none;border:none}
.slider input[type=range]::-webkit-slider-thumb{
  -webkit-appearance:none;appearance:none;
  height:22px;width:22px;border-radius:50%;background:#fff;
  border:2px solid var(--accent-secondary);
  box-shadow:0 0 15px rgba(0,210,255,0.8);
  margin-top:11px;cursor:grab;
}
.slider input[type=range]::-moz-range-track{height:44px;background:none;border:none}
.slider input[type=range]::-moz-range-thumb{
  height:18px;width:18px;border-radius:50%;background:#fff;
  border:2px solid var(--accent-secondary);
  box-shadow:0 0 15px rgba(0,210,255,0.8);cursor:grab;
}
.freq-row{display:flex;gap:8px;margin-top:8px}
.freq-row input{flex:1 1 0;min-width:0}

/* -------- Buttons ----------------------------------------------------- */
.btn{
  background:rgba(255,255,255,0.05);
  border:1px solid rgba(255,255,255,0.2);
  color:var(--text-main);
  padding:10px 18px;border-radius:8px;
  font:inherit;font-size:.92rem;font-weight:600;cursor:pointer;
  min-height:44px;transition:all .2s ease;
  text-decoration:none;display:inline-block;
}
.btn:hover{background:rgba(255,255,255,0.15)}
.btn:active{transform:translateY(1px)}
.btn:disabled{opacity:.5;cursor:default}
.btn-primary{
  background:linear-gradient(135deg,var(--accent-primary),var(--accent-secondary));
  border-color:transparent;color:#fff;
  box-shadow:0 0 18px rgba(0,210,255,0.4);
}
.btn-primary:hover{box-shadow:0 0 24px rgba(0,210,255,0.6)}
.btn-danger{color:var(--danger);border-color:rgba(239,68,68,.5)}
.btn-sm{padding:8px 14px;font-size:.85rem;min-height:38px}

.actionbar{
  position:fixed;left:0;right:0;bottom:0;z-index:40;
  display:flex;gap:10px;
  padding:12px 20px calc(12px + env(safe-area-inset-bottom));
  background:rgba(5,5,16,.92);
  border-top:1px solid rgba(255,255,255,0.08);
  backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);
}
.actionbar .btn{flex:1 1 0}
.actionbar .btn-primary{flex:2 1 0}

/* -------- Modal ------------------------------------------------------- */
.overlay{position:fixed;inset:0;z-index:60;display:none;align-items:flex-end;justify-content:center;background:rgba(0,0,0,.7)}
.overlay.open{display:flex}
.modal{
  width:100%;max-width:460px;
  background:linear-gradient(135deg, rgba(30,30,45,.95), rgba(15,15,25,.95));
  border:1px solid rgba(255,255,255,0.15);
  border-radius:20px 20px 0 0;
  padding:18px 18px calc(18px + env(safe-area-inset-bottom));
  backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);
}
#wifiList{max-height:46vh;overflow-y:auto;margin:12px 0;border:1px solid rgba(255,255,255,0.1);border-radius:10px}
.net{
  display:flex;justify-content:space-between;gap:12px;width:100%;text-align:left;
  padding:14px 12px;background:none;border:0;
  border-bottom:1px solid rgba(255,255,255,0.08);
  color:var(--text-main);font:inherit;cursor:pointer;min-height:48px;
  transition:background .15s ease;
}
.net:last-child{border-bottom:0}
.net:hover{background:rgba(255,255,255,0.05)}
.net .rssi{color:var(--text-muted);font-size:.82rem;flex:0 0 auto}

/* -------- Busy / loading overlay ------------------------------------- */
.overlay-center{align-items:center;justify-content:center;z-index:90}
.busy-box{
  display:flex;flex-direction:column;align-items:center;gap:14px;
  padding:26px 32px;border-radius:16px;min-width:220px;text-align:center;
  background:linear-gradient(135deg, rgba(30,30,45,.96), rgba(15,15,25,.96));
  border:1px solid rgba(255,255,255,0.15);
  box-shadow:0 25px 50px rgba(0,0,0,0.6);
  backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);
}
.busy-spinner{
  width:38px;height:38px;border-radius:50%;
  border:3px solid rgba(255,255,255,0.15);
  border-top-color:var(--accent-primary);
  animation:busySpin .8s linear infinite;
}
@keyframes busySpin{to{transform:rotate(360deg)}}
.busy-text{font-size:.95rem;font-weight:600;color:var(--text-main)}

/* -------- Toast ------------------------------------------------------- */
#toast{
  position:fixed;left:50%;transform:translateX(-50%);
  bottom:calc(20px + env(safe-area-inset-bottom));z-index:80;
  max-width:92vw;padding:12px 16px;border-radius:10px;
  background:rgba(20,20,30,.95);
  border:1px solid rgba(255,255,255,0.2);
  font-size:.9rem;opacity:0;pointer-events:none;
  transition:opacity .18s ease;
  backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);
}
#toast.show{opacity:1}
#toast.ok{border-color:rgba(16,185,129,.6);box-shadow:0 0 20px rgba(16,185,129,.3)}
#toast.err{border-color:rgba(239,68,68,.6);box-shadow:0 0 20px rgba(239,68,68,.3)}

/* -------- Mobile tweaks ----------------------------------------------- */
@media (max-width:600px){
  body{padding:8px}
  .dashboard-container{margin:10px 0;border-radius:18px}
  .top-bar{padding:10px 16px 4px;gap:8px}
  .page-title{font-size:1.25rem}
  .menu-bar{padding:0;gap:5px}
  .nav-btn{padding:7px 11px;font-size:.76rem;gap:4px}
  .control-row{padding:2px 16px 10px;gap:8px}
  .tab-content{padding:16px 18px}
  .stat-row{padding:0 16px 8px;gap:6px}
  .stat-pill{padding:3px 9px;font-size:.68rem}
  .control-card{padding:16px}
}
@media (min-width:720px){
  .grid{grid-template-columns:1fr 1fr;gap:20px}
  .grid.single{grid-template-columns:1fr}
}
@media (prefers-reduced-motion:reduce){
  .btn,.nav-btn,.control-card,.tab-content,#toast,.overlay,.modal,
  input,select{transition:none!important;animation:none!important}
}

    /* ==================================================================
       CLEAN THEME - same language as the PC dashboard: soft glass panel,
       hairline rows, quiet glows, sentence-case labels.
       ================================================================== */
    :root {
      --text-main: #f4f5fb;
      --text-muted: #b3b9d1;
      --accent-primary: #9db6ff;
      --accent-secondary: #b48cff;
      --glass-bg: linear-gradient(180deg, rgba(255,255,255,0.10), rgba(255,255,255,0.04));
      --glass-border-light: rgba(255,255,255,0.14);
      --glass-border-dim: rgba(255,255,255,0.10);
      --hair: rgba(255,255,255,0.10);
      --hair-strong: rgba(255,255,255,0.16);
    }
    body { background-color: #07070f; }
    .stars { display: none !important; }
    .orb { filter: blur(120px); opacity: .30; animation-duration: 40s; }
    .orb-1 { background: #4b2bff; }
    .orb-2 { background: #8a4bff; }
    .orb-3 { background: #1f8fff; opacity: .30; }

    .dashboard-container {
      border-radius: 30px;
      border: 1px solid var(--hair-strong);
      background: linear-gradient(180deg, rgba(22,22,38,0.74), rgba(12,12,22,0.72));
      backdrop-filter: blur(28px) saturate(140%);
      -webkit-backdrop-filter: blur(28px) saturate(140%);
      box-shadow: 0 40px 90px rgba(0,0,0,.55), inset 0 1px 0 rgba(255,255,255,.16);
    }
    .top-bar { padding: 22px 26px 14px; gap: 16px; align-items: center; }
    .page-title, .brand-title {
      background: none; -webkit-text-fill-color: currentColor; color: #fff;
      text-shadow: none; font-weight: 650; letter-spacing: -0.02em;
    }

    .nav-btn {
      border: 1px solid var(--hair); background: rgba(255,255,255,0.05);
      border-radius: 999px; color: var(--text-muted);
      padding: 8px 15px; font-weight: 550;
      transition: background .18s ease, color .18s ease, border-color .18s ease;
    }
    .nav-btn:hover { background: rgba(255,255,255,0.10); color: #fff; }
    .nav-btn.active {
      background: rgba(255,255,255,0.15); color: #fff;
      border-color: var(--hair-strong);
      box-shadow: inset 0 1px 0 rgba(255,255,255,.22);
    }

    .section-title {
      background: none; -webkit-text-fill-color: currentColor; color: #fff;
      text-shadow: none; font-weight: 600; letter-spacing: -0.01em;
    }
    .control-card, .zone-box, .enc-card, .ota-modal-panel {
      background: rgba(255,255,255,0.055);
      border: 1px solid var(--hair);
      border-radius: 20px;
      box-shadow: none;
      backdrop-filter: blur(14px); -webkit-backdrop-filter: blur(14px);
    }
    .zone-box { border-radius: 22px; }
    .node-chip {
      background: rgba(255,255,255,0.05); border: 1px solid var(--hair);
      border-radius: 14px;
    }
    .node-chip:hover { background: rgba(255,255,255,0.09); transform: none; }

    .btn-glass, .btn {
      background: rgba(255,255,255,0.07); border: 1px solid var(--hair);
      border-radius: 999px; color: var(--text-main); font-weight: 550;
    }
    .btn-glass:hover, .btn:hover { background: rgba(255,255,255,0.12); border-color: var(--hair-strong); }
    .btn-primary {
      background: linear-gradient(135deg, #9db6ff, #b48cff);
      border: none; border-radius: 999px; color: #0b0a16; font-weight: 650;
      box-shadow: 0 6px 20px rgba(150,140,255,.28);
    }
    select, input[type="text"], input[type="number"], input[type="file"], textarea {
      background: rgba(0,0,0,0.25); border: 1px solid var(--hair);
      border-radius: 12px; color: var(--text-main);
    }
    input[type="range"] { accent-color: var(--accent-secondary); }
    input[type="range"]::-webkit-slider-thumb {
      -webkit-appearance: none; appearance: none;
      width: 18px; height: 18px; border-radius: 50%;
      background: #fff; box-shadow: 0 2px 8px rgba(0,0,0,.45);
    }
    .stat-pill, .tel-pill {
      background: rgba(255,255,255,0.06); border: 1px solid var(--hair);
      border-radius: 10px;
    }
    .ota-modal-backdrop { background: rgba(4,4,10,0.62); backdrop-filter: blur(8px); }
    .ota-modal-panel {
      background: linear-gradient(180deg, rgba(30,30,52,0.94), rgba(16,16,30,0.96));
      border: 1px solid var(--hair-strong); border-radius: 24px;
    }
    #toast {
      background: rgba(20,20,36,0.95);
      border: 1px solid var(--hair-strong); border-radius: 14px;
    }

</style>
</head>
<body>
  <div class="galaxy-bg" aria-hidden="true">
    <div class="orb orb-1"></div>
    <div class="orb orb-2"></div>
    <div class="orb orb-3"></div>
    <div class="stars"></div>
  </div>

  <div class="dashboard-container">
    <div class="sticky-shell">
      <div class="top-bar">
        <div class="brand">
          <h1 class="page-title" id="server-title" onclick="openNameModal()" title="Click to edit node name">
            <span id="server-title-text">Enkelvoud Node</span>
            <svg viewBox="0 0 24 24" width="18" height="18" style="transition: opacity 0.2s, color 0.2s;">
              <path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04c.39-.39.39-1.02 0-1.41l-2.34-2.34c-.39-.39-1.02-.39-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/>
            </svg>
          </h1>
          <input type="hidden" id="friendly_name" name="friendly_name" form="setupForm" value="">
          <div class="node-sub" id="nodeSub">Loading settings…</div>
        </div>

        <nav class="menu-bar">
          <button type="button" class="nav-btn active" onclick="openTab(event, 'p-audio')">Audio</button>
          <button type="button" class="nav-btn"        onclick="openTab(event, 'p-network')">Network</button>
          <button type="button" class="nav-btn"        id="statusBtn"
                  title="Open node health &amp; logs in a new tab">Status</button>
          <button type="button" class="nav-btn"        id="revertBtn">Revert</button>
          <button type="button" class="nav-btn nav-btn-primary" id="saveBtn">Save</button>
        </nav>

        <div class="top-actions">
          <a class="icon-btn" id="openServerBtn" href="#" style="display:none"
             title="Open Server" aria-label="Open Server">&#9881;</a>
        </div>
      </div>

      <div class="control-row">
        <span class="master-label">Node Volume <b id="gvolVal">0%</b></span>
        <input type="range" id="gvol" min="0" max="100" step="1" value="0"
               aria-label="Node volume">
      </div>
      <div class="stat-row" aria-label="System load">
        <span class="stat-pill" id="stat-core0" title="Core 0 CPU load">C0 --%</span>
        <span class="stat-pill" id="stat-core1" title="Core 1 CPU load">C1 --%</span>
        <span class="stat-pill" id="stat-ram"   title="Internal heap used">RAM --%</span>
        <span class="stat-pill" id="stat-psram" title="PSRAM used" style="display:none">PS --%</span>
      </div>
    </div>

    <form id="setupForm">
      <section id="p-audio" class="tab-content active">
        <h2 class="section-title">Speakers &amp; Crossovers</h2>
        <div class="grid" id="speakerCards"></div>
      </section>

      <section id="p-network" class="tab-content">
        <h2 class="section-title">Wi-Fi</h2>
        <div class="control-card">
          <div class="field">
            <label for="ssid">Network name</label>
            <div style="display:flex;gap:8px">
              <input type="text" id="ssid" name="ssid"
                     autocomplete="on" autocapitalize="off" spellcheck="false">
              <button type="button" class="btn btn-sm" id="scanBtn"
                      style="flex:0 0 auto">Scan</button>
            </div>
          </div>
          <div class="field">
            <label for="pass">Password</label>
            <input type="password" id="pass" name="pass" autocomplete="off">
          </div>
        </div>

        <div class="control-card">
          <label class="check">
            <input type="checkbox" id="use_static" name="use_static">
            <span>Use a static IP</span>
          </label>
          <div id="staticFields" hidden>
            <div class="grid" style="margin-top:12px">
              <div class="field"><label for="static_ip">IP address</label>
                <input type="text" id="static_ip" name="static_ip"
                       inputmode="decimal" autocomplete="on"></div>
              <div class="field"><label for="static_gw">Gateway</label>
                <input type="text" id="static_gw" name="static_gw"
                       inputmode="decimal" autocomplete="on"></div>
              <div class="field"><label for="static_sn">Subnet mask</label>
                <input type="text" id="static_sn" name="static_sn"
                       inputmode="decimal" autocomplete="on"></div>
              <div class="field"><label for="static_dns">DNS server</label>
                <input type="text" id="static_dns" name="static_dns"
                       inputmode="decimal" autocomplete="on"></div>
            </div>
          </div>
        </div>
      </section>
    </form>
  </div>

  <div class="overlay overlay-center" id="busyOverlay"
       role="dialog" aria-modal="true" aria-label="Working">
    <div class="busy-box">
      <div class="busy-spinner" aria-hidden="true"></div>
      <div class="busy-text" id="busyText">Working…</div>
    </div>
  </div>

  <div class="overlay" id="wifiModal" role="dialog" aria-modal="true" aria-label="Available networks">
    <div class="modal">
      <div class="row">
        <strong>Available networks</strong>
        <button type="button" class="btn btn-sm" id="closeWifi">Close</button>
      </div>
      <div id="wifiList">
        <div style="padding:14px 12px;color:var(--text-muted)">Scanning…</div>
      </div>
    </div>
  </div>

  <div class="overlay" id="nameModal" role="dialog" aria-modal="true" aria-label="Edit Node Name">
    <div class="modal">
      <div class="row">
        <strong>Edit Node Name</strong>
        <button type="button" class="btn btn-sm" onclick="closeNameModal()">Close</button>
      </div>
      <div class="field" style="margin-top: 14px;">
        <label for="edit-name-input">New Node Name</label>
        <input type="text" id="edit-name-input" maxlength="32" autocomplete="off" spellcheck="false">
      </div>
      <div class="row" style="justify-content: flex-end; gap: 10px; margin-top: 14px;">
        <button type="button" class="btn" onclick="closeNameModal()">Cancel</button>
        <button type="button" class="btn btn-primary" onclick="saveName()">Save Name</button>
      </div>
    </div>
  </div>

  <div id="toast" role="status" aria-live="polite"></div>

<script>
// [v1.1.2-cfg-fetch / cfg-seed]
// Shape-correct default so openTab()'s lazy builders (buildSpeakers,
// populateNetworkTab, populateHardwareTab) never touch undefined fields
// if GET /api/cfg fails or races the user's first tab click. Filled in
// by bootFromCfg() once the real payload lands.
var CFG = {
  speakers: [],
  nodeVolume: 0, name: '', serverIp: '', serverFound: false,
  ssid: '', pass: '', useStatic: false, ip: '', gw: '', sn: '', dns: ''
};
(function(){
"use strict";
var $ = function(id){ return document.getElementById(id); };
var TYPES = {
  Full:    {min:0,    max:20000, lo:0,    hi:20000},
  Tweeter: {min:1000, max:20000, lo:2000, hi:20000},
  Mid:     {min:100,  max:8000,  lo:200,  hi:5000},
  Sub:     {min:0,    max:300,   lo:0,    hi:300}
};
var DAC_NAMES = ['Output 1','Output 2','Output 3','Output 4'];
var timers = {};

/* ---- toast ---- */
var toastTimer;
function toast(msg, kind){
  var t = $('toast');
  t.textContent = msg;
  t.className = 'show ' + (kind || '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(function(){ t.className = ''; }, 2600);
}

/* ---- busy / loading overlay ---- */
function showBusy(msg){
  var t = $('busyText'), o = $('busyOverlay');
  if (t) t.textContent = msg || 'Working…';
  if (o) o.classList.add('open');
}
function hideBusy(){
  var o = $('busyOverlay');
  if (o) o.classList.remove('open');
}

/* ---- network helper ---- */
function post(url, opts){
  var ctrl = new AbortController();
  var to = setTimeout(function(){ ctrl.abort(); }, 8000);
  opts = opts || {};
  opts.signal = ctrl.signal;
  return fetch(url, opts).then(function(r){
    clearTimeout(to);
    if (!r.ok) throw new Error('Node replied ' + r.status);
    return r;
  }, function(e){ clearTimeout(to); throw e; });
}
function sendAudio(body){
  return post('/api/volume', {
    method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)
  })['catch'](function(){ toast('Node did not accept that change', 'err'); });
}
function debounce(key, fn){ clearTimeout(timers[key]); timers[key] = setTimeout(fn, 140); }

/* ---- tabs ---- */
window.openTab = function(evt, tabId){
  document.querySelectorAll('.tab-content').forEach(function(p){
    p.classList.toggle('active', p.id === tabId);
  });
  document.querySelectorAll('.menu-bar .nav-btn').forEach(function(b){
    b.classList.remove('active');
  });
  if (evt && evt.currentTarget) evt.currentTarget.classList.add('active');
  else {
    // Programmatic fallback (not currently used, kept for parity).
    var match = document.querySelector('.menu-bar .nav-btn[onclick*="' + tabId + '"]');
    if (match) match.classList.add('active');
  }
  window.scrollTo(0, 0);

  // Build the tab's contents on first visit. Mirrors the server-side
  // dashboard, which calls fetchHeavyStats() / fetchSourceInfo() /
  // loadNetworkState() from openTab(). Here there is no HTTP fetch —
  // the data all arrived in the CFG blob at page load — but the same
  // deferral applies: don't touch DOM nodes that the user may never see.
  //
  // The once-only guard matters for p-audio: buildSpeakers() rebuilds
  // the speaker cards from CFG, which would clobber any unsaved slider
  // changes the user made since last visiting the tab.
  ensureTabBuilt(tabId);
};

var builtTabs = {};

function ensureTabBuilt(tabId) {
  if (builtTabs[tabId]) return;
  builtTabs[tabId] = true;

  if (tabId === 'p-audio') {
    buildSpeakers(CFG.speakers);
  } else if (tabId === 'p-network') {
    populateNetworkTab();
  }
}

function populateNetworkTab() {
  $('ssid').value          = CFG.ssid;
  $('pass').value          = CFG.pass;
  $('use_static').checked  = !!CFG.useStatic;
  $('static_ip').value     = CFG.ip;
  $('static_gw').value     = CFG.gw;
  $('static_sn').value     = CFG.sn;
  $('static_dns').value    = CFG.dns;
  if (typeof window.__syncStatic === 'function') window.__syncStatic();
}

/* ---- single-value slider ---- */
function paintSingle(input, fill){
  var min = +input.min, max = +input.max, v = +input.value;
  var pct = max > min ? ((v - min) / (max - min)) * 100 : 0;
  fill.style.left = '0%';
  fill.style.right = (100 - pct) + '%';
}

/* ---- dual-thumb slider, touch friendly ---- */
function sliderValueAt(ev, input){
  var r = input.getBoundingClientRect();
  if (!r.width) return +input.value;
  var x = ev.clientX - r.left;
  var min = +input.min, max = +input.max;
  var pct = Math.max(0, Math.min(r.width, x)) / r.width;
  var v = min + pct * (max - min);
  if (+input.step > 0) v = Math.round(v / +input.step) * +input.step;
  return Math.min(Math.max(v, min), max);
}
function thumbX(input, value){
  var r = input.getBoundingClientRect();
  var min = +input.min, max = +input.max;
  var pct = max > min ? ((value - min) / (max - min)) : 0;
  return r.left + pct * r.width;
}
function startDrag(ev, input, onValue){
  var move = function(e){
    if (e.pointerId !== ev.pointerId) return;
    var v = sliderValueAt(e, input);
    input.value = v;
    onValue(v);
  };
  var up = function(e){
    if (e.pointerId !== ev.pointerId) return;
    input.removeEventListener('pointermove', move);
    input.removeEventListener('pointerup', up);
    input.removeEventListener('pointercancel', up);
  };
  input.value = sliderValueAt(ev, input);
  onValue(input.value);
  input.addEventListener('pointermove', move);
  input.addEventListener('pointerup', up);
  input.addEventListener('pointercancel', up);
  try { input.setPointerCapture(ev.pointerId); } catch (e) { /* best effort */ }
  ev.preventDefault();
}
function bindSingle(input, onValue){
  input.addEventListener('pointerdown', function(ev){ startDrag(ev, input, onValue); });
  input.addEventListener('input', function(){ onValue(+input.value); });
}
function bindDual(lo, hi, onValue){
  function dragWhich(ev){
    var dLo = Math.abs(thumbX(lo, +lo.value) - ev.clientX);
    var dHi = Math.abs(thumbX(hi, +hi.value) - ev.clientX);
    var moved = dLo <= dHi ? lo : hi;
    startDrag(ev, moved, function(v){
      if (moved === lo) { lo.value = v; if (+hi.value < v) hi.value = v; }
      else              { hi.value = v; if (+lo.value > v) lo.value = v; }
      onValue();
    });
  }
  lo.addEventListener('pointerdown', dragWhich);
  hi.addEventListener('pointerdown', dragWhich);
}

function paintDual(i){
  var lo = $('low_' + i), hi = $('high_' + i);
  var min = +lo.min, max = +lo.max;
  var a = +lo.value, b = +hi.value;
  if (a > b) { a = b; lo.value = a; }
  $('loNum_' + i).value = a;
  $('hiNum_' + i).value = b;
  $('freqLbl_' + i).textContent = a + ' – ' + b + ' Hz';
  var span = max - min || 1;
  var f = $('freqFill_' + i);
  f.style.left  = (((a - min) / span) * 100) + '%';
  f.style.right = (((max - b) / span) * 100) + '%';
}

function applyType(i, type, push){
  var lim = TYPES[type] || TYPES.Full;
  var lo = $('low_' + i), hi = $('high_' + i);
  var loN = $('loNum_' + i), hiN = $('hiNum_' + i);
  lo.min = hi.min = loN.min = hiN.min = lim.min;
  lo.max = hi.max = loN.max = hiN.max = lim.max;
  if (push) { lo.value = lim.lo; hi.value = lim.hi; }
  else {
    lo.value = Math.min(Math.max(+lo.value, lim.min), lim.max);
    hi.value = Math.min(Math.max(+hi.value, lim.min), lim.max);
  }
  paintDual(i);
  if (push) {
    sendAudio({speaker_index:i, speaker_name:type,
               speaker_low:+lo.value, speaker_high:+hi.value});
  }
}

/* ---- build the four speaker cards ---- */
function buildSpeakers(list){
  var host = $('speakerCards');
  host.innerHTML = '';
  list.forEach(function(sp, i){
    var card = document.createElement('div');
    card.className = 'control-card';
    card.innerHTML =
      '<div class="row" style="margin-bottom:14px">' +
        '<span class="label">' + DAC_NAMES[i] + '</span>' +
        '<div style="display:flex;gap:8px">' +
        '<select name="spk_src_' + i + '" id="src_' + i + '" style="width:auto;min-width:88px" aria-label="' + DAC_NAMES[i] + ' input source">' +
          '<option value="0">Left</option><option value="1">Right</option><option value="2">Mono</option>' +
        '</select>' +
        '<select name="spk_name_' + i + '" id="type_' + i + '" style="width:auto;min-width:118px" aria-label="' + DAC_NAMES[i] + ' band type">' +
          '<option>Full</option><option>Tweeter</option><option>Mid</option><option>Sub</option>' +
        '</select>' +
        '</div>' +
      '</div>' +
      '<div class="row"><span class="label">Volume</span>' +
        '<span class="value" id="volLbl_' + i + '">0%</span></div>' +
      '<div class="slider">' +
        '<div class="track"></div><div class="fill" id="volFill_' + i + '"></div>' +
        '<input type="range" name="spk_vol_' + i + '" id="vol_' + i + '" min="0" max="100" step="1" ' +
          'value="0" aria-label="' + DAC_NAMES[i] + ' volume">' +
      '</div>' +
      '<div class="row" style="margin-top:14px"><span class="label">Crossover</span>' +
        '<span class="value" id="freqLbl_' + i + '">0 – 0 Hz</span></div>' +
      '<div class="slider dual">' +
        '<div class="track"></div><div class="fill" id="freqFill_' + i + '"></div>' +
        '<input type="range" name="spk_low_'  + i + '" id="low_'  + i + '" step="1" aria-label="High-pass">' +
        '<input type="range" name="spk_high_' + i + '" id="high_' + i + '" step="1" aria-label="Low-pass">' +
      '</div>' +
      '<div class="freq-row">' +
        '<input type="number" id="loNum_' + i + '" inputmode="numeric" aria-label="High-pass in hertz">' +
        '<input type="number" id="hiNum_' + i + '" inputmode="numeric" aria-label="Low-pass in hertz">' +
      '</div>';
    host.appendChild(card);

    var type = $('type_' + i), src = $('src_' + i), vol = $('vol_' + i), volFill = $('volFill_' + i);
    var lo = $('low_' + i), hi = $('high_' + i);
    var loN = $('loNum_' + i), hiN = $('hiNum_' + i);

    type.value = TYPES[sp.name] ? sp.name : 'Full';
    src.value = String(sp.source === 0 || sp.source === 1 || sp.source === 2 ? sp.source : 0);
    vol.value = sp.volume;
    $('volLbl_' + i).textContent = sp.volume + '%';
    paintSingle(vol, volFill);

    lo.min = hi.min = loN.min = hiN.min = 0;
    lo.max = hi.max = loN.max = hiN.max = 20000;
    lo.value = sp.low_freq; hi.value = sp.high_freq;
    applyType(i, type.value, false);

    type.addEventListener('change', function(){ applyType(i, type.value, true); });

    src.addEventListener('change', function(){
      sendAudio({speaker_index:i, speaker_source:+src.value});
    });

    function volChanged(){
      $('volLbl_' + i).textContent = vol.value + '%';
      paintSingle(vol, volFill);
      debounce('v' + i, function(){ sendAudio({speaker_index:i, speaker_volume:+vol.value}); });
    }
    vol.addEventListener('input', volChanged);
    bindSingle(vol, volChanged);

    function fromDual(){
      paintDual(i);
      debounce('f' + i, function(){
        sendAudio({speaker_index:i, speaker_low:+lo.value, speaker_high:+hi.value});
      });
    }
    bindDual(lo, hi, fromDual);

    function fromNumber(movedLow){
      var min = +lo.min, max = +lo.max;
      var a = parseInt(loN.value, 10); if (isNaN(a)) a = min;
      var b = parseInt(hiN.value, 10); if (isNaN(b)) b = max;
      a = Math.min(Math.max(a, min), max);
      b = Math.min(Math.max(b, min), max);
      if (a > b) { if (movedLow) b = a; else a = b; }
      lo.value = a; hi.value = b;
      paintDual(i);
      debounce('f' + i, function(){ sendAudio({speaker_index:i, speaker_low:a, speaker_high:b}); });
    }
    loN.addEventListener('change', function(){ fromNumber(true); });
    hiN.addEventListener('change', function(){ fromNumber(false); });
  });
}

/* ---- node volume ---- */
(function(){
  var g = $('gvol'), lbl = $('gvolVal');
  function gvolChanged(){
    lbl.textContent = g.value + '%';
    debounce('g', function(){ sendAudio({volume: +g.value / 100}); });
  }
  g.addEventListener('input', gvolChanged);
  bindSingle(g, gvolChanged);
})();

/* ---- static IP ---- */
(function(){
  var chk = $('use_static'), box = $('staticFields');
  function sync(){ box.hidden = !chk.checked; }
  chk.addEventListener('change', sync);

  // Per-field ownership flags. While a field is "owned" (true) we treat
  // its content as a suggestion derived from static_ip and rewrite it on
  // every keystroke in static_ip. The moment the user types into that
  // field we hand ownership over (false) and stop touching it; clearing
  // the field to empty hands ownership back. This is what makes the
  // suggestion track the IP live instead of only firing when the field
  // is empty, while still letting a user type a custom value that the
  // IP editor will not clobber on the next keystroke.
  var own = { static_gw: true, static_sn: true, static_dns: true };

  ['static_gw','static_sn','static_dns'].forEach(function(id){
    $(id).addEventListener('input', function(){
      own[id] = (this.value.trim() === '');
    });
  });

  function applySuggestions(){
    var p = $('static_ip').value.trim().split('.');
    if (p.length !== 4) return;
    for (var i = 0; i < 4; i++) {
      if (!/^\d{1,3}$/.test(p[i]) || +p[i] > 255) return;
    }
    var base = p[0] + '.' + p[1] + '.' + p[2] + '.1';
    if (own.static_gw)  $('static_gw').value  = base;
    if (own.static_dns) $('static_dns').value = base;
    if (own.static_sn)  $('static_sn').value  = '255.255.255.0';
  }

  $('static_ip').addEventListener('input', applySuggestions);
  window.__refreshStaticSuggestions = applySuggestions;

  window.__syncStatic = sync;
})();

/* ---- Wi-Fi scan ---- */
(function(){
  var modal = $('wifiModal'), list = $('wifiList');
  $('closeWifi').addEventListener('click', function(){ modal.classList.remove('open'); });
  modal.addEventListener('click', function(e){ if (e.target === modal) modal.classList.remove('open'); });
  $('scanBtn').addEventListener('click', function(){
    modal.classList.add('open');
    list.innerHTML = '<div style="padding:14px 12px;color:var(--text-muted)">Scanning…</div>';
    post('/scan').then(function(r){ return r.json(); }).then(function(nets){
      list.innerHTML = '';
      if (!nets || !nets.length) {
        list.innerHTML = '<div style="padding:14px 12px;color:var(--text-muted)">No networks found. Try again.</div>';
        return;
      }
      nets.sort(function(a, b){ return b.rssi - a.rssi; }).forEach(function(n){
        var b = document.createElement('button');
        b.type = 'button'; b.className = 'net';
        var s = document.createElement('span'); s.textContent = n.ssid || '(hidden)';
        var r = document.createElement('span'); r.className = 'rssi'; r.textContent = n.rssi + ' dBm';
        b.appendChild(s); b.appendChild(r);
        b.addEventListener('click', function(){
          $('ssid').value = n.ssid || '';
          modal.classList.remove('open');
          $('pass').focus();
        });
        list.appendChild(b);
      });
    })['catch'](function(){
      list.innerHTML = '<div style="padding:14px 12px;color:var(--danger)">Scan failed. The node may be busy playing audio — try again.</div>';
    });
  });
})();

/* ---- node status ---- */
$('statusBtn').addEventListener('click', function(){
  window.open('/status', '_blank', 'noopener');
});

/* ---- save / revert ---- */
/* ---- node name modal ---- */
window.openNameModal = function() {
  var modal = document.getElementById('nameModal');
  var input = document.getElementById('edit-name-input');
  if (modal && input) {
    input.value = document.getElementById('friendly_name').value || 'Enkelvoud Node';
    modal.classList.add('open');
    setTimeout(function(){ input.focus(); input.select(); }, 100);
  }
};

window.closeNameModal = function() {
  var modal = document.getElementById('nameModal');
  if (modal) modal.classList.remove('open');
};

window.saveName = function() {
  var newName = document.getElementById('edit-name-input').value.trim();
  if (!newName) return;
  document.getElementById('friendly_name').value = newName;
  document.getElementById('server-title-text').textContent = newName;
  closeNameModal();
  document.getElementById('saveBtn').click(); // Triggers the save sequence
};

var nameInput = document.getElementById('edit-name-input');
if (nameInput) {
  nameInput.addEventListener('keydown', function(e) {
    if (e.key === 'Enter') { 
      e.preventDefault(); 
      saveName(); 
    }
  });
}

/* ---- save / revert ---- */
$('saveBtn').addEventListener('click', function(){
  var btn = this;
  var orig = btn.textContent;
  btn.disabled = true; btn.textContent = 'Saving…';

  var fd = new FormData($('setupForm'));

  post('/save', {method:'POST', body:new URLSearchParams(fd)})
    .then(function(r){
      if (r.status === 202) {
        // AP mode: connection test started
        showBusy('Testing connection…');
        var pollTest = function() {
          fetch('/api/network-test', { cache: 'no-store' })
            .then(function(res){ return res.json(); })
            .then(function(d){
              if (d.state === 'success') {
                showBusy('Connected! Rebooting…');
                setTimeout(function(){ location.reload(); }, 5000);
              } else if (d.state === 'failed') {
                hideBusy();
                toast('Connection failed: ' + d.error, 'err');
                btn.disabled = false; btn.textContent = orig;
              } else {
                setTimeout(pollTest, 500);
              }
            })
            .catch(function(){ setTimeout(pollTest, 500); });
        };
        pollTest();
      } else {
        toast('Saved to flash', 'ok');
        btn.disabled = false; btn.textContent = orig;
      }
    })['catch'](function(e){
      hideBusy();
      toast('Not saved: ' + e.message, 'err');
      btn.disabled = false; btn.textContent = orig;
    });
});

$('revertBtn').addEventListener('click', function(){
  var btn = this;
  btn.disabled = true;

  // Full-screen loading overlay while we tell the node to discard the
  // in-page edits and reload from the last-saved config. Two stages:
  // first we wait on /cancel, then we swap the text as the browser
  // navigates back to /. The overlay hides itself on error so the user
  // can retry.
  showBusy('Reverting to saved settings…');
  post('/cancel', {method:'POST'}).then(function(){
    showBusy('Reloading…');
    setTimeout(function(){ location.reload(); }, 450);
  })['catch'](function(e){
    hideBusy();
    toast('Could not revert: ' + e.message, 'err');
    btn.disabled = false;
  });
});

/* ---- boot from server-fetched config ----------------------------------
   [v1.1.2-cfg-fetch]
   The page no longer carries an inline `var CFG = {...}` blob. The whole
   page is a single static PROGMEM string, and the config arrives via
   GET /api/cfg at boot. bootFromCfg() is called once, after that fetch
   resolves. If it fails the page still renders (the sticky shell and
   tabs are static HTML) and shows a toast instead of half-populating
   fields with undefined values.

   No CFG value is read outside this function, so nothing else in the
   script has to know about the fetch ordering. */
function bootFromCfg(){
  // Always-visible chrome only. Everything inside a tab is built lazily
  // by ensureTabBuilt(), which openTab() calls on first visit. The two
  // exceptions are the volume slider and the Open Server button, which
  // live in the sticky shell and are visible from every tab.
  var g = $('gvol');
  g.value = CFG.nodeVolume;
  $('gvolVal').textContent = CFG.nodeVolume + '%';

  $('server-title-text').textContent = CFG.name || 'Enkelvoud Node';
  $('friendly_name').value = CFG.name || 'Enkelvoud Node';
  $('nodeSub').textContent = (CFG.name || 'Unnamed node') + ' → ' + (CFG.serverIp ? 'connected' : 'no server');

  /* ---- Open Server button ------------------------------------------
     The node no longer stores a server address; it only knows the server IP
     it discovered (CFG.serverIp), so the button appears once connected. */
  (function(){
    var btn = $('openServerBtn');
    if (!btn) return;
    var url = '';
    if (CFG.serverFound && CFG.serverIp) {
      url = 'http://' + CFG.serverIp + '/';
    }
    if (url) { btn.href = url; btn.style.display = ''; }
  })();

  // [boot-race-rebuild]
  // If the user clicks Audio or Network before /api/cfg resolves,
  // ensureTabBuilt() runs against the *default* CFG (speakers:[],
  // empty ssid/pass/…) and then early-returns on every subsequent
  // call because it already set builtTabs[tabId] = true. bootFromCfg()
  // would then call ensureTabBuilt() below, get the early return, and
  // the tabs would stay empty until a page reload.
  //
  // Clearing builtTabs forces a fresh build with the CFG that just
  // arrived. This is safe to do unconditionally — it costs one extra
  // DOM rebuild of two tabs, only on boot.
  builtTabs = {};

  // p-audio is the default-active tab in the markup, so it needs its
  // first build now. openTab() will not fire for it on load.
  ensureTabBuilt('p-audio');

  // CRITICAL: Populate the Network tab immediately at boot. If this is left
  // unbuilt until the user clicks the tab, hitting Save while on the Audio
  // tab submits empty strings for SSID/Password/etc in FormData, silently
  // wiping the node's saved Wi-Fi credentials.
  ensureTabBuilt('p-network');
}

// Kick off the config fetch, then hand off to bootFromCfg().
fetch('/api/cfg', { cache: 'no-store' })
  .then(function(r){ if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
  .then(function(cfg){ CFG = cfg; bootFromCfg(); })
  .catch(function(e){
    var t = $('toast');
    if (t) { t.textContent = 'Failed to load node config: ' + e.message;
             t.className = 'show err'; }
  });

window.addEventListener('resize', function(){
  for (var i = 0; i < 4; i++) {
    if ($('vol_' + i)) { paintSingle($('vol_' + i), $('volFill_' + i)); paintDual(i); }
  }
});

/* ---- CPU / RAM / PSRAM stat pills -------------------------------- */
function setPill(el, text, pct){
  if (!el) return;
  el.textContent = text;
  el.classList.remove('warn','crit');
  if (typeof pct === 'number' && pct >= 0){
    if (pct >= 85)      el.classList.add('crit');
    else if (pct >= 65) el.classList.add('warn');
  }
}
function paintLoadPills(h){
  if (!h) return;
  var c0 = $('stat-core0'), c1 = $('stat-core1');
  var rm = $('stat-ram'),   ps = $('stat-psram');

  if (typeof h.cpuCore0Pct === 'number' && h.cpuCore0Pct >= 0)
    setPill(c0, 'C0 ' + h.cpuCore0Pct + '%', h.cpuCore0Pct);
  else
    setPill(c0, 'C0 n/a', -1);

  if (typeof h.cpuCore1Pct === 'number' && h.cpuCore1Pct >= 0)
    setPill(c1, 'C1 ' + h.cpuCore1Pct + '%', h.cpuCore1Pct);
  else
    setPill(c1, 'C1 n/a', -1);

  if (typeof h.heapUsedPct === 'number')
    setPill(rm, 'RAM ' + h.heapUsedPct + '%', h.heapUsedPct);

  if (ps){
    if (typeof h.psramTotalBytes === 'number' && h.psramTotalBytes > 0){
      ps.style.display = '';
      setPill(ps, 'PS ' + (h.psramUsedPct || 0) + '%', h.psramUsedPct || 0);
    } else {
      ps.style.display = 'none';
    }
  }
}
async function pollHealth(){
  if (document.hidden) return;
  try {
    var r = await fetch('/api/health', { cache: 'no-store' });
    if (!r.ok) return;
    paintLoadPills(await r.json());
  } catch (e) { /* transient; next tick retries */ }
}
(function healthLoop(){
  pollHealth().then(function(){ setTimeout(healthLoop, 5000); });
})();
})();
</script>
</body>
</html>
)EVD";

// [v1.1.2-cfg-fetch]
// streamWebPageTemplate() was removed here. The page is now a single static
// PROGMEM block (EVDSET_PAGE_HTML) served directly by handleRoot() in
// EVDPWeb.h. The config JSON it used to build is now the response body of
// GET /api/cfg (handleApiCfg in EVDPWeb.h).
//
// Side effect: since the page no longer has raw JSON spliced into it at
// serve time, it can be pre-gzipped and served with Content-Encoding: gzip.
// Run gen_gz.py to regenerate EVDSET_GZ.h, then update handleRoot() in
// EVDPWeb.h to use beginResponse_P(EVDSET_HTML_GZ, ...) with the
// Content-Encoding header, matching the server's pattern.

// ---------------------------------------------------------------------------
// Standalone Status page. Restyled to match the dashboard look, but the same
// two JSON endpoints it always used. Same animation treatment as the setup
// page: three independent orbs and three-layer twinkling stars.
// ---------------------------------------------------------------------------
static const char EVDSET_STATUS_PAGE[] PROGMEM = R"EVDS(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Enkelvoud Node — Status</title>
<style>
:root{
  --bg:#050510;--text:#fff;--muted:#cbd5e1;
  --accent-primary:#00d2ff;--accent-secondary:#8e2de2;
  --danger:#ef4444;--ok:#10b981;--warn:#e8b339;
}
*{margin:0;padding:0;box-sizing:border-box;font-family:'Inter',system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
body{color:var(--text);background:var(--bg);min-height:100vh;padding:18px;position:relative;overflow-x:hidden}

.galaxy-bg{position:fixed;top:0;left:0;width:100vw;height:100vh;overflow:hidden;z-index:0;pointer-events:none}
.orb{position:absolute;border-radius:50%;filter:blur(70px);opacity:.65;will-change:transform}
.orb-1{width:300px;height:300px;background:#4a00e0;top:-100px;left:-100px;
       animation:float-a 18s infinite ease-in-out alternate}
.orb-2{width:400px;height:400px;background:#8e2de2;bottom:-150px;right:-100px;
       animation:float-b 22s infinite ease-in-out alternate;animation-delay:-6s}
.orb-3{width:250px;height:250px;background:#00d2ff;top:40%;left:45%;
       animation:float-c 15s infinite ease-in-out alternate;animation-delay:-9s}
@keyframes float-a{0%{transform:translate(0,0) scale(1)}50%{transform:translate(90px,-70px) scale(1.15)}100%{transform:translate(-30px,50px) scale(.95)}}
@keyframes float-b{0%{transform:translate(0,0) scale(1)}50%{transform:translate(-110px,60px) scale(1.1)}100%{transform:translate(50px,-30px) scale(.9)}}
@keyframes float-c{0%{transform:translate(-50%,-50%) scale(1)}50%{transform:translate(calc(-50% + 80px), calc(-50% - 60px)) scale(1.2)}100%{transform:translate(calc(-50% - 40px), calc(-50% + 40px)) scale(.85)}}

.stars,.stars::before,.stars::after{position:absolute;inset:0;background-repeat:repeat;animation:twinkle 3.6s infinite ease-in-out}
.stars{background-image:
  radial-gradient(1px 1px at 20px 30px, #fff, transparent),
  radial-gradient(1px 1px at 40px 70px, #fff, transparent),
  radial-gradient(1.5px 1.5px at 90px 40px, #fff, transparent);
  background-size:200px 200px;opacity:.55}
.stars::before{content:'';background-image:
  radial-gradient(1.5px 1.5px at 50px 160px, #fff, transparent),
  radial-gradient(1px 1px at 130px 80px, #fff, transparent),
  radial-gradient(2px 2px at 160px 120px, #fff, transparent);
  background-size:250px 250px;opacity:.35;animation-duration:5.2s;animation-delay:-1.4s}
.stars::after{content:'';background-image:
  radial-gradient(1px 1px at 15px 90px, #fff, transparent),
  radial-gradient(1px 1px at 110px 20px, #fff, transparent),
  radial-gradient(1.5px 1.5px at 70px 200px, #fff, transparent);
  background-size:180px 180px;opacity:.45;animation-duration:6.5s;animation-delay:-3.1s}
@keyframes twinkle{0%,100%{opacity:.2}50%{opacity:.85}}

.wrap{max-width:920px;margin:0 auto;position:relative;z-index:1}
header{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:20px;flex-wrap:wrap}
h1{font-size:1.3rem;font-weight:700;
  background:linear-gradient(to right, #00d2ff, #8e2de2);
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;
  text-shadow:0 2px 10px rgba(0,210,255,0.3)}
.stamp{color:var(--muted);font-size:.8rem}

.section-title{font-size:.75rem;color:var(--muted);text-transform:uppercase;
  letter-spacing:1.2px;margin:18px 0 8px 4px;font-weight:600}

.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-bottom:4px}
.card{background:rgba(255,255,255,0.04);
  border:1px solid rgba(255,255,255,0.1);
  border-top:1px solid rgba(255,255,255,0.2);
  border-radius:14px;padding:12px 14px;
  backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);
  box-shadow:inset 0 2px 10px rgba(255,255,255,0.02)}
.card .k{font-size:.68rem;color:var(--muted);text-transform:uppercase;letter-spacing:.8px}
.card .v{font-size:1.05rem;font-weight:600;margin-top:5px;word-break:break-word;text-shadow:0 1px 5px rgba(0,0,0,.5)}
.card .sub{font-size:.72rem;color:var(--muted);margin-top:2px}

.ok{color:var(--ok)} .bad{color:var(--danger)} .warn{color:var(--warn)}

.err-panel{margin-top:14px;border-radius:14px;padding:14px;
  border:1px solid rgba(16,185,129,.4);background:rgba(16,185,129,.04)}
.err-panel.has-errors{border-color:rgba(239,68,68,.5);background:rgba(239,68,68,.06)}
.err-panel .head{font-size:.75rem;text-transform:uppercase;letter-spacing:1px;
  margin-bottom:10px;font-weight:700}
.err-panel.has-errors .head{color:var(--danger)}
.err-panel.clean .head{color:var(--ok)}
.err-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:8px 16px}
.err-item{display:flex;justify-content:space-between;font-size:.82rem;
  padding:5px 0;border-bottom:1px solid rgba(255,255,255,.05)}
.err-item:last-child{border-bottom:0}
.err-item .name{color:var(--muted)}
.err-item .val{font-weight:600;font-variant-numeric:tabular-nums}
.err-item .val.zero{color:var(--ok)}
.err-item .val.nonzero{color:var(--danger)}

#logs,#rawHealth{background:rgba(0,0,0,.4);margin-top:16px;
  border:1px solid rgba(255,255,255,0.1);border-radius:14px;padding:14px;
  font:12px/1.6 ui-monospace,Consolas,monospace;white-space:pre-wrap;word-break:break-word;
  max-height:50vh;overflow-y:auto;
  backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px)}
#rawHealth{max-height:40vh;font-size:11.5px}
.l-err{color:var(--danger)} .l-warn{color:var(--warn)} .l-info{color:var(--muted)}

/* -------- Start / Clear controls ------------------------------------- */
.controls{
  display:flex;gap:10px;align-items:center;
  margin:0 0 18px 0;padding:12px 16px;border-radius:14px;
  background:rgba(255,255,255,0.04);
  border:1px solid rgba(255,255,255,0.1);
  border-top:1px solid rgba(255,255,255,0.2);
}
.controls button{
  font:inherit;font-weight:600;font-size:.88rem;
  padding:8px 22px;border-radius:8px;cursor:pointer;min-height:40px;
  transition:all .2s ease;
}
.btn-start{
  background:linear-gradient(135deg,#00d2ff,#8e2de2);
  border:none;color:#fff;
  box-shadow:0 0 14px rgba(0,210,255,0.35);
}
.btn-start:hover{filter:brightness(1.1);}
.btn-start.active{
  background:linear-gradient(135deg,#ef4444,#b91c1c);
  box-shadow:0 0 14px rgba(239,68,68,0.35);
}
.btn-clear{
  background:rgba(255,255,255,0.06);
  border:1px solid rgba(255,255,255,0.2);
  color:var(--text);
}
.btn-clear:hover{background:rgba(255,255,255,0.14);}
.ctrl-state{
  margin-left:auto;color:var(--muted);font-size:.82rem;
  font-variant-numeric:tabular-nums;
}

@media (prefers-reduced-motion:reduce){
  .card,#logs,.controls,.controls button{transition:none!important;animation:none!important}
}

    /* ==== CLEAN THEME (status page) - same palette as the settings page ==== */
    :root {
      --bg: #07070f;
      --text: #f4f5fb;
      --muted: #b3b9d1;
      --accent-primary: #9db6ff;
      --accent-secondary: #b48cff;
      --danger: #ff6b81;
      --ok: #4ade80;
      --warn: #fbbf24;
      --hair: rgba(255,255,255,0.10);
    }
    body { background-color: #07070f; color: var(--text); }
    .card { background: rgba(255,255,255,0.055) !important;
            border: 1px solid var(--hair) !important;
            border-radius: 18px !important; box-shadow: none !important; }
    .btn, button { border-radius: 999px !important; border: 1px solid var(--hair) !important;
                   background: rgba(255,255,255,0.07) !important; color: var(--text) !important; }
    input, select { background: rgba(0,0,0,0.25) !important; border: 1px solid var(--hair) !important;
                    border-radius: 10px !important; color: var(--text) !important; }

</style>
</head>
<body>
<div class="galaxy-bg" aria-hidden="true">
  <div class="orb orb-1"></div><div class="orb orb-2"></div><div class="orb orb-3"></div>
  <div class="stars"></div>
</div>
<div class="wrap">
  <header>
    <h1>Enkelvoud Node — Status &amp; Logs</h1>
    <span class="stamp" id="stamp">idle</span>
  </header>

  <div class="controls">
    <button id="startBtn" class="btn-start" type="button">Start</button>
    <button id="clearBtn" class="btn-clear" type="button">Clear</button>
    <button id="recordBtn" class="btn-clear" type="button">Record 10 min</button>
    <span class="ctrl-state" id="ctrlState">stopped</span>
  </div>

  <div class="section-title">System</div>
  <div class="grid" id="sysCards"></div>

  <div class="section-title">Network</div>
  <div class="grid" id="netCards"></div>

  <div class="section-title">Encoder (as received)</div>
  <div class="grid" id="encCards"></div>

  <div class="section-title">Stutter Diagnostics</div>
  <div class="grid" id="stutCards"></div>

  <div class="section-title">Audio</div>
  <div class="grid" id="audCards"></div>

  <div class="err-panel clean" id="errPanel">
    <div class="head" id="errHead">Problems</div>
    <div class="err-grid" id="errList"></div>
  </div>

  <div class="section-title">Log</div>
  <div id="logs">Connecting…</div>

  <div class="section-title">Raw Health JSON</div>
  <div id="rawHealth">(press Start to load)</div>

  <div id="recProgressOverlay" style="position:fixed;inset:0;z-index:200;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,0.75);padding:20px">
    <div style="width:100%;max-width:460px;background:linear-gradient(135deg,rgba(30,25,55,0.97),rgba(15,10,30,0.97));border:1px solid rgba(255,255,255,0.15);border-radius:20px;padding:24px;box-shadow:0 25px 60px rgba(0,0,0,0.6)">
      <h3 style="font-size:1.1rem;margin-bottom:14px;color:#fff">Recording Diagnostics</h3>
      <p style="font-size:.85rem;color:#cbd5e1;line-height:1.5">Capturing /api/health + /api/logs every second for up to 10 minutes. Press <b>Finish Now</b> at any time to stop and download what has been captured so far.</p>
      <div style="height:8px;background:rgba(255,255,255,0.1);border-radius:4px;overflow:hidden;margin:16px 0">
        <div id="recProgressFill" style="height:100%;width:0%;background:linear-gradient(to right,#00d2ff,#8e2de2);transition:width .3s ease"></div>
      </div>
      <p style="font-size:.9rem;color:#cbd5e1;font-variant-numeric:tabular-nums" id="recProgressText">0.0s / 600s</p>
      <button type="button" id="recFinishBtn" class="btn-start" style="margin-top:14px;width:100%">Finish Now</button>
    </div>
  </div>

  <div id="recResultOverlay" style="position:fixed;inset:0;z-index:200;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,0.75);padding:20px">
    <div style="width:100%;max-width:720px;max-height:90vh;background:linear-gradient(135deg,rgba(30,25,55,0.97),rgba(15,10,30,0.97));border:1px solid rgba(255,255,255,0.15);border-radius:20px;padding:24px;box-shadow:0 25px 60px rgba(0,0,0,0.6);display:flex;flex-direction:column">
      <h3 style="font-size:1.1rem;margin-bottom:14px;color:#fff">Diagnostic Recording Complete</h3>
      <p style="font-size:.82rem;color:#cbd5e1;margin-bottom:10px">Copy or download. Paste the file for analysis.</p>
      <textarea id="recResultText" readonly style="flex:1 1 auto;width:100%;min-height:300px;max-height:60vh;padding:12px;background:rgba(0,0,0,0.5);border:1px solid rgba(255,255,255,0.15);border-radius:10px;color:#cbd5e1;font-family:ui-monospace,'SF Mono',Menlo,Consolas,monospace;font-size:.72rem;line-height:1.4;resize:vertical"></textarea>
      <div style="display:flex;gap:10px;margin-top:14px;justify-content:flex-end">
        <button type="button" class="btn-clear" onclick="document.getElementById('recResultOverlay').style.display='none'">Close</button>
        <button type="button" class="btn-clear" onclick="recCopy()">Copy</button>
        <button type="button" class="btn-start" onclick="recDownload()">Download .txt</button>
      </div>
    </div>
  </div>
</div>
<script>
var $=function(id){return document.getElementById(id)};
function esc(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML;}
function fmt(sec){
  sec = sec|0;
  var m=Math.floor(sec/60)%60,h=Math.floor(sec/3600);
  return (h?h+'h ':'')+(m?m+'m ':'')+(sec%60)+'s';
}
function fmtAge(ms){
  if (ms === 0xFFFFFFFF || ms === null || ms === undefined) return 'never';
  ms = ms|0;
  if (ms < 1000) return ms + ' ms';
  if (ms < 60000) return (ms/1000).toFixed(1) + ' s';
  return fmt(Math.floor(ms/1000));
}
function card(k,v,cls,sub){
  return '<div class="card"><div class="k">'+esc(k)+'</div>'+
         '<div class="v '+(cls||'')+'">'+esc(v)+'</div>'+
         (sub?'<div class="sub">'+esc(sub)+'</div>':'')+'</div>';
}
function pctLabel(v){
  return (typeof v === 'number' && v >= 0) ? (v + '%') : 'n/a';
}
function pctClass(v){
  if (typeof v !== 'number' || v < 0) return '';
  if (v >= 85) return 'bad';
  if (v >= 65) return 'warn';
  return 'ok';
}
function errItem(name,val){
  var cls = (val|0) === 0 ? 'zero' : 'nonzero';
  return '<div class="err-item"><span class="name">'+esc(name)+
         '</span><span class="val '+cls+'">'+esc(String(val|0))+'</span></div>';
}

function render(h){
  var bootCls = (h.bootReason === 'POWERON' || h.bootReason === 'SOFTWARE') ? 'ok' : 'warn';
  if (h.bootReason === 'PANIC' || h.bootReason === 'TASK_WDT' ||
      h.bootReason === 'INT_WDT' || h.bootReason === 'BROWNOUT') bootCls = 'bad';
  var bootSub = 'boot #' + (h.bootCount|0);

  $('sysCards').innerHTML =
    card('Uptime',  fmt(h.uptimeSec), 'ok') +
    card('Core 0',  pctLabel(h.cpuCore0Pct), pctClass(h.cpuCore0Pct)) +
    card('Core 1',  pctLabel(h.cpuCore1Pct), pctClass(h.cpuCore1Pct)) +
    card('RAM Used', pctLabel(h.heapUsedPct), pctClass(h.heapUsedPct),
         (h.heapFree|0).toLocaleString() + ' B free / ' +
         (h.heapTotalBytes|0).toLocaleString() + ' B total') +
    (h.psramTotalBytes > 0
      ? card('PSRAM', pctLabel(h.psramUsedPct), pctClass(h.psramUsedPct),
             (h.psramFreeBytes|0).toLocaleString() + ' B free / ' +
             (h.psramTotalBytes|0).toLocaleString() + ' B total')
      : card('PSRAM', 'n/a', '', 'no PSRAM detected')) +
    card('Heap Low', (h.heapMin|0) + ' B', (h.heapMin|0) < 20000 ? 'bad' : '') +
    card('Heap Max Alloc', (h.heapMaxAlloc|0) + ' B') +
    card('Boot Reason', h.bootReason || '?', bootCls, bootSub);

  var wifiOk = h.wifiConnected;
  var compatState = h.audioCompatState || 'unknown';
  var compatCls = compatState === 'compatible' ? 'ok' :
                  compatState === 'incompatible' ? 'bad' : 'warn';
  var compatSub = 'declared v' + (h.declaredAudioProtocolVersion|0) +
    (compatState === 'compatible' ? ' · negotiated v' + (h.negotiatedAudioProtocolVersion|0)
                                   : ' · ' + (h.audioCompatReason || ''));
  $('netCards').innerHTML =
    card('Wi-Fi', wifiOk ? 'UP' : 'DOWN', wifiOk ? 'ok' : 'bad',
         wifiOk ? ('ch ' + (h.wifiChannel|0) + ' · ' + (h.ip||'')) : '') +
    card('RSSI', wifiOk ? ((h.wifiRssi|0) + ' dBm') : '—',
         wifiOk ? (h.wifiRssi > -70 ? 'ok' : h.wifiRssi > -80 ? 'warn' : 'bad') : '') +
    card('Disconnects', h.wifiDisconnects|0,
         (h.wifiDisconnects|0) === 0 ? 'ok' : 'warn') +
    card('Server', h.serverFound ? (h.serverIp||'found') : 'not found',
         h.serverFound ? 'ok' : 'bad') +
    card('Last Contact', fmtAge(h.lastServerContactAgeMs),
         h.lastServerContactAgeMs < 10000 ? 'ok' :
         h.lastServerContactAgeMs < 60000 ? 'warn' : 'bad') +
    card('Audio Compat', compatState.toUpperCase(), compatCls, compatSub) +
    card('Clock Sync', h.clockSynced ? 'YES' : 'NO',
         h.clockSynced ? 'ok' : 'warn',
         h.clockSynced ? ('offset ' + (h.clockOffsetMs|0) + ' ms · jitter ' +
                           (h.clockJitterMs|0) + ' ms' +
                           (h.clockSyncRttValid ? (' · rtt ' + (h.clockSyncRttMs|0) + ' ms') : ''))
                       : 'waiting for time_sync') +
    card('Clock Resyncs', h.clockResyncs|0,
         (h.clockResyncs|0) <= 1 ? 'ok' : 'warn') +
    card('Server Epoch', h.serverEpochKnown ? (h.serverEpoch >>> 0) : 'unknown',
         h.serverEpochKnown ? 'ok' : 'warn',
         'sync port ' + (h.clockSyncPort|0));

  // ---- Encoder (as received from the server) -------------------------
  // Compact version of the server's Advanced > Encoding tab. The numbers
  // here are what the SERVER told this node its encoder is currently
  // running; if any value disagrees with the server's Encoding tab, the
  // ctrl push / registration response has not reached the node.
  var encSignalName = function(v) {
    if (v === 3001)  return 'voice';
    if (v === 3002)  return 'music';
    if (v === -1000) return 'auto';
    if (v === -1)    return '\u2014';
    return '0x' + (v|0).toString(16);
  };
  var encAppName = function(v) {
    if (v === 2049) return 'audio';
    if (v === 2048) return 'voip';
    if (v === 2051) return 'lowdelay';
    if (v === -1)   return '\u2014';
    return '0x' + (v|0).toString(16);
  };
  var encAge    = h.serverEncSettingsAgeMs;
  var encAgeTxt = (encAge === undefined || encAge === 0xFFFFFFFF) ? 'never'
                : encAge < 1000 ? (encAge + ' ms')
                : encAge < 60000 ? ((encAge/1000).toFixed(1) + ' s')
                : fmtAge(encAge);
  var encAgeCls = (encAge === 0xFFFFFFFF || encAge > 300000) ? 'bad'
                : encAge > 60000 ? 'warn' : 'ok';

  $('encCards').innerHTML =
    card('Bitrate',        (h.serverEncBitrate    > 0 ? ((h.serverEncBitrate|0)/1000).toFixed(0) + ' kbps' : '\u2014'),
                           h.serverEncBitrate > 0 ? 'ok' : 'warn') +
    card('Complexity',     (h.serverEncComplexity >= 0 ? String(h.serverEncComplexity|0) : '\u2014'),
                           h.serverEncComplexity >= 0 ? 'ok' : 'warn') +
    card('VBR',            h.serverEncVbr == null ? '\u2014' : (h.serverEncVbr ? 'On' : 'Off'), 'ok') +
    card('VBR Constraint', h.serverEncVbrConstraint == null ? '\u2014' : (h.serverEncVbrConstraint ? 'On' : 'Off'), 'ok') +
    card('Signal',         encSignalName(h.serverEncSignal),
                           h.serverEncSignal >= 0 ? 'ok' : 'warn') +
    card('Application',    encAppName(h.serverEncApplication),
                           h.serverEncApplication >= 0 ? 'ok' : 'warn') +
    card('Sample Rate',
         h.serverOpusSampleRate === 48000 ? '48 kHz' :
         h.serverOpusSampleRate === 24000 ? '24 kHz' : '\u2014',
         h.serverOpusSampleRate ? 'ok' : 'warn',
         'encoder Fs') +
    card('Source Rate',
         h.serverSourceSampleRate === 44100 ? '44.1 kHz' :
         h.serverSourceSampleRate === 48000 ? '48 kHz' :
         (h.serverSourceSampleRate
            ? ((h.serverSourceSampleRate / 1000).toFixed(1) + ' kHz')
            : '\u2014'),
         h.serverSourceSampleRate ? 'ok' : 'warn',
         'I2S clock at server') +
    card('Server Enc Age', encAgeTxt, encAgeCls, 'last ctrl push / register');

  // ---- Stutter diagnostics -------------------------------------------
  // pkt/s is the packet count of the last full second (steady state ~50).
  // gap is the worst inter-arrival gap in that same second (steady state
  // <25 ms). A one-second burst of loss during the 10-30 s periodic
  // stutter shows as pkt/s dropping to ~30 and gap jumping to 200-500.
  var pktLast = (typeof h.packetsLastSec === 'number') ? h.packetsLastSec : 0;
  var gapLast = (typeof h.maxGapMsLastSec === 'number') ? h.maxGapMsLastSec : 0;
  var pktCls  = pktLast >= 45 ? 'ok' : pktLast >= 30 ? 'warn' : 'bad';
  var gapCls  = gapLast <  30 ? 'ok' : gapLast <  80 ? 'warn' : 'bad';
  // [post-queue-diagnostics] Three additional per-second counters catch a
  // stutter that is downstream of the packet/scheduler layer. All zero in
  // steady state. A spike on the timeline in any of these three identifies
  // the exact second when the pipeline produced an audible artifact.
  var corrAct  = (typeof h.correctorFramesLastSec === 'number') ? h.correctorFramesLastSec : 0;
  var shortDec = (typeof h.shortDecodeFramesLastSec === 'number') ? h.shortDecodeFramesLastSec : 0;
  var lead20   = (typeof h.leadBelow20LastSec === 'number') ? h.leadBelow20LastSec : 0;
  $('stutCards').innerHTML =
    card('Packets / sec',    String(pktLast), pktCls, 'last full second') +
    card('Max Gap (1 s)',    gapLast + ' ms', gapCls, 'worst inter-arrival') +
    card('Corrector frames', String(corrAct),
         corrAct === 0 ? 'ok' : 'warn', 'last second, expect 0') +
    card('Short decodes',    String(shortDec),
         shortDec === 0 ? 'ok' : 'bad', 'last second, expect 0') +
    card('Near-deadline',    String(lead20),
         lead20 === 0 ? 'ok'
           : (pktLast > 0 && lead20 >= Math.max(1, (pktLast * 8) / 10)) ? 'bad'
           : 'warn',
         (pktLast > 0)
           ? (lead20 === 0
                ? 'headroom healthy'
                : Math.round(100.0 * lead20 / pktLast) + '% of frames <20 ms')
           : '<20 ms margin, expect 0') +
    card('Gaps >35 ms',      String(h.gapGt35msTotal|0), 'warn', 'cumulative') +
    card('Gaps >50 ms',      String(h.gapGt50msTotal|0), 'warn', 'cumulative') +
    card('Gaps >100 ms',     String(h.gapGt100msTotal|0), 'bad',  'cumulative');

  var q = h.queueDepth|0;
  var ql = h.queueLen|0;
  var qcls = q === 0 ? 'warn' : (q >= ql ? 'bad' : 'ok');
  $('audCards').innerHTML =
    card('Queue', q + ' / ' + ql, qcls, 'target ' + (h.queueTarget|0)) +
    card('Last Packet', fmtAge(h.lastAudioPacketAgeMs),
         h.lastAudioPacketAgeMs < 200 ? 'ok' :
         h.lastAudioPacketAgeMs < 2000 ? 'warn' : 'bad') +
    card('Packets Rx', (h.packetsReceived|0).toLocaleString(), 'ok') +
    card('Rebuffers', h.rebuffers|0,
         (h.rebuffers|0) === 0 ? 'ok' : 'warn',
         (h.rebuffers|0) > 0 ? ('last ' + fmtAge(h.lastRebufferAgeMs)) : '') +
    card('Dropped', h.packetsDropped|0,
         (h.packetsDropped|0) === 0 ? 'ok' : 'warn') +
    card('Oversize', h.packetsOversize|0,
         (h.packetsOversize|0) === 0 ? 'ok' : 'bad') +
    card('Playout Delay', (h.playoutDelayMs|0) + ' / ' + (h.playoutDelayTargetMs|0) + ' ms') +
    card('Rate Trim', (h.rateTrimHz|0) + ' Hz',
         (h.rateTrimHz|0) === 0 ? 'warn' : 'ok',
         (h.rateTrimHz|0) === 0 ? 'controller idle/disabled' : 'trim active') +
    card('Queue Error', h.queueError|0, Math.abs(h.queueError|0) <= 2 ? 'ok' : 'warn',
         'correction ' + (h.correctionSamples|0) + ' smp') +
    card('Total Correction', (h.totalCorrectionSamples|0).toLocaleString(),
         (h.totalCorrectionSamples|0) === 0 ? 'ok' : 'warn',
         'cumulative samples dropped/added') +
    card('Seq Gaps / Missing', (h.seqGaps|0) + ' / ' + (h.seqMissing|0),
         (h.seqGaps|0) === 0 ? 'ok' : 'warn',
          h.audioProtoV2 ? 'v2 protocol' : 'waiting for audio') +
    card('Reordered / Duplicate', (h.seqReordered|0) + ' / ' + (h.seqDuplicate|0),
         ((h.seqReordered|0) + (h.seqDuplicate|0)) === 0 ? 'ok' : 'warn') +
    card('Concealed (PLC)', h.framesConcealed|0,
         (h.framesConcealed|0) === 0 ? 'ok' : 'warn') +
    card('Late / Discarded', (h.framesLate|0) + ' / ' + (h.framesDiscarded|0),
         (h.framesDiscarded|0) === 0 ? 'ok' : 'warn') +
    card('Late Histogram', [h.framesLateLt5|0, h.framesLate5to20|0,
          h.framesLate20to100|0, h.framesLate100to200|0].join(' / '),
         ((h.framesLate20to100|0) + (h.framesLate100to200|0)) === 0 ? 'ok' : 'bad',
         '<5 / 5-20 / 20-100 / 100-200 ms') +
    card('Network Latency', (h.lastNetworkLatencyMs|0) + ' ms',
         (h.lastNetworkLatencyMs|0) < (h.playoutDelayMs|0) ? 'ok' : 'bad',
         'headroom ' + ((h.playoutHeadroomMs|0) >= 0 ? '+' : '') +
         (h.playoutHeadroomMs|0) + ' ms');

  var errItems = [
    ['Opus decode errors', h.opusDecodeErrors|0],
    ['I2S write errors',   h.i2sWriteErrors|0],
    ['I2S timeouts',       h.i2sTimeouts|0],
    ['POST stats fails',   h.postStatsFails|0],
    ['POST register fails',h.postRegisterFails|0],
    ['Control JSON errors',h.ctrlJsonErrors|0],
    ['Discovery failures', h.discoveryFailures|0],
    ['Multicast join fails', h.multicastJoinFails|0],
    ['Audio withheld (incompatible)', h.audioWithheldIncompatible|0],
  ];
  // [dac-dead-visible] A DAC whose i2s_driver_install()/i2s_set_pin()
  // failed at boot leaves the audio task silent but does NOT increment
  // i2sWriteErrors (process_and_output_audio skips a non-ready DAC
  // entirely via the `if (!g_i2s_dac*_ready) off* = want` guard). The
  // error panel was therefore green while half the output was dead.
  // Push an explicit 1-count entry so the panel flips to red and names
  // the cause. `=== false` so a missing field (older firmware) does not
  // false-positive.
  if (h.i2sDac1Ready === false) errItems.push(['DAC1 failed to initialise', 1]);
  if (h.i2sDac2Ready === false) errItems.push(['DAC2 failed to initialise', 1]);
  var anyError = errItems.some(function(it){ return it[1] > 0; });
  var totalErr = errItems.reduce(function(a,b){ return a + b[1]; }, 0);

  $('errList').innerHTML = errItems.map(function(it){ return errItem(it[0], it[1]); }).join('');
  $('errPanel').className = 'err-panel ' + (anyError ? 'has-errors' : 'clean');
  // [problems-label] The panel mixes cumulative error counters with
  // binary per-DAC readiness flags (see [dac-dead-visible] above), so
  // "Error Counters — N total" misread an `N=1` from a dead DAC as
  // "one error event" when the actual event counters were all zero.
  // "Problems — N total" reads correctly for both kinds of entry.
  $('errHead').textContent = anyError
    ? ('Problems — ' + totalErr + ' total')
    : 'Problems — all clear';

  $('stamp').textContent = 'updated ' + new Date().toLocaleTimeString();
}

function logs(list){
  $('logs').innerHTML = list.map(function(l){
    var cls='l-info';
    if (l.indexOf('ERROR')!==-1 || l.indexOf('FAIL')!==-1 ||
        l.indexOf('failed')!==-1 || l.indexOf('PANIC')!==-1 ||
        l.indexOf('WDT')!==-1) cls='l-err';
    else if (l.indexOf('WARN')!==-1 || l.indexOf('warn')!==-1) cls='l-warn';
    return '<span class="'+cls+'">'+esc(l)+'</span>';
  }).join('\n');
}

function tick(){
  // [status-no-store] Both endpoints are mutable; without no-store a
  // browser can serve a stale /api/health or /api/logs response and the
  // dashboard shows frozen counters. The dashboard page already does
  // this for /api/health (see pollHealth) but the status page did not.
  fetch('/api/health', { cache: 'no-store' }).then(function(r){return r.json()}).then(function(h){
    render(h);
    var el = document.getElementById('rawHealth');
    if (el) el.textContent = JSON.stringify(h, null, 2);
  }).catch(function(){});
  fetch('/api/logs', { cache: 'no-store' }).then(function(r){return r.json()}).then(logs).catch(function(){});
}

/* ---- Start / Clear gating ---------------------------------------------
   Nothing runs on this page until Start is pressed. The page previously
   called tick() at load and polled forever, which meant every open of
   /status quietly hit /api/health and /api/logs twice per second. Now the
   page sits idle. Clear issues a soft-reset to the node (counters zeroed,
   event timestamps re-anchored to now) and wipes the client view so the
   next poll shows a fresh measurement window. */
var polling    = false;
var pollTimer  = null;
var startBtn   = document.getElementById('startBtn');
var clearBtn   = document.getElementById('clearBtn');
var ctrlState  = document.getElementById('ctrlState');

function startPolling(){
  if (polling) return;
  polling = true;
  startBtn.textContent = 'Stop';
  startBtn.classList.add('active');
  ctrlState.textContent = 'polling';
  tick();
  pollTimer = setInterval(tick, 2000);
}

function stopPolling(){
  if (!polling) return;
  polling = false;
  startBtn.textContent = 'Start';
  startBtn.classList.remove('active');
  ctrlState.textContent = 'stopped';
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

startBtn.addEventListener('click', function(){
  if (polling) stopPolling(); else startPolling();
});

clearBtn.addEventListener('click', function(){
  fetch('/api/stats/reset', { method:'POST' })
    .catch(function(){ /* node may be unreachable; still wipe the view */ })
    .then(function(){
      // Wipe the client-side view so the fresh counters are visible on the
      // next poll instead of being blended with the previous window.
      ['sysCards','netCards','audCards','errList'].forEach(function(id){
        var el = document.getElementById(id); if (el) el.innerHTML = '';
      });
      // [problems-label-clear] Matches the "Problems" text set by
      // render() (see [problems-label] in the STATUS page's render()).
      // This handler used to write 'Error Counters', which is the OLD
      // label; after a Clear the header reverted to it for one poll
      // cycle before render() put "Problems" back. Operators who clicked
      // Clear right before navigating away saw the wrong label.
      document.getElementById('errHead').textContent = 'Problems';
      document.getElementById('errPanel').className = 'err-panel clean';
      document.getElementById('logs').textContent = '(cleared)';
      document.getElementById('rawHealth').textContent = '(cleared)';
      document.getElementById('stamp').textContent =
        'cleared at ' + new Date().toLocaleTimeString();
      // If a poll was in flight when Clear was pressed, we may get one
      // last response with the old counters after the reset call lands.
      // The next 2 s tick will show the reset values, which is fine.
    });
});

/* ---- 60 s diagnostic recorder -------------------------------------- */
var recActive    = false;
var recSamples   = [];
var recAllLogs   = [];
var recStartTime = 0;
var recLastTail  = null;
// [recorder-10min] Was 60000 (60 s). 10 minutes is enough to catch a
// periodic 10-30 s stutter several times over; Finish Now ends the run
// early and builds the report from whatever was captured.
var REC_DURATION_MS  = 600000;
var REC_FINISHED_EARLY = false;

function recPad(v, w){
  var s = String(v);
  while (s.length < w) s = ' ' + s;
  return s;
}

function recUiShowProgress(){
  document.getElementById('recProgressOverlay').style.display = 'flex';
  document.getElementById('recProgressFill').style.width = '0%';
  document.getElementById('recProgressText').textContent = '0.0s / 600s';
  var btn = document.getElementById('recFinishBtn');
  if (btn) { btn.disabled = false; btn.textContent = 'Finish Now'; }
}
function recUiHideProgress(){
  document.getElementById('recProgressOverlay').style.display = 'none';
}

function fetchWithTimeout(url, timeoutMs) {
  return new Promise(function(resolve, reject) {
    var timer = setTimeout(function() { reject(new Error('timeout')); }, timeoutMs);
    fetch(url, { cache: 'no-store' }).then(
      function(res) { clearTimeout(timer); resolve(res); },
      function(err) { clearTimeout(timer); reject(err); }
    );
  });
}

async function recTick(){
  if (!recActive) return;
  var DUR = REC_DURATION_MS;
  var elapsed = Date.now() - recStartTime;
  var t = elapsed / 1000.0;
  if (elapsed >= DUR) { recFinish(false); return; }
  // [wall-timestamp] Capture wall time NOW, before the /api/health and
  // /api/logs fetches. Each can take up to 2.5 s on a slow node, so the
  // old code (which read wall after both fetches) drifted up to ~5 s
  // relative to t. Move it here so the recorder's t and wall columns
  // describe the same instant.
  var wall = new Date().toISOString().substring(11, 23);

  document.getElementById('recProgressFill').style.width = (elapsed / DUR * 100) + '%';
  document.getElementById('recProgressText').textContent =
    t.toFixed(1) + 's / ' + (DUR/1000) + 's — ' + recSamples.length + ' samples';

  var health = null, newLogs = [];

  try {
    var hr = await fetchWithTimeout('/api/health', 2500);
    if (hr.ok) health = await hr.json();
  } catch (e) {}
  
  try {
    var lr = await fetchWithTimeout('/api/logs', 2500);
    if (lr.ok) {
      var logs = await lr.json();
      if (Array.isArray(logs) && logs.length) {
        var idx = -1;
        if (recLastTail !== null) {
          for (var j = logs.length - 1; j >= 0; j--) {
            if (logs[j] === recLastTail) { idx = j; break; }
          }
        }
        var startAt = (idx >= 0) ? idx + 1 : Math.max(0, logs.length - 10);
        for (var k = startAt; k < logs.length; k++) newLogs.push(logs[k]);
        if (logs.length) recLastTail = logs[logs.length - 1];
      }
    }
  } catch (e) {}

  for (var li = 0; li < newLogs.length; li++) recAllLogs.push(newLogs[li]);

  recSamples.push({ t: t, wall: wall, health: health, newLogs: newLogs });

  var now = Date.now();
  var elapsedNext = now - recStartTime;
  if (elapsedNext >= DUR) {
    recFinish(false);
  } else {
    var nextDelay = 1000 - (elapsedNext % 1000);
    setTimeout(recTick, nextDelay);
  }
}

function recStart(){
  if (recActive) return;
  if (polling) stopPolling();
  recActive         = true;
  REC_FINISHED_EARLY = false;
  recSamples        = [];
  recAllLogs        = [];
  recStartTime      = Date.now();
  recLastTail       = null;
  document.getElementById('recordBtn').disabled = true;
  recUiShowProgress();
  // [recorder-auto-clear]
  // Force this node's counters to a known baseline before the first
  // /api/health snapshot. Fire-and-forget: if the POST fails (rare on a
  // healthy LAN — 500 ms timeout on the server side, and the node's own
  // handler is pure counter assignment with no I/O) the recording still
  // captures the pre-reset state in its first sample. The Summary block
  // uses the LAST sample, which will always be post-reset because the
  // recorder runs for 60-600 s and the reset lands in the first 100 ms.
  //
  // NOTE: this only clears THIS node. For a fleet-wide reset, the
  // operator must either hit Clear on every node's Status page, or
  // power-cycle the server (which fires handleServerEpoch on every
  // node — see [epoch-resets-timeline-integrity] in EVDPNet.h for
  // what that resets).
  fetch('/api/stats/reset', { method:'POST' }).catch(function(){});
  recTick();
}

// [recorder-finish-early]
// early=true when called from the Finish Now button: stops the timer and
// builds the report from whatever has been captured so far. early=false
// when the 10-minute timer elapses on its own. Either way the report,
// copy, and download buttons behave identically. A double-finish (auto
// timer fires while the user is mid-click on Finish Now) is a no-op
// because recActive is cleared before the 300 ms reveal.
function recFinish(early){
  if (!recActive) return;
  recActive = false;
  REC_FINISHED_EARLY = !!early;
  if (early) {
    document.getElementById('recProgressText').textContent =
      'Finished early — building report…';
  } else {
    document.getElementById('recProgressFill').style.width = '100%';
  }
  var btn = document.getElementById('recFinishBtn');
  if (btn) { btn.disabled = true; btn.textContent = 'Finishing…'; }
  setTimeout(function(){
    recUiHideProgress();
    document.getElementById('recResultText').value = recBuildReport();
    document.getElementById('recResultOverlay').style.display = 'flex';
    document.getElementById('recordBtn').disabled = false;
  }, 300);
}

function recBuildReport(){
  var lines = [];
  var first = recSamples.length ? recSamples[0] : null;
  var last  = recSamples.length ? recSamples[recSamples.length - 1] : null;
  var lastH = last ? last.health : null;
  var dur   = recSamples.length ? (recSamples[recSamples.length - 1].t - recSamples[0].t) : 0;
  var interval = recSamples.length > 1 ? (dur / (recSamples.length - 1)) : 0;

  lines.push('=== Enkelvoud Node Diagnostic Recording ===');
  if (lastH) {
    lines.push('Boot reason: ' + (lastH.bootReason || '?'));
    lines.push('Boot count:  ' + (lastH.bootCount | 0));
    lines.push('Uptime:      ' + (lastH.uptimeSec | 0) + ' s');
    lines.push('Firmware:    v' + (lastH.version || '?'));
  }
  lines.push('Started:    ' + (first ? first.wall : ''));
  lines.push('Duration:   ' + dur.toFixed(1) + ' s (nominal ' +
             (REC_DURATION_MS/1000).toFixed(0) + '.0 s)' +
             (REC_FINISHED_EARLY ? ' — finished early by user' : ''));
  lines.push('Interval:   ' + interval.toFixed(3) + ' s (nominal 1.000 s)');
  lines.push('Samples:    ' + recSamples.length);
  lines.push('');

  lines.push('=== Column Legend ===');
  lines.push('t      = seconds from start');
  lines.push('c0/c1  = CPU core 0/1 %');
  lines.push('ram/ps = heap/PSRAM used %');
  lines.push('q      = audio queue depth');
  lines.push('lat    = last network latency ms');
  lines.push('rate   = resampler rate-trim Hz');
  lines.push('pkt    = packets received in the last full second (steady state ~50)');
  lines.push('gap    = worst inter-arrival gap in that second (steady state <25 ms)');
  lines.push('crr    = frames in the last second where the corrector modified samples');
  lines.push('shrt   = frames in the last second that decoded short');
  lines.push('ld20   = frames in the last second with <20 ms of deadline margin');
  lines.push('corr   = corrector sample value on the last frame (instantaneous)');
  lines.push('l20    = cumulative frames late 20-100 ms');
  lines.push('disc   = cumulative frames discarded');
  lines.push('conc   = cumulative frames concealed (PLC)');
  lines.push('rbuf   = cumulative rebuffers');
  lines.push('rtt    = best clock-sync RTT ms');
  lines.push('off    = server clock offset ms');
  lines.push('head   = playout headroom ms');
  lines.push('sync   = clock synced (Y/n)');
  lines.push('net    = C<chan>/<rssi> when connected, DOWN otherwise');
  lines.push('');

  lines.push('=== Timeline ===');
  // [recorder-flag-column] One-character-per-cause marker so a stutter
  // second pops out without scanning every column. Empty FLAG = clean
  // second. Key (also printed under the summary):
  //   C=corrector  S=shortDecode  L=late20-100  Z=late100-200
  //   D=discarded  X=PLC          R=rebuffer    G=gap>100ms
  //   P=pkt/s<40   N=nearDeadline(>5)
  lines.push('t     | wall         | c0  | c1  | ram | ps  | q   | pkt | gap | crr | shrt| ld20| corr | lat  | rate  | FLAG | l20    | disc | conc | rbuf | rtt  | off    | head   | sync | net');
  lines.push('-'.repeat(220));

  // [recorder-delta-tracking] Cumulative counters (framesLate*,
  // framesDiscarded, framesConcealed, rebuffers) must be diffed against
  // the previous sample to detect "something happened THIS second".
  // Per-second counters (corrector, shortDecode, leadBelow20, packets,
  // gap) are already deltas from the node's own 1 s rollover.
  var prevLate20 = null, prevLate100 = null, prevDisc = null,
      prevConc = null,   prevRbuf = null;

  // [recorder-event-log] Collected during the timeline walk, printed as a
  // dedicated summary BEFORE the per-sample wall of green so the operator
  // does not have to scroll to find the second that glitched.
  var stutterEvents = [];

  for (var i = 0; i < recSamples.length; i++) {
    var s = recSamples[i];
    var h = s.health || {};
    var c0 = (typeof h.cpuCore0Pct === 'number' && h.cpuCore0Pct >= 0) ? h.cpuCore0Pct : '-';
    var c1 = (typeof h.cpuCore1Pct === 'number' && h.cpuCore1Pct >= 0) ? h.cpuCore1Pct : '-';
    var q  = (typeof h.queueDepth === 'number') ? h.queueDepth : '-';
    var lat = (typeof h.lastNetworkLatencyMs === 'number') ? h.lastNetworkLatencyMs : '-';
    var rate = (typeof h.rateTrimHz === 'number') ? h.rateTrimHz : '-';
    var rtt = (h.clockSyncRttValid && typeof h.clockSyncRttMs === 'number') ? h.clockSyncRttMs : '-';
    var off = (typeof h.clockOffsetMs === 'number') ? h.clockOffsetMs : '-';
    var hr  = (typeof h.playoutHeadroomMs === 'number') ? h.playoutHeadroomMs : '-';
    var sync = h.clockSynced ? 'Y' : 'n';
    var net = h.wifiConnected
                ? ('C' + (h.wifiChannel|0) + '/' + (h.wifiRssi|0))
                : 'DOWN';
    var ps = (h.psramTotalBytes > 0) ? (h.psramUsedPct|0) : '-';

    var pktNow  = (typeof h.packetsLastSec === 'number') ? h.packetsLastSec : -1;
    var gapNow  = (typeof h.maxGapMsLastSec === 'number') ? h.maxGapMsLastSec : -1;
    var crrNow  = (typeof h.correctorFramesLastSec === 'number') ? h.correctorFramesLastSec : -1;
    var shrtNow = (typeof h.shortDecodeFramesLastSec === 'number') ? h.shortDecodeFramesLastSec : -1;
    var ld20Now = (typeof h.leadBelow20LastSec === 'number') ? h.leadBelow20LastSec : -1;
    var corrNow = (typeof h.correctionSamples === 'number') ? h.correctionSamples : -1;

    var curLate20  = h.framesLate20to100|0;
    var curLate100 = h.framesLate100to200|0;
    var curDisc    = h.framesDiscarded|0;
    var curConc    = h.framesConcealed|0;
    var curRbuf    = h.rebuffers|0;

    var dLate20  = (prevLate20  === null) ? 0 : (curLate20  - prevLate20);
    var dLate100 = (prevLate100 === null) ? 0 : (curLate100 - prevLate100);
    var dDisc    = (prevDisc    === null) ? 0 : (curDisc    - prevDisc);
    var dConc    = (prevConc    === null) ? 0 : (curConc    - prevConc);
    var dRbuf    = (prevRbuf    === null) ? 0 : (curRbuf    - prevRbuf);
    prevLate20  = curLate20;  prevLate100 = curLate100;
    prevDisc    = curDisc;    prevConc    = curConc;  prevRbuf = curRbuf;

    // --- build flag string for this row ---
    var flags = '';
    if (crrNow   > 0)   flags += 'C';
    if (shrtNow  > 0)   flags += 'S';
    if (dLate20  > 0)   flags += 'L';
    if (dLate100 > 0)   flags += 'Z';
    if (dDisc    > 0)   flags += 'D';
    if (dConc    > 0)   flags += 'X';
    if (dRbuf    > 0)   flags += 'R';
    if (gapNow  > 100)  flags += 'G';
    if (pktNow >= 0 && pktNow < 40) flags += 'P';
    if (ld20Now  > 5)   flags += 'N';

    // --- collect the event entry if the flag set is non-empty ---
    if (flags.length > 0) {
      var causes = [];
      if (crrNow   > 0)   causes.push('corrector ' + crrNow + 'f');
      if (shrtNow  > 0)   causes.push('shortDecode ' + shrtNow + 'f');
      if (dLate20  > 0)   causes.push('late20-100 +' + dLate20);
      if (dLate100 > 0)   causes.push('late100-200 +' + dLate100);
      if (dDisc    > 0)   causes.push('discarded +' + dDisc);
      if (dConc    > 0)   causes.push('PLC +' + dConc);
      if (dRbuf    > 0)   causes.push('REBUFFER +' + dRbuf);
      if (gapNow  > 100)  causes.push('gap ' + gapNow + 'ms');
      if (pktNow >= 0 && pktNow < 40) causes.push('pkt/s=' + pktNow);
      if (ld20Now  > 5)   causes.push('nearDeadline ' + ld20Now + 'f');
      stutterEvents.push({
        t: s.t, wall: s.wall, flags: flags, causes: causes.join(', ')
      });
    }

    lines.push(
      recPad(s.t.toFixed(1), 5) + ' | ' +
      (s.wall || '').padEnd(12) + ' | ' +
      recPad(c0, 3) + ' | ' +
      recPad(c1, 3) + ' | ' +
      recPad(h.heapUsedPct|0, 3) + ' | ' +
      recPad(ps, 3) + ' | ' +
      recPad(q, 3) + ' | ' +
      recPad(pktNow  >= 0 ? pktNow  : '-', 3) + ' | ' +
      recPad(gapNow  >= 0 ? gapNow  : '-', 3) + ' | ' +
      recPad(crrNow  >= 0 ? crrNow  : '-', 3) + ' | ' +
      recPad(shrtNow >= 0 ? shrtNow : '-', 4) + ' | ' +
      recPad(ld20Now >= 0 ? ld20Now : '-', 4) + ' | ' +
      recPad(corrNow, 4) + ' | ' +
      recPad(lat, 4) + ' | ' +
      recPad(rate, 5) + ' | ' +
      recPad(flags, 4) + ' | ' +
      recPad(curLate20, 6) + ' | ' +
      recPad(curDisc, 4) + ' | ' +
      recPad(curConc, 4) + ' | ' +
      recPad(curRbuf, 4) + ' | ' +
      recPad(rtt, 4) + ' | ' +
      recPad(off, 6) + ' | ' +
      recPad(hr, 6) + ' | ' +
      sync.padEnd(4) + ' | ' + net
    );
  }
  lines.push('');

  // [recorder-stutter-summary] Printed BEFORE the per-sample wall of
  // green so the operator sees "here is what happened and when" without
  // scrolling. Empty list = the recording was clean and any stutter the
  // operator heard was NOT in any path these counters measure.
  lines.push('=== STUTTER EVENTS ===');
  if (stutterEvents.length === 0) {
    lines.push('NONE — every second of the capture had:');
    lines.push('  corrector idle, no short decodes, no discards, no PLC,');
    lines.push('  no rebuffers, no packet gaps > 100 ms, pkt/s >= 40,');
    lines.push('  no near-deadline storms.');
    lines.push('If the operator heard a stutter it was NOT in the audio');
    lines.push('pipeline counters. Check the Log Lines section for any');
    lines.push('node-side breadcrumbs (STUTTER, Flush+rebuffer, Starved).');
  } else {
    lines.push('Detected ' + stutterEvents.length +
               ' second(s) with anomalous audio activity:');
    lines.push('');
    lines.push('  t (s) | wall         | flags | cause');
    lines.push('  ------+--------------+-------+------------------------------------------');
    for (var ei = 0; ei < stutterEvents.length; ei++) {
      var ev = stutterEvents[ei];
      lines.push(
        '  ' + recPad(ev.t.toFixed(1), 5) + ' | ' +
        (ev.wall || '').padEnd(12) + ' | ' +
        recPad(ev.flags, 5) + ' | ' +
        ev.causes
      );
    }
    lines.push('');
    lines.push('Flag key: C=corrector  S=shortDecode  L=late20-100  Z=late100-200');
    lines.push('          D=discarded  X=PLC          R=rebuffer    G=gap>100ms');
    lines.push('          P=pkt/s<40   N=nearDeadline(>5)');
  }
  lines.push('');

  lines.push('=== Summary (last sample) ===');
  if (lastH) {
    lines.push('WiFi:               ' + (lastH.wifiConnected ? 'UP' : 'DOWN') +
               ' ch ' + (lastH.wifiChannel|0) + ' RSSI ' + (lastH.wifiRssi|0) + ' dBm');
    lines.push('Disconnects:        ' + (lastH.wifiDisconnects|0));
    lines.push('Server:             ' + (lastH.serverFound ? lastH.serverIp : 'NOT FOUND'));
    lines.push('Last server contact: ' + ((lastH.lastServerContactAgeMs === 0xFFFFFFFF || lastH.lastServerContactAgeMs == null) ? 'never' : (lastH.lastServerContactAgeMs|0) + ' ms ago'));
    lines.push('Audio compat:       ' + (lastH.audioCompatState || '?') +
               ' (declared v' + (lastH.declaredAudioProtocolVersion|0) +
               ', negotiated v' + (lastH.negotiatedAudioProtocolVersion|0) +
               ', reason: ' + (lastH.audioCompatReason || '') + ')');
    lines.push('Clock sync:         ' + (lastH.clockSynced ? 'YES' : 'NO') +
               ' offset=' + (lastH.clockOffsetMs|0) + 'ms' +
               ' jitter=' + (lastH.clockJitterMs|0) + 'ms' +
               ' resyncs=' + (lastH.clockResyncs|0) +
               ' port=' + (lastH.clockSyncPort|0));
    lines.push('Playout:            current=' + (lastH.playoutDelayMs|0) + 'ms' +
               ' target=' + (lastH.playoutDelayTargetMs|0) + 'ms' +
               ' headroom=' + (lastH.playoutHeadroomMs|0) + 'ms');
    lines.push('Queue:              ' + (lastH.queueDepth|0) + '/' + (lastH.queueLen|0) +
               ' (target ' + (lastH.queueTarget|0) + ', err ' + (lastH.queueError|0) + ')');

    // [encoder-info-in-recording]
    // The encoder fields the server mirrors into every registration response
    // and ctrl push. A change here mid-recording means the server applied a
    // new setting while the capture was running, which is the first place to
    // look if an "issue on the received encoded stream" correlates with a
    // quality or timing change. A -1 or '—' means the server never declared
    // the field (older server build).
    (function(){
  var encSignalName = function(v) {
    if (v === 3001)  return 'voice';
    if (v === 3002)  return 'music';
    if (v === -1000) return 'auto';
    if (v === -1)    return '\u2014';
    return '0x' + (v|0).toString(16);
  };
      var encAppName = function(v) {
        if (v === 2049) return 'audio';
        if (v === 2048) return 'voip';
        if (v === 2051) return 'lowdelay';
        if (v === -1)   return '\u2014';
        return '0x' + (v|0).toString(16);
      };
      var encAge    = lastH.serverEncSettingsAgeMs;
      var encAgeTxt = (encAge === undefined || encAge === 0xFFFFFFFF) ? 'never'
                    : encAge < 1000 ? (encAge + ' ms')
                    : encAge < 60000 ? ((encAge/1000).toFixed(1) + ' s')
                    : fmt(Math.floor(encAge/1000));
      lines.push('Encoder (server):   bitrate=' +
                 (lastH.serverEncBitrate > 0 ? ((lastH.serverEncBitrate|0)/1000).toFixed(0) + ' kbps' : '\u2014') +
                 ' complexity=' + (lastH.serverEncComplexity >= 0 ? (lastH.serverEncComplexity|0) : '\u2014') +
                 ' vbr=' + (lastH.serverEncVbr == null ? '\u2014' : (lastH.serverEncVbr ? 'on' : 'off')) +
                 ' vbrC=' + (lastH.serverEncVbrConstraint == null ? '\u2014' : (lastH.serverEncVbrConstraint ? 'on' : 'off')) +
                 ' signal=' + encSignalName(lastH.serverEncSignal) +
                 ' app=' + encAppName(lastH.serverEncApplication));
      lines.push('Encoder age:        ' + encAgeTxt + ' (last ctrl push / register)');
    })();
    lines.push('Packets:            rx=' + (lastH.packetsReceived|0) +
               ' dropped=' + (lastH.packetsDropped|0) +
               ' oversize=' + (lastH.packetsOversize|0));
    lines.push('Sequence:           gaps=' + (lastH.seqGaps|0) +
               ' missing=' + (lastH.seqMissing|0) +
               ' reordered=' + (lastH.seqReordered|0) +
               ' dup=' + (lastH.seqDuplicate|0));
    lines.push('Frames:             late=' + (lastH.framesLate|0) +
               ' [' + (lastH.framesLateLt5|0) + '/' + (lastH.framesLate5to20|0) + '/' +
                     (lastH.framesLate20to100|0) + '/' + (lastH.framesLate100to200|0) + ']' +
               ' concealed=' + (lastH.framesConcealed|0) +
               ' discarded=' + (lastH.framesDiscarded|0));
    lines.push('Rebuffers:          ' + (lastH.rebuffers|0) +
               ' (last ' + ((lastH.lastRebufferAgeMs === 0xFFFFFFFF || lastH.lastRebufferAgeMs == null) ? 'never' : (lastH.lastRebufferAgeMs|0) + ' ms ago') + ')');
    lines.push('Correction:         last=' + (lastH.correctionSamples|0) +
               ' total=' + (lastH.totalCorrectionSamples|0) +
               ' rateTrim=' + (lastH.rateTrimHz|0) + ' Hz');
    lines.push('Errors:             opusDecode=' + (lastH.opusDecodeErrors|0) +
               ' i2sWrite=' + (lastH.i2sWriteErrors|0) +
               ' i2sTimeout=' + (lastH.i2sTimeouts|0));
    lines.push('                    postStats=' + (lastH.postStatsFails|0) +
               ' postRegister=' + (lastH.postRegisterFails|0) +
               ' ctrlJson=' + (lastH.ctrlJsonErrors|0) +
               ' ctrlRx=' + (lastH.ctrlPacketsRx|0));
    lines.push('                    discovery=' + (lastH.discoveryFailures|0) +
               ' mcastJoin=' + (lastH.multicastJoinFails|0) +
               ' withheldIncompat=' + (lastH.audioWithheldIncompatible|0));
    lines.push('Heap:               free=' + (lastH.heapFree|0) +
               ' min=' + (lastH.heapMin|0) +
               ' maxAlloc=' + (lastH.heapMaxAlloc|0) +
               ' used=' + (lastH.heapUsedPct|0) + '%');
    if (lastH.psramTotalBytes > 0) {
      lines.push('PSRAM:              free=' + (lastH.psramFreeBytes|0) +
                 ' total=' + (lastH.psramTotalBytes|0) +
                 ' used=' + (lastH.psramUsedPct|0) + '%');
    }
  } else {
    lines.push('(no successful /api/health response during window)');
  }
  lines.push('');

  lines.push('=== Log Lines (new during window) ===');
  if (!recAllLogs.length) {
    lines.push('(no log lines captured)');
  } else {
    for (var li = 0; li < recAllLogs.length; li++) lines.push(recAllLogs[li]);
  }
  lines.push('');

  lines.push('=== Last /api/health Snapshot ===');
  if (lastH) lines.push(JSON.stringify(lastH, null, 2));

  return lines.join('\n');
}

function recCopy(){
  var ta = document.getElementById('recResultText');
  ta.select();
  try { document.execCommand('copy'); } catch (e) {}
}

function recDownload(){
  var ta = document.getElementById('recResultText');
  var blob = new Blob([ta.value], { type: 'text/plain' });
  var url = URL.createObjectURL(blob);
  var a = document.createElement('a');
  a.href = url;
  a.download = 'enkelvoud-node-diag-' + new Date().toISOString().replace(/[:.]/g, '-') + '.txt';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  setTimeout(function(){ URL.revokeObjectURL(url); }, 1000);
}

document.getElementById('recordBtn').addEventListener('click', recStart);
// [recorder-finish-early] Wire up the Finish Now button added to the
// progress overlay. Calling recFinish(true) short-circuits the 10-minute
// timer and immediately builds the report from the samples captured so
// far; the user can then Copy or Download normally.
(function(){
  var btn = document.getElementById('recFinishBtn');
  if (btn) btn.addEventListener('click', function(){ recFinish(true); });
})();
// Deliberately no auto-start: the page sits idle until the user presses Start.
ctrlState.textContent = 'stopped';
</script>
</body>
</html>
)EVDS";

#endif