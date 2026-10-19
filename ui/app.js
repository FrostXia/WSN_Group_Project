/* Same-origin API: backend serves this page. No hard-coded node list. */
const $ = s => document.querySelector(s);
const METRICS = {
  temperature: {label:"Temperature",unit:"°C"}, humidity:{label:"Humidity",unit:"%"},
  light:{label:"Illuminance",unit:"lux"}, pir:{label:"Motion (PIR)",unit:"",presence:true},
  mq2_voltage:{label:"MQ-2 response",unit:"V"}, dust_voltage:{label:"Estimated dust concentration",unit:"µg/m³"},
  mmwave:{label:"Presence (mmWave)",unit:"",presence:true}
};
const manualSensors={pir:"PIR",mq2_voltage:"MQ-2",dust_voltage:"Dust sensor"};
const savingSensors=new Set(), controlErrors=new Map(), openControls=new Set();
let nodes=[], latest={}, networks=[], history=[], selected=null, connected=false, busy=false;
let settings={offline_seconds:120,stale_seconds:120,alerts_enabled:false,thresholds:{}};
let historyTruncated=false, trendUnavailable=false, historyGeneration=0;
const esc = s => String(s ?? "").replace(/[&<>"']/g, c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const num = v => typeof v==="number" && Number.isFinite(v) ? v.toLocaleString(undefined,{maximumFractionDigits:2}):"—";
const clock = t => t && Number.isFinite(Date.parse(t)) ? new Date(t).toLocaleTimeString():"—";
const seconds = t => t ? Math.max(0,(Date.now()-Date.parse(t))/1000):Infinity;
const fresh = s => seconds(s.recorded_at)<=settings.stale_seconds;
const readings = id => latest[id]||[];
const online = n => connected && seconds(n.last_seen)<=settings.offline_seconds;
function alert(s) {
  if(!METRICS[s.sensor_type]) return false;
  const rule=settings.thresholds[s.sensor_type];
  return settings.alerts_enabled && s.quality==="valid" && fresh(s) && rule &&
    (rule.direction==="high" ? s.value>rule.value:s.value<rule.value);
}
// Stored dust_voltage remains sensor-side volts; convert only for presentation.
// Nominal supplier parameters, not calibrated and not a PM2.5 measurement.
function shownValue(s) {
  if(!s || s.quality!=="valid" || !Number.isFinite(s.value)) return null;
  return s.sensor_type==="dust_voltage" ? Math.max(0,(s.value-0.9)*200) : s.value;
}
function display(s,spec) {
  if(!s || s.quality!=="valid" || !Number.isFinite(s.value)) return "NoData";
  if(spec===METRICS.mmwave) return s.value===1?"Present":"Absent";
  return spec.presence?(s.value===1?"Motion":"No motion"):num(shownValue(s));
}
function metricKeys(id) {
  const node=nodes.find(n=>n.id===id);
  return node && online(node) ? readings(id).filter(s=>METRICS[s.sensor_type] &&
    s.quality==="valid" && Number.isFinite(s.value) && fresh(s) &&
    (!(s.sensor_type in manualSensors) || node.sensors?.[s.sensor_type]))
    .map(s=>s.sensor_type) : [];
}
async function get(path) {
  const res=await fetch(path,{signal:AbortSignal.timeout(5000),cache:"no-store"});
  if(!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}
async function loadHistory() {
  const generation=++historyGeneration, id=selected;
  if(!id){history=[];return;}
  try {
    const res=await get(`/api/history?node_id=${encodeURIComponent(id)}&hours=1&limit=5000`);
    if(generation!==historyGeneration || id!==selected) return;
    history=res.data; historyTruncated=res.truncated; trendUnavailable=false;
  } catch(e) {
    if(generation!==historyGeneration || id!==selected) return;
    history=[]; trendUnavailable=true;
  }
}
async function refresh() {
  if(busy)return;
  busy=true; $("#refreshButton").disabled=true;
  try {
    const data=await get("/api/snapshot");
    nodes=data.nodes; latest=data.latest; networks=data.network; settings=data.settings; connected=true;
    if(!nodes.some(n=>n.id===selected)) selected=nodes[0]?.id||null;
    render(); await loadHistory();
  } catch(e) { connected=false; console.info(e.message); }
  finally {busy=false;$("#refreshButton").disabled=false;render();}
}
function renderNodes() {
  const grid=$("#nodeGrid"); grid.replaceChildren();
  if(!nodes.length) {
    grid.innerHTML='<p class="empty-alert">No nodes registered. Start the gateway with the root board connected. Sensor-disabled nodes are supported.</p>';
    return;
  }
  for(const node of nodes) {
    const list=readings(node.id), active=online(node), warnings=list.some(alert);
    const usable=metricKeys(node.id).length>0;
    const fragment=$("#nodeTemplate").content.cloneNode(true), card=fragment.querySelector("article");
    fragment.querySelector(".room-label").textContent=node.room;
    fragment.querySelector("h3").textContent=node.label;
    const state=fragment.querySelector(".node-state");
    state.textContent=!connected?"Status unknown":!active?"No recent packets":warnings?"Attention":!usable?"Online · NoData":"Online";
    state.className=`node-state ${!active?"offline":warnings?"warning":""}`;
    card.classList.toggle("offline",!active);card.classList.toggle("warning",active&&(warnings));
    fragment.querySelector(".reading-grid").innerHTML=metricKeys(node.id).map(k=>{
      const spec=METRICS[k],s=list.find(v=>v.sensor_type===k);
      const label=s&&!fresh(s)?" · Stale":"";
      return `<div class="metric-reading ${!s?"no-reading":""}"><span class="reading-label">${esc(spec.label+label)}</span><span class="reading-value ${s&&alert(s)?"alert":""}">${esc(display(s,spec))}<small>${s&&s.quality==="valid"?esc(spec.unit):""}</small></span></div>`;
    }).join("");
    if(!metricKeys(node.id).length) fragment.querySelector(".reading-grid").innerHTML='<p class="empty-alert">NoData</p>';
    const controls=document.createElement("details");
    controls.className="sensor-controls"; controls.open=openControls.has(node.id);
    controls.ontoggle=()=>controls.open?openControls.add(node.id):openControls.delete(node.id);
    const summary=document.createElement("summary"); summary.textContent="Sensor settings"; controls.append(summary);
    for(const [kind,label] of Object.entries(manualSensors)) {
      const row=document.createElement("label"), input=document.createElement("input"), name=document.createElement("span");
      input.type="checkbox"; input.checked=!!node.sensors?.[kind];
      input.disabled=!connected || savingSensors.has(node.id);
      name.textContent=label; row.append(input); row.append(name); controls.append(row);
      input.onchange=()=>setSensor(node.id,kind,input.checked);
    }
    const help=document.createElement("p");
    help.textContent=controlErrors.get(node.id)||"Enable after connecting; disable after unplugging. These switches do not detect disconnection.";
    help.setAttribute("role",controlErrors.has(node.id)?"alert":"note"); controls.append(help);
    fragment.querySelector(".reading-grid").after(controls);
    const net=networks.find(v=>v.node_id===node.id);
    const info=document.createElement("p"); info.className="route-summary";
    info.textContent=`Last received ${clock(node.last_seen)} · Path: ${net?.path?.join(" → ")||"Unknown"}`;
    fragment.querySelector(".reading-grid").after(info);
    fragment.querySelector(".last-seen").textContent=usable?"Gateway time; sampling time estimated":"No fresh valid sensor readings";
    fragment.querySelector(".focus-node").onclick=()=>{selectNode(node.id);$("#trend").scrollIntoView({behavior:"smooth"});};
    grid.append(fragment);
  }
}
function trendSvg(points,key) {
  points=points.map(p=>({...p,value:shownValue(p)}));
  if(!points.length)return '<p class="empty-alert">No historical readings</p>';
  const good=points.filter(p=>p.quality==="valid"&&Number.isFinite(p.value));
  if(!good.length)return '<p class="empty-alert">No valid historical readings</p>';
  const w=420,h=160,left=12,right=408,top=18,bottom=130;
  let lo=Math.min(...good.map(p=>p.value)),hi=Math.max(...good.map(p=>p.value));
  const margin=Math.max((hi-lo)*.2,Math.abs(hi)*.01,.5);lo-=margin;hi+=margin;
  const t0=Date.parse(points[0].recorded_at),t1=Date.parse(points.at(-1).recorded_at);
  const x=p=>left+(Date.parse(p.recorded_at)-t0)/Math.max(1,t1-t0)*(right-left);
  const y=p=>bottom-(p.value-lo)/(hi-lo)*(bottom-top);
  let path="",pen=false,lastTime=null;
  for(const p of points) {
    if(p.quality!=="valid"||!Number.isFinite(p.value)){pen=false;continue;}
    const t=Date.parse(p.recorded_at);
    if(lastTime!==null && t-lastTime>settings.stale_seconds*1000)pen=false;
    path+=`${pen?"L":"M"}${x(p).toFixed(2)},${y(p).toFixed(2)} `;pen=true;lastTime=t;
  }
  const end=good.at(-1);
  return `<svg class="mini-chart" role="img" aria-label="${esc(METRICS[key].label)} trend" viewBox="0 0 ${w} ${h}"><line class="chart-grid" x1="${left}" x2="${right}" y1="${bottom}" y2="${bottom}"/><text class="chart-label" x="${left}" y="12">${esc(num(hi))}</text><path class="chart-line" d="${path}"/><circle class="chart-dot" cx="${x(end)}" cy="${y(end)}" r="3"/><text class="chart-label" x="${left}" y="155">${esc(clock(points[0].recorded_at))}</text><text class="chart-label" text-anchor="end" x="${right}" y="155">${esc(clock(points.at(-1).recorded_at))}</text></svg>`;
}
function renderTrends() {
  const node=nodes.find(n=>n.id===selected);
  $("#trendDescription").textContent=node?`${node.label} · ${trendUnavailable?"History unavailable":historyTruncated?"Latest 5,000 records; history truncated":"Last hour · gateway-estimated time"}`:"Select a node after data arrives";
  const buttons=$("#trendNodeButtons");buttons.replaceChildren();
  for(const n of nodes){const b=document.createElement("button");b.className=`trend-node-button ${n.id===selected?"active":""}`;b.textContent=n.label;b.onclick=()=>selectNode(n.id);buttons.append(b);}
  const keys=node?[...new Set([...metricKeys(node.id),...history.filter(s=>
    s.node_id===node.id && METRICS[s.sensor_type] && s.quality==="valid" &&
    (!(s.sensor_type in manualSensors)||node.sensors?.[s.sensor_type]))
    .map(s=>s.sensor_type)])]:[];
  $("#trendGrid").innerHTML=node?keys.map(k=>{
    const points=history.filter(p=>p.node_id===selected&&p.sensor_type===k).sort((a,b)=>Date.parse(a.recorded_at)-Date.parse(b.recorded_at));
    const current=readings(node.id).find(s=>s.sensor_type===k);
    const s=online(node)&&current&&fresh(current)?current:null, spec=METRICS[k];
    return `<article class="trend-card"><div class="trend-card-head"><span>${esc(spec.label)}</span><strong>${esc(display(s,spec))}<small>${s&&s.quality==="valid"?esc(spec.unit):""}</small></strong></div>${trendSvg(points,k)}</article>`;
  }).join(""):"";
}
async function setSensor(id,kind,enabled) {
  savingSensors.add(id); controlErrors.delete(id); renderNodes();
  try {
    const response=await fetch(`/api/nodes/${encodeURIComponent(id)}/sensors/${kind}`, {
      method:"PUT",headers:{"Content-Type":"application/json"},body:JSON.stringify({enabled}),signal:AbortSignal.timeout(5000)});
    if(!response.ok) throw new Error(`HTTP ${response.status}`);
    const result=await response.json();
    const node=nodes.find(n=>n.id===id); if(node) node.sensors=result.sensors;
    // Never reveal cached readings immediately after an enable/disable.
    latest[id]=(latest[id]||[]).map(s=>s.sensor_type===kind?{...s,value:null,quality:"unknown"}:s);
    await refresh();
  } catch(e) { controlErrors.set(id,"Could not save sensor settings. Refresh and retry."); }
  finally { savingSensors.delete(id); render(); }
}
async function selectNode(id){selected=id;history=[];renderTrends();await loadHistory();renderTrends();}
function renderNetwork(){
  $("#networkRows").innerHTML=networks.map(n=>{
    const node=nodes.find(x=>x.id===n.node_id),isOnline=node&&online(node);
    return `<tr><td>${esc(node?.label||n.node_id)}</td><td>${esc(n.path.join(" → ")||"Unknown")}${isOnline?"":" (last known)"}</td><td>${esc(num(n.path_cost))} ${n.metric==="energy_estimated_mj"?"mJ est.":"ETX approx."}</td><td>${esc(num(n.link_q))}</td><td>${esc(num(n.link_rssi))}</td></tr>`;
  }).join("")||'<tr><td colspan="5">No path data yet</td></tr>';
}
function render(){
  $("#connectionText").textContent=connected?"Data service connected":"Data service unavailable";
  $("#connectionText").previousElementSibling.style.background=connected?"var(--accent)":"var(--amber)";
  const warnings=connected?nodes.filter(online).flatMap(n=>readings(n.id).filter(alert).map(s=>({n,s}))):[];
  const count=nodes.filter(online).length;
  $("#onlineNodes").textContent=`${count} / ${nodes.length}`;
  $("#metricCount").textContent=new Set(nodes.flatMap(n=>metricKeys(n.id))).size;
  $("#activeAlerts").textContent=settings.alerts_enabled?warnings.length:"Disabled";
  $("#alertBadge").textContent=warnings.length;
  const freshCount=nodes.filter(online).filter(n=>metricKeys(n.id).length>0).length;
  const status=$("#systemStatus");
  status.textContent=!connected?"Service unavailable":!nodes.length?"Waiting for nodes":!count?"No recent packets":warnings.length?"Attention required":!freshCount?"Waiting for readings":count<nodes.length?"Some nodes unavailable":"Receiving data";
  status.className=warnings.length||!connected?"status-warning":"status-ok";
  $("#updatedAt").textContent=`Checked ${clock(new Date().toISOString())}`;
  $("#alertList").innerHTML=!connected?'<p class="empty-alert">Current alert state unavailable</p>':!settings.alerts_enabled?'<p class="empty-alert">Alerts disabled. Configure and validate thresholds in settings.json before enabling.</p>':warnings.length?warnings.map(({n,s})=>`<div class="alert-row"><span class="alert-icon">▲</span><div><strong>${esc(n.label)} · ${esc(METRICS[s.sensor_type].label)}</strong><p>${esc(num(shownValue(s)))} ${esc(METRICS[s.sensor_type].unit)}</p></div><span class="alert-time">${esc(clock(s.recorded_at))}</span></div>`).join(""):'<p class="empty-alert">No threshold breaches in fresh available readings. Missing data is not a safe-state confirmation.</p>';
  renderNodes();renderTrends();renderNetwork();
}
document.querySelectorAll(".nav-item").forEach(a=>a.addEventListener("click",()=>{
  document.querySelectorAll(".nav-item").forEach(x=>x.classList.toggle("active",x===a));
}));
$("#refreshButton").addEventListener("click",refresh);
refresh();setInterval(refresh,5000);
