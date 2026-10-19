#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <BH1750.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali_scheme.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#include "sensors.h"
#include "radar_parser.h"
#if !CONFIG_IDF_TARGET_ESP32S3
#error "GP2Y fast acquisition v2 is specific to ESP32-S3."
#endif
#include <hal/adc_ll.h>
#include <esp_private/adc_share_hw_ctrl.h>
// ESP-IDF uses this same spinlock to serialize RTC ADC register access.
extern portMUX_TYPE rtc_spinlock;

static DHT dht(DHT_PIN,DHT11);
static BH1750 light;
static bool lightReady=false;
static uint32_t lightStarted=0;
static uint8_t lightAddress=0x23;
static HardwareSerial radarSerial(1);
static RadarParser radar;
static SemaphoreHandle_t adcMutex;
static adc_oneshot_unit_handle_t adcUnit=nullptr;
static adc_cali_handle_t mqCalibration=nullptr,dustCalibration=nullptr;
static adc_channel_t mqChannel,dustChannel;
static bool prepareAdc(int pin,adc_channel_t &channel,adc_cali_handle_t &calibration) {
  adc_unit_t unit;
  if(!adcUnit || adc_oneshot_io_to_channel(pin,&unit,&channel)!=ESP_OK || unit!=ADC_UNIT_1) return false;
  adc_oneshot_chan_cfg_t config{};
  config.atten=ADC_ATTEN_DB_11; config.bitwidth=ADC_BITWIDTH_12;
  if(adc_oneshot_config_channel(adcUnit,channel,&config)!=ESP_OK) return false;
  adc_cali_curve_fitting_config_t cal{};
  cal.unit_id=ADC_UNIT_1; cal.chan=channel; cal.atten=config.atten; cal.bitwidth=config.bitwidth;
  return adc_cali_create_scheme_curve_fitting(&cal,&calibration)==ESP_OK;
}
static float calibratedVoltage(int raw,adc_cali_handle_t calibration) {
  int mv=0;
  if(!calibration || raw<0 || raw>=4095 || adc_cali_raw_to_voltage(calibration,raw,&mv)!=ESP_OK) return NAN;
  return mv/1000.0f;
}
static hw_timer_t *ledTimer=nullptr;
static portMUX_TYPE dustLock=portMUX_INITIALIZER_UNLOCKED;
static float dustVoltage=NAN;
static uint32_t dustAt=0;

