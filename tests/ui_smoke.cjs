// Node-only rendering logic smoke tests; not a browser or visual-layout test.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../ui/app.js'),'utf8');
const html=fs.readFileSync(path.join(__dirname,'../ui/index.html'),'utf8');
for(const [,id] of source.matchAll(/\$\("#([^" ]+)"\)/g))assert(html.includes(`id="${id}"`),`Missing DOM id ${id}`);
class Element {
  constructor(){this.children=[];this.parts=new Map();this.textContent='';this.innerHTML='';this.style={};this.classList={toggle(){}};this.previousElementSibling={style:{}};}
  querySelector(s){if(!this.parts.has(s))this.parts.set(s,new Element());return this.parts.get(s);}
  replaceChildren(){this.children=[];} append(e){this.children.push(e);} after(){} addEventListener(){} scrollIntoView(){}
}
async function scenario(snapshot,fail=false){
  const els=new Map(),get=s=>{if(!els.has(s))els.set(s,new Element());return els.get(s);};
  get('#nodeTemplate').content={cloneNode:()=>new Element()};
  const context=vm.createContext({console,Date,Number,String,Math,Set,Promise,encodeURIComponent,
    AbortSignal:{timeout:()=>null},setInterval(){},
    document:{querySelector:get,querySelectorAll:()=>[],createElement:()=>new Element()},
    fetch:async url=>{if(fail)throw new Error('offline');return {ok:true,json:async()=>url.startsWith('/api/history')?{data:[],truncated:false}:snapshot};}});
  vm.runInContext(source,context);
  for(let i=0;i<12;i++)await new Promise(r=>setImmediate(r));
  return {get,context};
}
(async()=>{
  const base={nodes:[],latest:{},network:[],settings:{offline_seconds:120,stale_seconds:120,alerts_enabled:false,thresholds:{}}};
  let s=await scenario(base);assert.equal(s.get('#onlineNodes').textContent,'0 / 0');
  assert.equal(s.get('#systemStatus').textContent,'Waiting for nodes');
  const node={id:'esp32_3',label:'ESP32 · 03',room:'Room 2',last_seen:new Date().toISOString()};
  const state={...base,nodes:[node],network:[{node_id:node.id,path:[3,1,2],path_cost:2,link_q:.9,link_rssi:-65,metric:'etx_app'}]};
  s=await scenario(state);assert.equal(s.get('#systemStatus').textContent,'Waiting for readings');
  assert.equal(s.get('#onlineNodes').textContent,'1 / 1');
  assert(s.get('#networkRows').innerHTML.includes('3 → 1 → 2'));
  const reading={sensor_type:'temperature',quality:'valid',value:25,recorded_at:new Date().toISOString()};
  s=await scenario({...state,latest:{esp32_3:[reading]}});assert.equal(s.get('#systemStatus').textContent,'Receiving data');
  s=await scenario(base,true);assert.equal(s.get('#systemStatus').textContent,'Service unavailable');
  console.log('4 UI logic smoke scenarios passed; DOM IDs checked. No browser visual QA.');
})().catch(e=>{console.error(e);process.exitCode=1;});
