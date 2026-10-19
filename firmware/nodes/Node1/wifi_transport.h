#pragma once
// Included after Packet's definition. Only the worker performs HTTP operations.
#include <HTTPClient.h>
#include <esp_timer.h>
#include "wifi_config.h"
struct UploadItem { Packet packet; int64_t receivedMs; };
static QueueHandle_t uploadQueue=nullptr;
static String numberOrNull(float n) { return isfinite(n)?String(n,4):String("null"); }
static String uploadJson(const UploadItem &u,int64_t offset) {
  const Packet &p=u.packet;
  char stamp[32]; snprintf(stamp,sizeof(stamp),"%lld",(long long)(offset+u.receivedMs));
  String j; j.reserve(1000);
  j="{\"observed_at_ms\":"; j+=stamp;
  j+=",\"raw\":{\"type\":\"sample\",\"node\":"; j+=p.source;
  j+=",\"boot\":"; j+=p.originBoot; j+=",\"seq\":"; j+=p.seq;
  j+=",\"room\":"; j+=p.room; j+=",\"valid\":"; j+=p.valid;
  j+=",\"sensor_mask\":"; j+=p.sensorMask;
  j+=",\"sample_age_ms\":"; j+=p.sampleAgeMs;
  j+=",\"temperature\":"; j+=numberOrNull(p.temperature);
  j+=",\"humidity\":"; j+=numberOrNull(p.humidity);
  j+=",\"lux\":"; j+=numberOrNull(p.lux);
  j+=",\"motion\":"; j+=p.motion;
  j+=",\"presence\":"; j+=p.presence;
  j+=",\"mq2_voltage\":"; j+=numberOrNull(p.mq2Voltage);
  j+=",\"dust_voltage\":"; j+=numberOrNull(p.dustVoltage);
  j+=",\"path_cost\":"; j+=numberOrNull(p.cost);
  j+=",\"link_q\":"; j+=numberOrNull(p.linkQ);
  j+=",\"link_rssi\":"; j+=numberOrNull(p.linkRssi);
  j+=",\"metric\":"; j+=p.metric;
  j+=",\"path\":[";
  for(unsigned i=0;i<p.count;i++){ if(i) j+=','; j+=p.path[i]; }
  if(p.count) j+=',';
  j+=ROOT_ID; j+="]}}"; return j;
}
static void connectLabWiFi() {
  WiFi.begin(LAB_WIFI_SSID,LAB_WIFI_PASSWORD,0,LAB_LOCK_BSSID?LAB_AP_BSSID:nullptr);
}
static void uploadWorker(void*) {
  int64_t clockOffset=0; bool clockReady=false, haveItem=false, wasConnected=false;
  UploadItem item{}; String payload;
  uint32_t lastAttempt=millis(), retryMs=1000;
  connectLabWiFi();
  for(;;) {
    if(WiFi.status()!=WL_CONNECTED) {
      if(wasConnected) Serial.println("{\"type\":\"wifi_disconnected\"}");
      wasConnected=false;
      if((uint32_t)(millis()-lastAttempt)>=15000) {
        WiFi.disconnect(false,false); connectLabWiFi(); lastAttempt=millis();
        Serial.println("{\"type\":\"wifi_retry\"}");
      }
      vTaskDelay(pdMS_TO_TICKS(250)); continue;
    }
    if(!wasConnected) {
      Serial.printf("{\"type\":\"wifi_connected\",\"node\":%u,\"ip\":\"%s\",\"channel\":%d,\"bssid\":\"%s\"}\n",
        NODE_ID,WiFi.localIP().toString().c_str(),(int)WiFi.channel(),WiFi.BSSIDstr().c_str());
      wasConnected=true;
    }
    if(NODE_ID!=ROOT_ID) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
    if(!clockReady) {
      // Local backend provides UTC: works without internet/NTP. Fix this mapping
      // for the entire boot so HTTP retries never advance a packet timestamp.
      HTTPClient http; http.setConnectTimeout(2000); http.setTimeout(2000);
      int64_t begin=esp_timer_get_time()/1000;
      http.begin(String(LAB_SERVER)+"/api/wifi-time"); int code=http.GET();
      String reply=code==200?http.getString():String();
      int64_t end=esp_timer_get_time()/1000; http.end();
      char *tail=nullptr; int64_t epoch=strtoll(reply.c_str(),&tail,10);
      if(code==200 && tail && *tail=='\0' && epoch>1700000000000LL && epoch<4102444800000LL && end-begin<2000) {
        clockOffset=epoch-(begin+end)/2; clockReady=true;
        Serial.println("{\"type\":\"wifi_clock_ready\"}");
      } else { Serial.printf("{\"type\":\"wifi_time_failed\",\"http\":%d}\n",code); vTaskDelay(pdMS_TO_TICKS(3000)); }
      continue;
    }
    if(!haveItem) {
      if(xQueueReceive(uploadQueue,&item,pdMS_TO_TICKS(250))!=pdTRUE) continue;
      payload=uploadJson(item,clockOffset); haveItem=true;
    }
    HTTPClient http; http.setConnectTimeout(2000); http.setTimeout(2000);
    http.begin(String(LAB_SERVER)+"/api/wifi-upload");
    http.addHeader("Content-Type","application/json");
    int code=http.POST(payload); String reply=code>0?http.getString():String(); http.end();
    // This endpoint returns exactly 'success' or 'duplicate' only after DB commit.
    if(code==200 && (reply=="success" || reply=="duplicate")) {
      Serial.printf("{\"type\":\"wifi_uploaded\",\"node\":%u,\"seq\":%lu}\n",item.packet.source,(unsigned long)item.packet.seq);
      haveItem=false; payload=""; retryMs=1000;
    } else if(code>=400 && code<500 && code!=408 && code!=429) {
      Serial.printf("{\"type\":\"wifi_rejected\",\"node\":%u,\"seq\":%lu,\"http\":%d}\n",item.packet.source,(unsigned long)item.packet.seq,code);
      haveItem=false; payload=""; retryMs=1000;
    } else {
      Serial.printf("{\"type\":\"wifi_upload_retry\",\"http\":%d,\"queued\":%u}\n",code,(unsigned)uxQueueMessagesWaiting(uploadQueue));
      vTaskDelay(pdMS_TO_TICKS(retryMs)); retryMs=retryMs>=15000?15000:retryMs*2;
    }
  }
}
static bool startWifiUpload() {
  if(NODE_ID==ROOT_ID) { uploadQueue=xQueueCreate(96,sizeof(UploadItem)); if(!uploadQueue) return false; }
  WiFi.setAutoReconnect(true);
  return xTaskCreate(uploadWorker,"wifi_upload",8192,nullptr,1,nullptr)==pdPASS;
}
static bool queueUpload(const Packet &p) {
  UploadItem item{p,esp_timer_get_time()/1000};
  if(uploadQueue && xQueueSend(uploadQueue,&item,0)==pdTRUE) return true;
  Serial.printf("{\"type\":\"wifi_queue_full\",\"node\":%u,\"seq\":%lu}\n",p.source,(unsigned long)p.seq);
  return false;
}