static void ARDUINO_ISR_ATTR ledOff() {
  gpio_set_level((gpio_num_t)DUST_LED_PIN,0);
}
static bool lightResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission()==0;
}
// Counters describe software observations; they do not prove physical LED timing.
struct DustDiagnostics {
  uint32_t cycles=0, ok=0, busy=0, late=0, prepError=0, adcError=0, rail=0, calibration=0;
  uint32_t startUs=0, endUs=0;
  int raw=-1;
};
static DustDiagnostics dustDiag;
static bool dustTaskStarted=false;
static uint32_t dustLastReport=0;
static void reportDust() {
  if(uint32_t(millis()-dustLastReport)<5000) return;
  dustLastReport=millis();
  portENTER_CRITICAL(&dustLock);
  DustDiagnostics d=dustDiag;
  float v=dustVoltage; uint32_t at=dustAt;
  portEXIT_CRITICAL(&dustLock);
  const uint32_t age=uint32_t(millis()-at);
  char voltage[24], ageText[24];
  if(isfinite(v) && age<DUST_FRESH_MS) snprintf(voltage,sizeof(voltage),"%.4f",v);
  else snprintf(voltage,sizeof(voltage),"null");
  if(isfinite(v)) snprintf(ageText,sizeof(ageText),"%lu",(unsigned long)age);
  else snprintf(ageText,sizeof(ageText),"null");
  Serial.printf("{\"type\":\"dust_diag\",\"node\":%u,\"revision\":\"bc337-v2-fastadc\","
    "\"running\":%s,\"cycles\":%lu,\"ok\":%lu,\"busy\":%lu,\"late\":%lu,"
    "\"prep_error\":%lu,\"adc_error\":%lu,\"rail\":%lu,\"cal_error\":%lu,\"raw\":%d,"
    "\"start_us\":%lu,\"end_us\":%lu,\"last_good_age_ms\":%s,\"voltage\":%s}\n",
    NODE_ID,dustTaskStarted?"true":"false",(unsigned long)d.cycles,(unsigned long)d.ok,
    (unsigned long)d.busy,(unsigned long)d.late,(unsigned long)d.prepError,(unsigned long)d.adcError,
    (unsigned long)d.rail,(unsigned long)d.calibration,d.raw,
    (unsigned long)d.startUs,(unsigned long)d.endUs,ageText,voltage);
}
static void dustTask(void*) {
  TickType_t cycle=xTaskGetTickCount();
  for(;;) {
    float voltage=NAN;
    int raw=-1;
    uint32_t started=0,ended=0;
    enum Result { GOOD, BUSY, LATE, PREP_ERROR, ADC_ERROR, RAIL, CAL_ERROR } status=BUSY;
    if(xSemaphoreTake(adcMutex,pdMS_TO_TICKS(2))==pdTRUE) {
      // Prepare ADC1 on the correct channel/attenuation, including hardware
      // calibration, BEFORE the optical pulse. Discard this LED-off reading.
      // Keep our mutex throughout so the MQ-2 task cannot change ADC1 channel.
      int discarded=-1;
      esp_err_t prepared=adc_oneshot_read(adcUnit,dustChannel,&discarded);
      if(prepared!=ESP_OK) status=PREP_ERROR;
      else if(adc_lock_try_acquire(ADC_UNIT_1)!=ESP_OK) status=BUSY;
      else {
        // Take the IDF ADC1 lock too: no driver read can race the LL conversion.
        timerStop(ledTimer); timerWrite(ledTimer,0);
        timerAlarm(ledTimer,DUST_LED_OFF_US,false,0);
        gpio_set_level((gpio_num_t)DUST_LED_PIN,1);
        timerStart(ledTimer);
        while(timerRead(ledTimer)<DUST_SAMPLE_US) { }
        bool done=false, triggered=false;
        portENTER_CRITICAL(&rtc_spinlock);
        started=(uint32_t)timerRead(ledTimer);
        if(started<DUST_LATEST_START_US && SENS.sar_slave_addr1.meas_status==0) {
          // ESP32-S3 RTC ADC1 start sequence, as used by adc_oneshot_ll_start.
          // No unbounded idle wait; reject if the hardware was not idle.
          SENS.sar_meas1_ctrl2.meas1_start_sar=0;
          SENS.sar_meas1_ctrl2.meas1_start_sar=1;
          triggered=true;
          do {
            done=adc_oneshot_ll_get_event(ADC_LL_EVENT_ADC1_ONESHOT_DONE);
            if(done) break;
          } while(timerRead(ledTimer)<DUST_LED_OFF_US);
          if(done) raw=(int)adc_oneshot_ll_get_raw_result(ADC_UNIT_1);
        }
        ended=(uint32_t)timerRead(ledTimer);
        // A timer interrupt may be deferred by this short critical section.
        // Explicitly turn the LED off at the deadline on every timeout path.
        if(ended>=DUST_LED_OFF_US) gpio_set_level((gpio_num_t)DUST_LED_PIN,0);
        portEXIT_CRITICAL(&rtc_spinlock);
        while(timerRead(ledTimer)<DUST_LED_OFF_US) { }
        gpio_set_level((gpio_num_t)DUST_LED_PIN,0);
        timerStop(ledTimer);
        adc_lock_release(ADC_UNIT_1);
        if(started>=DUST_LATEST_START_US || ended>DUST_LED_OFF_US || (triggered&&!done)) status=LATE;
        else if(!triggered) status=ADC_ERROR;
        else if(raw<0 || raw>=4095) status=RAIL;
        else {
          voltage=calibratedVoltage(raw,dustCalibration)*DUST_DIVIDER_GAIN;
          status=isfinite(voltage)?GOOD:CAL_ERROR;
        }
      }
      xSemaphoreGive(adcMutex);
    }
    portENTER_CRITICAL(&dustLock);
    ++dustDiag.cycles;
    dustDiag.raw=raw; dustDiag.startUs=started; dustDiag.endUs=ended;
    switch(status) {
      case GOOD:
        ++dustDiag.ok;
        // A transient failure no longer destroys the last successful sample.
        // Its timestamp is advanced ONLY on success; it expires after 1 second.
        dustVoltage=voltage; dustAt=millis(); break;
      case BUSY: ++dustDiag.busy; break;
      case LATE: ++dustDiag.late; break;
      case PREP_ERROR: ++dustDiag.prepError; break;
      case ADC_ERROR: ++dustDiag.adcError; break;
      case RAIL: ++dustDiag.rail; break;
      case CAL_ERROR: ++dustDiag.calibration; break;
    }
    portEXIT_CRITICAL(&dustLock);
    vTaskDelayUntil(&cycle,pdMS_TO_TICKS(10));
    if(xTaskGetTickCount()-cycle>pdMS_TO_TICKS(10)) cycle=xTaskGetTickCount();
  }
}
void sensorsBegin() {
#if ENABLE_SENSORS
#if SENSOR_MASK & 1
  dht.begin();
#endif
#if SENSOR_MASK & 2
  Wire.begin(SDA_PIN,SCL_PIN); Wire.setTimeOut(20);
#endif
#if SENSOR_MASK & 4
  pinMode(PIR_PIN,INPUT);
#endif
#if SENSOR_MASK & 8
  radarSerial.setRxBufferSize(512);
  radarSerial.begin(RADAR_BAUD,SERIAL_8N1,RADAR_RX_PIN,-1);
#endif
#if SENSOR_MASK & (16|32)
  adcMutex=xSemaphoreCreateMutex();
  adc_oneshot_unit_init_cfg_t adcConfig{}; adcConfig.unit_id=ADC_UNIT_1;
  adcConfig.ulp_mode=ADC_ULP_MODE_DISABLE;
  if(adc_oneshot_new_unit(&adcConfig,&adcUnit)!=ESP_OK) adcUnit=nullptr;
#endif
#if SENSOR_MASK & 16
  prepareAdc(MQ2_PIN,mqChannel,mqCalibration);
#endif
#if SENSOR_MASK & 32
  pinMode(DUST_LED_PIN,OUTPUT); digitalWrite(DUST_LED_PIN,LOW);
  bool dustReady=prepareAdc(DUST_PIN,dustChannel,dustCalibration);
  int discarded=0;
  if(dustReady) adc_oneshot_read(adcUnit,dustChannel,&discarded);
  ledTimer=timerBegin(1000000);
  bool timerReady=ledTimer!=nullptr;
  if(timerReady && adcMutex && dustReady) {
    timerStop(ledTimer); timerAttachInterrupt(ledTimer,ledOff);
    dustTaskStarted=xTaskCreate(dustTask,"dust",3072,nullptr,2,nullptr)==pdPASS;
  }
  if(!dustTaskStarted && ledTimer) { timerEnd(ledTimer); ledTimer=nullptr; }
  Serial.printf("{\"type\":\"dust_init\",\"node\":%u,\"revision\":\"bc337-v2-fastadc\","
    "\"adc_ready\":%s,\"mutex_ready\":%s,\"timer_ready\":%s,\"task_ready\":%s,"
    "\"divider_gain\":%.3f}\n",NODE_ID,dustReady?"true":"false",
    adcMutex?"true":"false",timerReady?"true":"false",dustTaskStarted?"true":"false",DUST_DIVIDER_GAIN);
#endif
#endif
}
void sensorsPoll() {
#if ENABLE_SENSORS && (SENSOR_MASK & 32)
  reportDust();
#endif
#if ENABLE_SENSORS && (SENSOR_MASK & 8)
  unsigned budget=512;
  while(budget-- && radarSerial.available()) radar.feed(radarSerial.read(),millis());
#endif
#if ENABLE_SENSORS && (SENSOR_MASK & 2)
  // Retry disconnected modules once per second; allow conversion after reconnect.
  static uint32_t lastTry=0;
  if(!lightReady && uint32_t(millis()-lastTry)>=1000) {
    lastTry=millis();
    if(lightResponds(0x23)) lightAddress=0x23;
    else if(lightResponds(0x5c)) lightAddress=0x5c;
    else return;
    lightReady=light.begin(BH1750::CONTINUOUS_HIGH_RES_MODE,lightAddress,&Wire);
    lightStarted=millis();
  }
#endif
}
SensorReadings sensorsRead() {
  SensorReadings r;
#if ENABLE_SENSORS
#if SENSOR_MASK & 1
  r.humidity=dht.readHumidity(); r.temperature=dht.readTemperature();
  if(isfinite(r.humidity)&&isfinite(r.temperature)) r.valid|=1;
#endif
#if SENSOR_MASK & 2
  if(lightReady && uint32_t(millis()-lightStarted)>=180) {
    if(lightResponds(lightAddress)) r.lux=light.readLightLevel();
    if(isfinite(r.lux)&&r.lux>=0) r.valid|=2;
    else { lightReady=false; r.lux=NAN; }
  }
#endif
#if SENSOR_MASK & 4
  r.motion=digitalRead(PIR_PIN)==HIGH; r.valid|=4;
#endif
#if SENSOR_MASK & 8
  sensorsPoll();
  if(radar.available(millis(),RADAR_TIMEOUT_MS)) { r.presence=radar.present; r.valid|=8; }
#endif
#if SENSOR_MASK & 16
  if(adcMutex && mqCalibration && xSemaphoreTake(adcMutex,pdMS_TO_TICKS(20))==pdTRUE) {
    int raw=-1;
    if(adc_oneshot_read(adcUnit,mqChannel,&raw)==ESP_OK) r.mq2Voltage=calibratedVoltage(raw,mqCalibration);
    xSemaphoreGive(adcMutex);
    if(isfinite(r.mq2Voltage)) r.valid|=16;
  }
#endif
#if SENSOR_MASK & 32
  portENTER_CRITICAL(&dustLock);
  float voltage=dustVoltage; uint32_t sampled=dustAt;
  portEXIT_CRITICAL(&dustLock);
  if(isfinite(voltage)&&uint32_t(millis()-sampled)<DUST_FRESH_MS) { r.dustVoltage=voltage; r.valid|=32; }
#endif
#endif
  return r;
}
