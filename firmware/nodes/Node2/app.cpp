#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "config.h"
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 1, 0)
#error "Requires Arduino-ESP32 3.x with ESP-IDF >= 5.1"
#endif
#if ENABLE_SENSORS && (SENSOR_MASK & 2)
#include <Wire.h>
#include <BH1750.h>
static BH1750 lightMeter;
static bool lightOK;
#endif
#if ENABLE_SENSORS && (SENSOR_MASK & 1)
#include <DHT.h>
static DHT dht(DHT_PIN, DHT11);
#endif

// Same ESP32 little-endian IEEE754 wire ABI on all boards. Versioned, <=250B.
enum Kind : uint8_t { ADV=1, PROBE=2, ACK=3, DATA=4, RECEIPT=5 };
struct __attribute__((packed)) Packet {
  uint32_t network;
  uint8_t version, kind, metric, flags;
  uint16_t sender, root, source, room;
  uint32_t senderBoot, originBoot, seq, token, epoch, sampleAgeMs;
  float cost, temperature, humidity, lux, linkQ, linkRssi;
  uint8_t motion, valid, count, cursor, sensorMask;
  uint16_t path[8];
};
static_assert(sizeof(Packet) <= 250, "ESP-NOW v1 packet budget");
struct Rx { Packet p; uint8_t mac[6]; int8_t rssi; };
struct Neighbor {
  bool used=false, verified=false;
  uint16_t id=0;
  uint8_t mac[6]{};
  uint32_t boot=0, lastSeen=0, advAt=0, verifiedAt=0, advSeq=0;
  float q=.8f, rssi=-100;
  Packet adv{};
};
struct Item { Packet p; uint32_t at; };
struct Seen { uint16_t source; uint32_t boot, seq, at; bool used; };
static QueueHandle_t rxQueue;
static Neighbor neighbors[MAX_NEIGHBORS];
static Item outbox[OUTBOX_SIZE];
static unsigned outCount=0;
static Seen seen[128]{};
static unsigned seenCursor=0;
static uint32_t bootId, dataSeq=0, tokenSeq=0, advSeq=0;
static int parent=-1, candidate=-1;
static unsigned confirmations=0;
static Packet route{};
static uint32_t advInterval=ADV_MIN_MS, nextAdv=0, lastEval=0, lastProbe=0, lastSample=0;
static unsigned probeCursor=0;
static const uint8_t broadcastMac[6]={255,255,255,255,255,255};
struct Flight { bool active=false, probe=false; int neighbor=-1; Packet p{}; uint32_t sent=0; unsigned attempts=0; };
static Flight flight;
static bool ready=false;

static uint32_t age(uint32_t now,uint32_t then) { return now-then; }
static bool due(uint32_t now,uint32_t when) { return (int32_t)(now-when)>=0; }
static bool contains(const Packet &p,uint16_t id) {
  for(unsigned i=0;i<p.count && i<MAX_PATH;i++) if(p.path[i]==id) return true;
  return false;
}
static Packet makePacket(Kind kind) {
  Packet p{}; p.network=NETWORK_ID; p.version=2; p.kind=kind;
  p.metric=ROUTING_METRIC; p.sender=NODE_ID; p.senderBoot=bootId; p.root=ROOT_ID;
  return p;
}
static void event(const char *name,uint16_t other=0) {
  Serial.printf("{\"type\":\"event\",\"node\":%u,\"event\":\"%s\",\"other\":%u}\n",NODE_ID,name,other);
}
static bool addPeer(const uint8_t *mac) {
  if(esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer{}; memcpy(peer.peer_addr,mac,6);
  peer.channel=RADIO_CHANNEL; peer.ifidx=WIFI_IF_STA; peer.encrypt=false;
  return esp_now_add_peer(&peer)==ESP_OK;
}
static bool sendRaw(const uint8_t *mac,const Packet &p) {
  return addPeer(mac) && esp_now_send(mac,(const uint8_t*)&p,sizeof(p))==ESP_OK;
}
static void receive(const esp_now_recv_info_t *info,const uint8_t *bytes,int len) {
  if(!info || !info->rx_ctrl || len!=(int)sizeof(Packet)) return;
  Rx rx{}; memcpy(&rx.p,bytes,sizeof(Packet)); memcpy(rx.mac,info->src_addr,6);
  rx.rssi=info->rx_ctrl->rssi;
  // Callback does no sensor I/O, retries, peer mutation, or serial output.
  xQueueSend(rxQueue,&rx,0);
}
static void fastAdvertise() {
  advInterval=ADV_MIN_MS; nextAdv=millis()+100+(esp_random()%400);
}
static void invalidate() {
  if(NODE_ID==ROOT_ID) return;
  bool had=route.count!=0; parent=-1; route=makePacket(ADV); route.cost=INFINITY;
  candidate=-1; confirmations=0;
  if(had) { event("route_lost"); fastAdvertise(); }
}
static int findNeighbor(const Rx &rx) {
  int empty=-1;
  for(unsigned i=0;i<MAX_NEIGHBORS;i++) {
    Neighbor &n=neighbors[i];
    if(!n.used) { empty=(int)i; continue; }
    if(n.id==rx.p.sender) {
      if(memcmp(n.mac,rx.mac,6)!=0) { event("duplicate_node_id",n.id); return -1; }
      if(n.boot!=rx.p.senderBoot) {
        if(parent==(int)i) invalidate();
        if(flight.active && flight.neighbor==(int)i) flight.active=false;
        n.verified=false; n.advAt=0; n.adv.count=0; n.q=.8f; n.boot=rx.p.senderBoot;
        event("neighbor_reboot",n.id);
      }
      return i;
    }
  }
  // Bounded table. Reclaim only stale, non-active peers.
  if(empty<0) for(unsigned i=0;i<MAX_NEIGHBORS;i++)
    if((int)i!=parent && !(flight.active && flight.neighbor==(int)i) &&
       age(millis(),neighbors[i].lastSeen)>ROUTE_TTL_MS) {
      esp_now_del_peer(neighbors[i].mac); empty=i; break;
    }
  if(empty<0) return -1;
  Neighbor &n=neighbors[empty]; n=Neighbor{}; n.used=true; n.id=rx.p.sender;
  n.boot=rx.p.senderBoot; memcpy(n.mac,rx.mac,6); n.rssi=rx.rssi;
  return empty;
}
static bool validAdv(const Packet &p) {
  if(!p.count) return true; // explicit withdrawal
  if(p.count>MAX_PATH || p.path[0]!=p.sender || p.path[p.count-1]!=ROOT_ID ||
     !isfinite(p.cost) || p.cost<0 || !p.epoch) return false;
  if(p.sender==ROOT_ID && (p.count!=1 || p.cost!=0 || p.epoch!=p.senderBoot)) return false;
  for(unsigned i=0;i<p.count;i++) for(unsigned j=0;j<i;j++) if(p.path[i]==p.path[j]) return false;
  return true;
}
static bool usable(int i) {
  const Neighbor &n=neighbors[i]; uint32_t now=millis();
  return n.used && n.verified && age(now,n.verifiedAt)<VERIFIED_TTL_MS &&
    age(now,n.advAt)<ROUTE_TTL_MS && n.adv.count && n.adv.count<MAX_PATH &&
    (n.adv.flags&1) && n.q>=MIN_Q && n.rssi>=MIN_RSSI && !contains(n.adv,NODE_ID);
}
static float linkCost(int i) {
  const Neighbor &n=neighbors[i];
  float e=ROUTING_METRIC==ETX ? 1.0f : attemptEnergyMj(n.id);
  return isfinite(e) && e>0 ? e/fmaxf(n.q,.05f) : INFINITY;
}
static void selectRoute() {
  if(NODE_ID==ROOT_ID || flight.active) return;
  int best=-1; float bestCost=INFINITY;
  for(unsigned i=0;i<MAX_NEIGHBORS;i++) if(usable(i)) {
    float c=linkCost(i)+neighbors[i].adv.cost;
    if(c<bestCost) { best=i; bestCost=c; }
  }
  bool oldOK=parent>=0 && usable(parent) && isfinite(linkCost(parent));
  if(best<0) { invalidate(); return; }
  int chosen=parent;
  if(!oldOK) chosen=best;
  else if(best!=parent && bestCost<SWITCH_RATIO*(linkCost(parent)+neighbors[parent].adv.cost)) {
    if(candidate==best) confirmations++; else { candidate=best; confirmations=1; }
    if(confirmations>=SWITCH_CONFIRMATIONS) chosen=best;
  } else { candidate=-1; confirmations=0; }
  if(chosen<0) return;
  Packet next=makePacket(ADV); const Packet &a=neighbors[chosen].adv;
  next.count=a.count+1; next.path[0]=NODE_ID;
  for(unsigned k=0;k<a.count;k++) next.path[k+1]=a.path[k];
  next.epoch=a.epoch; next.cost=linkCost(chosen)+a.cost;
  bool changed=chosen!=parent || next.epoch!=route.epoch || next.count!=route.count ||
    memcmp(next.path,route.path,sizeof(next.path))!=0;
  bool costChanged=!isfinite(route.cost) || fabsf(next.cost-route.cost)>.15f*fmaxf(route.cost,.1f);
  parent=chosen; route=next;
  if(changed || costChanged) {
    fastAdvertise(); event("route_update",neighbors[parent].id);
    if(changed) { candidate=-1; confirmations=0; }
  }
}
static void advertise() {
  Packet p=route; p.kind=ADV; p.seq=++advSeq;
  p.flags=(RELAY_ALLOWED || NODE_ID==ROOT_ID) && outCount<OUTBOX_SIZE-2 ? 1 : 0;
  sendRaw(broadcastMac,p);
  Serial.printf("{\"type\":\"route\",\"node\":%u,\"boot\":%lu,\"parent\":%u,\"metric\":%u,\"cost\":",
    NODE_ID,(unsigned long)bootId,parent>=0?neighbors[parent].id:0,(unsigned)ROUTING_METRIC);
  if(isfinite(p.cost)) Serial.print(p.cost,4); else Serial.print("null");
  Serial.print(",\"path\":["); for(unsigned i=0;i<p.count;i++) { if(i) Serial.print(','); Serial.print(p.path[i]); }
  Serial.println("]}");
  nextAdv=millis()+advInterval/2+(esp_random()%(advInterval/2));
  advInterval=advInterval>=ADV_MAX_MS/2?ADV_MAX_MS:advInterval*2;
}
static bool duplicate(const Packet &p) {
  for(const Seen &s:seen) if(s.used && s.source==p.source && s.boot==p.originBoot && s.seq==p.seq &&
    age(millis(),s.at)<DATA_TTL_MS*2) return true;
  return false;
}
static void remember(const Packet &p) {
  seen[seenCursor]={p.source,p.originBoot,p.seq,millis(),true}; seenCursor=(seenCursor+1)%128;
}
static bool enqueue(const Packet &p) {
  if(outCount>=OUTBOX_SIZE) { event("queue_full",p.source); return false; }
  outbox[outCount++]={p,millis()}; return true;
}
static void pop() { for(unsigned i=1;i<outCount;i++) outbox[i-1]=outbox[i]; if(outCount) outCount--; }
static void jsonFloat(float value) { if(isfinite(value)) Serial.print(value,3); else Serial.print("null"); }
static void deliver(const Packet &p) {
  Serial.printf("{\"type\":\"sample\",\"node\":%u,\"boot\":%lu,\"seq\":%lu,\"room\":%u,\"valid\":%u,\"temperature\":",
    p.source,(unsigned long)p.originBoot,(unsigned long)p.seq,p.room,p.valid);
  jsonFloat(p.temperature); Serial.print(",\"humidity\":"); jsonFloat(p.humidity);
  Serial.print(",\"lux\":"); jsonFloat(p.lux);
  Serial.printf(",\"motion\":%u,\"path_cost\":",p.motion); jsonFloat(p.cost);
  Serial.print(",\"link_q\":"); jsonFloat(p.linkQ);
  Serial.print(",\"link_rssi\":"); jsonFloat(p.linkRssi);
  Serial.printf(",\"metric\":%u,\"sensor_mask\":%u,\"sample_age_ms\":%lu",p.metric,p.sensorMask,(unsigned long)p.sampleAgeMs);
  Serial.print(",\"path\":[");
  for(unsigned i=0;i<p.count;i++) { if(i) Serial.print(','); Serial.print(p.path[i]); }
  if(p.count) Serial.print(',');
  Serial.printf("%u]}\n",ROOT_ID);
}
static void hopAck(const Rx &rx) {
  Packet a=makePacket(ACK); a.token=rx.p.token; a.originBoot=rx.p.senderBoot;
  a.cost=rx.rssi; // Receiver reports RSSI of the actual forward-direction packet.
  sendRaw(rx.mac,a);
}
static void receipt(const Packet &data) {
  if(!data.count) return;
  Packet r=data; r.kind=RECEIPT; r.sender=NODE_ID; r.senderBoot=bootId;
  r.cursor=r.count-1; enqueue(r);
}
static void handle(const Rx &rx) {
  const Packet &p=rx.p;
  if(p.network!=NETWORK_ID || p.version!=2 || p.root!=ROOT_ID || p.metric!=ROUTING_METRIC ||
    !p.sender || p.sender==NODE_ID || p.count>MAX_PATH || p.kind<ADV || p.kind>RECEIPT) return;
  int ni=findNeighbor(rx); if(ni<0) return;
  Neighbor &n=neighbors[ni]; n.lastSeen=millis();
  if(p.kind==ADV) {
    if(!validAdv(p)) return;
    if(n.advAt && (int32_t)(p.seq-n.advSeq)<=0) return;
    n.adv=p; n.advAt=millis(); n.advSeq=p.seq;
    if(parent==ni && !usable(ni)) invalidate();
    return;
  }
  if(p.kind==PROBE) { hopAck(rx); return; }
  if(p.kind==ACK) {
    if(flight.active && flight.neighbor==ni && p.token==flight.p.token && p.originBoot==bootId) {
      n.q=.9f*n.q+.1f; n.verified=true; n.verifiedAt=millis();
      if(isfinite(p.cost) && p.cost<=0 && p.cost>=-127) n.rssi=.8f*n.rssi+.2f*p.cost;
      Serial.printf("{\"type\":\"link\",\"node\":%u,\"peer\":%u,\"q\":%.4f,\"rssi\":%.1f,\"rtt_ms\":%lu}\n",
        NODE_ID,n.id,n.q,n.rssi,(unsigned long)age(millis(),flight.sent));
      if(!flight.probe) pop();
      flight.active=false;
    }
    return;
  }
  if(p.kind==RECEIPT) {
    if(!p.count || p.cursor>=p.count || p.path[p.cursor]!=NODE_ID) return;
    if(p.cursor==0 && p.source==NODE_ID && p.originBoot==bootId) {
      event("root_received",p.source);
      Serial.printf("{\"type\":\"receipt\",\"node\":%u,\"boot\":%lu,\"seq\":%lu}\n",NODE_ID,(unsigned long)p.originBoot,(unsigned long)p.seq);
      hopAck(rx);
    } else if(p.cursor>0) { Packet r=p; r.cursor--; if(enqueue(r)) hopAck(rx); }
    return;
  }
  if(!p.source || !p.count || p.path[0]!=p.source || p.path[p.count-1]!=p.sender || contains(p,NODE_ID)) return;
  for(unsigned i=0;i<p.count;i++) for(unsigned j=0;j<i;j++) if(p.path[i]==p.path[j]) return;
  if(duplicate(p)) { hopAck(rx); if(NODE_ID==ROOT_ID) receipt(p); return; }
  if(NODE_ID==ROOT_ID) {
    deliver(p); remember(p); hopAck(rx); receipt(p);
  } else if(RELAY_ALLOWED && p.count<MAX_PATH-1) {
    if(enqueue(p)) { remember(p); hopAck(rx); }
  }
}
static void transmit() {
  flight.p.sender=NODE_ID; flight.p.senderBoot=bootId;
  if(flight.attempts && flight.p.kind==DATA) flight.p.sampleAgeMs+=age(millis(),flight.sent);
  flight.p.token=++tokenSeq; flight.sent=millis(); flight.attempts++;
  // Local driver rejection is observed as timeout; q is application-attempt q.
  sendRaw(neighbors[flight.neighbor].mac,flight.p);
}
static void serviceFlight() {
  if(!flight.active || age(millis(),flight.sent)<ACK_TIMEOUT_MS) return;
  Neighbor &n=neighbors[flight.neighbor]; n.q=.9f*n.q;
  if(flight.attempts<MAX_ATTEMPTS) { transmit(); return; }
  n.verified=false;
  if(parent==flight.neighbor) invalidate();
  event("hop_failed",n.id); flight.active=false;
  // Keep data queued for alternate route. Receipts have fixed reverse path.
}
static void startWork() {
  if(flight.active) return;
  uint32_t now=millis();
  if(age(now,lastProbe)>=PROBE_MS) {
    lastProbe=now;
    for(unsigned k=0;k<MAX_NEIGHBORS;k++) {
      unsigned i=probeCursor++%MAX_NEIGHBORS;
      if(neighbors[i].used && age(now,neighbors[i].lastSeen)<ROUTE_TTL_MS) {
        flight=Flight{}; flight.active=true; flight.probe=true; flight.neighbor=i;
        flight.p=makePacket(PROBE); transmit(); return;
      }
    }
  }
  if(!outCount) return;
  uint32_t ttl=outbox[0].p.kind==RECEIPT?8000:DATA_TTL_MS;
  if(age(now,outbox[0].at)>ttl) { event("queue_expired",outbox[0].p.source); pop(); return; }
  Packet p=outbox[0].p; int target=-1;
  if(p.kind==RECEIPT) {
    for(unsigned i=0;i<MAX_NEIGHBORS;i++) if(neighbors[i].used && neighbors[i].id==p.path[p.cursor] &&
      neighbors[i].verified && age(now,neighbors[i].verifiedAt)<VERIFIED_TTL_MS) target=i;
  } else {
    if(parent<0 || !usable(parent)) return;
    // Reject a stale route that intersects the DATA packet's already-visited path.
    for(unsigned i=0;i<p.count;i++) if(contains(neighbors[parent].adv,p.path[i])) { invalidate(); return; }
    if(p.count>=MAX_PATH-1) { event("hop_limit",p.source); pop(); return; }
    p.sampleAgeMs+=age(now,outbox[0].at);
    p.path[p.count++]=NODE_ID; target=parent;
  }
  if(target<0) return;
  flight=Flight{}; flight.active=true; flight.neighbor=target; flight.p=p; transmit();
}
static void sample() {
  Packet p=makePacket(DATA); p.source=NODE_ID; p.originBoot=bootId; p.seq=++dataSeq; p.room=ROOM_ID;
  Serial.printf("{\"type\":\"generated\",\"node\":%u,\"boot\":%lu,\"seq\":%lu}\n",NODE_ID,(unsigned long)bootId,(unsigned long)p.seq);
  p.cost=route.cost; p.linkQ=p.linkRssi=NAN;
  if(parent>=0) { p.linkQ=neighbors[parent].q; p.linkRssi=neighbors[parent].rssi; }
  p.temperature=p.humidity=p.lux=NAN;
#if ENABLE_SENSORS
  p.sensorMask=SENSOR_MASK;
#endif
#if ENABLE_SENSORS && (SENSOR_MASK & 1)
  p.temperature=dht.readTemperature(); p.humidity=dht.readHumidity();
  if(isfinite(p.temperature)&&isfinite(p.humidity)) p.valid|=1;
#endif
#if ENABLE_SENSORS && (SENSOR_MASK & 2)
  if(lightOK) { p.lux=lightMeter.readLightLevel(); if(isfinite(p.lux)&&p.lux>=0) p.valid|=2; else p.lux=NAN; }
#endif
#if ENABLE_SENSORS && (SENSOR_MASK & 4)
  p.motion=digitalRead(PIR_PIN)==HIGH; p.valid|=4;
#endif
  if(NODE_ID==ROOT_ID) deliver(p); else enqueue(p);
}
void appSetup() {
  Serial.begin(115200); bootId=esp_random(); if(!bootId) bootId=1;
  rxQueue=xQueueCreate(32,sizeof(Rx)); if(!rxQueue) { event("queue_init_failed"); return; }
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  if(esp_wifi_set_channel(RADIO_CHANNEL,WIFI_SECOND_CHAN_NONE)!=ESP_OK || esp_now_init()!=ESP_OK) {
    event("radio_init_failed"); return;
  }
  if(esp_now_register_recv_cb(receive)!=ESP_OK || !addPeer(broadcastMac)) { event("radio_setup_failed"); return; }
  route=makePacket(ADV); route.cost=INFINITY;
  if(NODE_ID==ROOT_ID) { route.count=1; route.path[0]=ROOT_ID; route.cost=0; route.epoch=bootId; }
#if ENABLE_SENSORS && (SENSOR_MASK & 4)
  pinMode(PIR_PIN,INPUT);
#endif
#if ENABLE_SENSORS && (SENSOR_MASK & 1)
  dht.begin();
#endif
#if ENABLE_SENSORS && (SENSOR_MASK & 2)
  Wire.begin(SDA_PIN,SCL_PIN);
  lightOK=lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
#endif
  nextAdv=millis()+esp_random()%500; lastSample=millis(); ready=true; event("boot");
}
void appLoop() {
  if(!ready) { delay(100); return; }
  Rx rx; unsigned handled=0;
  while(handled++<32 && xQueueReceive(rxQueue,&rx,0)==pdTRUE) handle(rx);
  serviceFlight(); uint32_t now=millis();
  if(age(now,lastEval)>=EVALUATE_MS) { lastEval=now; selectRoute(); }
  if(due(now,nextAdv)) advertise();
  // DHT access can briefly block. Avoid it while waiting for an ACK.
  if(!flight.active && age(now,lastSample)>=SAMPLE_MS) { lastSample=now; sample(); }
  startWork(); delay(1);
}
