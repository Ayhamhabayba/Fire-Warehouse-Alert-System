/* Three-warehouse fire alarm — ESP32-S3, Arduino-ESP32 3.x
   SIM800L UART, PCM5102A I2S, 3 MQ-2 analog, 15 flame inputs, 3 door relays.
   Demonstration/prototype only: not a certified life-safety controller.
   LittleFS optional WAV files: /voice1.wav, /voice2.wav, /voice3.wav
   WAV format: PCM 16-bit mono, 16000 Hz. If missing, siren still works.
*/
#include <Arduino.h>
#include <LittleFS.h>
#include <ESP_I2S.h>
#include <math.h>

constexpr uint8_t N = 3;
const int flamePins[N][5] = {{1,2,3,7,8},{9,10,12,13,14},{15,16,17,18,21}};
const int gasPins[N] = {4,5,6};
const int relayPins[N] = {11, 47, 48}; // GPIO48 often drives onboard RGB LED: verify hardware; avoid GPIO35-37 (OPI PSRAM)
const int gasThreshold[N] = {2500,2500,2500}; // PLACEHOLDER: calibrate EACH real MQ-2; ADC, NOT ppm
constexpr bool FLAME_ACTIVE_LOW = true;
constexpr bool RELAY_ACTIVE_HIGH = true; // set physical red module jumper to HIGH and verify

// SIM800L EVB: ESP TX -> modem RX; ESP RX <- modem TX. Common GND.
constexpr int GSM_RX = 41; // ESP receives from SIM800L TX
constexpr int GSM_TX = 42; // ESP sends to SIM800L RX
const char ADMIN_NUMBER[] = "+970XXXXXXXXX"; // REQUIRED: set real phone number

// ESP32-S3 -> PCM5102A I2S
constexpr int I2S_BCLK = 38; // PCM5102A BCK
constexpr int I2S_WS   = 39; // PCM5102A LCK/LRCK
constexpr int I2S_DOUT = 40; // PCM5102A DIN
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t SIREN_MS = 10000;
constexpr uint32_t VOICE_GAP_MS = 400;
constexpr uint32_t REPEAT_GAP_MS = 1200;

HardwareSerial gsm(1);
I2SClass audio;
bool audioReady = false;
bool fsReady = false;
struct Warehouse {
  bool danger=false;
  bool latched=false;
  bool doorOpen=false; // commanded relay state, NOT verified physical door position
  bool flame=false;
  int gas=0;
};
Warehouse wh[N];

// Alarm sound scheduler: voice -> siren -> next warehouse (round-robin).
enum AudioStage { AUDIO_IDLE, AUDIO_VOICE, AUDIO_SIREN, AUDIO_GAP, AUDIO_REPEAT_GAP };
AudioStage audioStage = AUDIO_IDLE;
int activeVoice = -1;
File voiceFile;
uint32_t wavBytesLeft=0;
uint32_t stageStarted=0;
uint32_t phaseIndex=0;
uint32_t nextAudioTick=0;
int lastPlayed=-1;

// GSM parser and bounded SMS FIFO. A sent SMS is confirmed by +CMGS and OK.
String gsmLine, serialLine;
String smsQueue[12];
uint8_t smsHead=0, smsTail=0, smsCount=0;
enum SmsState { SMS_IDLE, SMS_WAIT_PROMPT, SMS_WAIT_RESULT };
SmsState smsState=SMS_IDLE;
uint32_t smsStateSince=0;
bool smsGotCmgs=false;
uint32_t lastGsmInit=0;
bool gsmReady=false;
uint8_t gsmInitStep=0;
bool gsmAwaiting=false;
uint32_t gsmInitSent=0;
const char* gsmInitCommands[] = {"AT", "ATE0", "AT+CMGF=1", "AT+CSCS=\"GSM\"", "AT+CNMI=2,2,0,0,0"};
void serviceGsmInit() {
  if(gsmReady || smsState!=SMS_IDLE) return;
  if(gsmAwaiting) {
    if(millis()-gsmInitSent > 3000) {
      gsmAwaiting=false;
      gsmInitStep=0;
      lastGsmInit=millis();
      Serial.println("[GSM] Init timeout; retrying");
    }
    return;
  }
  if(gsmInitStep==0 && millis()-lastGsmInit < 2500) return;
  gsm.println(gsmInitCommands[gsmInitStep]);
  gsmAwaiting=true;
  gsmInitSent=millis();
}


void setDoor(uint8_t i, bool open) {
  digitalWrite(relayPins[i], open == RELAY_ACTIVE_HIGH ? HIGH : LOW);
  wh[i].doorOpen=open;
}
void queueSMS(const String& msg) {
  if (String(ADMIN_NUMBER).indexOf('X') >= 0) {
    Serial.println("[CONFIG REQUIRED] Set ADMIN_NUMBER. Unsent: " + msg);
    return;
  }
  if(smsCount>=12) {Serial.println("[ERROR] SMS queue full");return;}
  smsQueue[smsTail]=msg;
  smsTail=(smsTail+1)%12;
  smsCount++;
}
void modemCommand(const char* cmd) { gsm.println(cmd); }
void handleCommand(String command) {
  command.trim(); command.toLowerCase();
  if (command=="status") {
    String s="WAREHOUSES STATUS\n";
    for (int i=0;i<N;i++) s += "W"+String(i+1)+" GasADC="+String(wh[i].gas)+" Flame="+(wh[i].flame?"YES":"NO")+" Danger="+(wh[i].danger?"YES":"NO")+" Door="+(wh[i].doorOpen?"OPEN":"CLOSED")+" Alarm="+(wh[i].latched?"LATCHED":"OFF")+"\n";
    Serial.println(s); queueSMS(s); return;
  }
  if ((command.length()==5 && command.startsWith("open")) ||
      (command.length()==6 && command.startsWith("close"))) {
    int i=command[command.length()-1]-'1';
    if (i<0 || i>=N) return;
    if (command.startsWith("open")) {setDoor(i,true); queueSMS("Warehouse "+String(i+1)+" door OPEN");}
    else if (wh[i].danger || wh[i].latched) queueSMS("CLOSE DENIED W"+String(i+1)+": danger/alarm latched; clear hazard then reset"+String(i+1));
    else {setDoor(i,false); queueSMS("Warehouse "+String(i+1)+" door CLOSED");}
    return;
  }
  if (command.length()==6 && command.startsWith("reset")) {
    int i=command[5]-'1';
    if (i<0 || i>=N) return;
    if (wh[i].danger) {queueSMS("RESET DENIED W"+String(i+1)+": hazard still detected"); return;}
    wh[i].latched=false;
    queueSMS("Warehouse "+String(i+1)+" alarm RESET. Door remains open until close"+String(i+1));
    Serial.printf("[RESET] Warehouse %d; door remains %s\n",i+1,wh[i].doorOpen?"OPEN":"CLOSED");
    return;
  }
  Serial.println("Commands: status, open1..3, close1..3, reset1..3");
}
void processGsmLine(String line) {
  line.trim(); if (!line.length()) return;
  Serial.println("[GSM] " + line);
  static bool expectingBody=false;
  static bool authorized=false;
  if (line.startsWith("+CMT:")) {
    // +CMT: "+970...",...  Exact number comparison, not substring.
    int q1=line.indexOf('"'), q2=(q1>=0)?line.indexOf('"',q1+1):-1;
    String sender=(q1>=0 && q2>q1)?line.substring(q1+1,q2):"";
    authorized=(sender==String(ADMIN_NUMBER) && String(ADMIN_NUMBER).indexOf('X')<0);
    expectingBody=true;
    return;
  }
  if (expectingBody) {
    expectingBody=false;
    if (authorized) handleCommand(line);
    else Serial.println("[SECURITY] Ignored SMS from unknown sender");
    return;
  }
  if(gsmAwaiting && (line=="OK" || line=="ERROR")) {
    if(line=="OK") {
      gsmAwaiting=false;
      if(++gsmInitStep>=5) {gsmReady=true; Serial.println("[GSM] Initialized");}
    } else if(line=="ERROR") {
      gsmAwaiting=false; gsmInitStep=0; lastGsmInit=millis();
    }
    return;
  }
  if (line.startsWith("+CMGS:")) smsGotCmgs=true;
  if (smsState==SMS_WAIT_RESULT && line=="OK" && smsGotCmgs) {
    Serial.println("[SMS] Modem accepted SMS");
    smsHead=(smsHead+1)%12; smsCount--; smsState=SMS_IDLE;
  } else if (smsState!=SMS_IDLE && (line=="ERROR" || line.startsWith("+CMS ERROR"))) {
    Serial.println("[SMS] Modem error; retry later");
    smsState=SMS_IDLE;
    gsmReady=false; gsmInitStep=0; gsmAwaiting=false; lastGsmInit=millis();
    smsStateSince=millis(); // backoff managed by GSM init delay
  }
}
void serviceGsm() {
  while(gsm.available()) {
    char c=(char)gsm.read();
    if(c=='>' && smsState==SMS_WAIT_PROMPT) {
      gsm.print(smsQueue[smsHead]);gsm.write((uint8_t)26);
      smsState=SMS_WAIT_RESULT;smsStateSince=millis();smsGotCmgs=false;
    } else if(c=='\n') {processGsmLine(gsmLine);gsmLine="";}
    else if(c!='\r' && gsmLine.length()<240) gsmLine+=c;
  }
  if (smsState!=SMS_IDLE && millis()-smsStateSince>25000) {
    Serial.println("[SMS] Timeout; retry queued message");
    smsState=SMS_IDLE;smsStateSince=millis();
    gsmReady=false;gsmInitStep=0;gsmAwaiting=false;lastGsmInit=millis();
  }
  serviceGsmInit();
  if(gsmReady && smsState==SMS_IDLE && smsCount && (smsStateSince==0 || millis()-smsStateSince>=5000)) {
    gsm.print("AT+CMGS=\"");gsm.print(ADMIN_NUMBER);gsm.println("\"");
    smsState=SMS_WAIT_PROMPT;smsStateSince=millis();
  }
}

// Locate WAV data chunk; accepts only 16kHz, 16-bit, mono PCM.
bool openVoice(int i) {
  if (!fsReady || !audioReady) return false;
  String name="/voice"+String(i+1)+".wav";
  voiceFile=LittleFS.open(name,"r");
  if(!voiceFile) {Serial.println("[AUDIO] Missing "+name);return false;}
  uint8_t hdr[12];
  if(voiceFile.read(hdr,12)!=12 || memcmp(hdr,"RIFF",4) || memcmp(hdr+8,"WAVE",4)) {voiceFile.close();return false;}
  bool formatOK=false;
  while(voiceFile.available()>=8) {
    uint8_t ch[8]; if(voiceFile.read(ch,8)!=8) break;
    uint32_t size=uint32_t(ch[4]) | (uint32_t(ch[5])<<8) | (uint32_t(ch[6])<<16) | (uint32_t(ch[7])<<24);
    size_t pos=voiceFile.position();
    if(!memcmp(ch,"fmt ",4) && size>=16) {
      uint8_t fmt[16]; if(voiceFile.read(fmt,16)!=16) break;
      uint16_t codec=fmt[0] | (fmt[1]<<8), channels=fmt[2] | (fmt[3]<<8);
      uint32_t rate=uint32_t(fmt[4]) | (uint32_t(fmt[5])<<8) | (uint32_t(fmt[6])<<16) | (uint32_t(fmt[7])<<24);
      uint16_t bits=fmt[14] | (fmt[15]<<8);
      formatOK=(codec==1 && channels==1 && rate==SAMPLE_RATE && bits==16);
    }
    if(!memcmp(ch,"data",4)) {
      if(!formatOK) break;
      if (size > voiceFile.size()-voiceFile.position()) break;
      wavBytesLeft=size; return true;
    }
    if (size > voiceFile.size() || pos+size+(size&1)>voiceFile.size()) break;
    voiceFile.seek(pos+size+(size&1));
  }
  Serial.println("[AUDIO] Unsupported WAV: use 16-bit mono 16000Hz PCM");
  voiceFile.close(); return false;
}
void writeStereoFromMono(const int16_t* mono, size_t samples) {
  if(!audioReady) return;
  int16_t stereo[512];
  for(size_t j=0;j<samples;j++) {stereo[2*j]=mono[j];stereo[2*j+1]=mono[j];}
  audio.write((uint8_t*)stereo,samples*4);
}
void serviceAudio() {
  int first=-1;
  for(int step=1;step<=N;step++) {
    int i=(lastPlayed+step+N)%N;
    if(wh[i].latched) {first=i;break;}
  }
  if(first<0) {
    if(voiceFile) voiceFile.close();
    audioStage=AUDIO_IDLE; activeVoice=-1; return;
  }
  if(audioStage==AUDIO_IDLE) {
    activeVoice=first;lastPlayed=first;stageStarted=millis();
    Serial.printf("[AUDIO] VOICE: Fire detected in warehouse %d. Evacuate immediately!\n",first+1);
    if(openVoice(first)) audioStage=AUDIO_VOICE;
    else {audioStage=AUDIO_SIREN;stageStarted=millis();phaseIndex=0;nextAudioTick=millis();Serial.println("[AUDIO] SIREN START");}
    return;
  }
  if(activeVoice>=0 && !wh[activeVoice].latched && audioStage!=AUDIO_IDLE) {
    if(voiceFile) voiceFile.close();
    audioStage=AUDIO_IDLE; activeVoice=-1;
    return;
  }
  if(audioStage==AUDIO_REPEAT_GAP) {
    if(millis()-stageStarted>=REPEAT_GAP_MS) audioStage=AUDIO_IDLE;
    return;
  }
  if(audioStage==AUDIO_VOICE) {
    if(!wh[activeVoice].latched || !voiceFile || wavBytesLeft<2) {
      if(voiceFile) voiceFile.close();
      audioStage=AUDIO_GAP;stageStarted=millis();return;
    }
    int16_t mono[256];
    size_t wanted=min((uint32_t)sizeof(mono),wavBytesLeft);
    size_t got=voiceFile.read((uint8_t*)mono,wanted);
    if(got==0) {voiceFile.close();wavBytesLeft=0;return;}
    wavBytesLeft-=got;
    writeStereoFromMono(mono,got/2);
    return;
  }
  if(audioStage==AUDIO_GAP) {
    if(millis()-stageStarted>=VOICE_GAP_MS) {
      audioStage=AUDIO_SIREN;stageStarted=millis();phaseIndex=0;nextAudioTick=millis();
      Serial.println("[AUDIO] SIREN START (10 seconds)");
    }
    return;
  }
  if(audioStage==AUDIO_SIREN) {
    if(millis()-stageStarted>=SIREN_MS) {
      if(audioReady) { int16_t silence[512]={0}; audio.write((uint8_t*)silence,sizeof(silence)); }
      Serial.println("[AUDIO] SIREN END");
      audioStage=AUDIO_REPEAT_GAP;stageStarted=millis();return;
    }
    if(!audioReady) return; // timeline still simulated in Serial
    if(millis()<nextAudioTick) return;
    int16_t mono[256];
    // Alternating two-tone siren (not a real certified alarm waveform)
    uint32_t elapsed=millis()-stageStarted;
    float hz=((elapsed/550)%2==0)?750.0f:1050.0f;
    for(int j=0;j<256;j++) {
      mono[j]=(int16_t)(6500.0f*sinf(2.0f*PI*hz*(phaseIndex++)/SAMPLE_RATE));
    }
    writeStereoFromMono(mono,256);
    nextAudioTick=millis()+16;
  }
}
void serviceSensors() {
  for(int i=0;i<N;i++) {
    bool flame=false;
    for(int j=0;j<5;j++) if(digitalRead(flamePins[i][j])==(FLAME_ACTIVE_LOW?LOW:HIGH)) flame=true;
    int gas=analogRead(gasPins[i]);
    // 3 consecutive samples (~300ms) reduce single-sample false triggers.
    // This is NOT a certified fire detector; do not use it as the sole evacuation controller.
    static uint8_t flameStreak[N]={0}, gasStreak[N]={0};
    flameStreak[i]=flame ? min(3,(int)flameStreak[i]+1) : 0;
    gasStreak[i]=(gas>gasThreshold[i]) ? min(3,(int)gasStreak[i]+1) : 0;
    bool danger=flameStreak[i]>=3 || gasStreak[i]>=3;
    wh[i].flame=flame;wh[i].gas=gas;wh[i].danger=danger;
    if(danger && !wh[i].latched) {
      wh[i].latched=true;
      setDoor(i,true);
      Serial.printf("[FIRE] W%d: flame=%d, gas ADC=%d; relay OPEN commanded (not verified)\n",i+1,flame,gas);
      queueSMS("FIRE ALERT! Warehouse "+String(i+1)+". Door OPEN commanded (unverified). Evacuate immediately.");
    }
    if(danger && !wh[i].doorOpen) setDoor(i,true);
  }
}
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  for(int i=0;i<N;i++) {
    digitalWrite(relayPins[i],RELAY_ACTIVE_HIGH?LOW:HIGH);
    pinMode(relayPins[i],OUTPUT);
    pinMode(gasPins[i],INPUT);
    for(int j=0;j<5;j++) pinMode(flamePins[i][j],INPUT_PULLUP);
  }
  gsm.begin(9600,SERIAL_8N1,GSM_RX,GSM_TX);
  // Modem initialized asynchronously; each AT command must return OK.
  fsReady=LittleFS.begin(false);
  audio.setPins(I2S_BCLK,I2S_WS,I2S_DOUT);
  audioReady=audio.begin(I2S_MODE_STD,SAMPLE_RATE,I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO);
  Serial.printf("[BOOT] LittleFS=%s I2S=%s\n",fsReady?"YES":"NO",audioReady?"YES":"NO");
  Serial.println("Commands: status, open1..3, close1..3, reset1..3");
}
void loop() {
  static uint32_t lastSensors=0;
  if(millis()-lastSensors>=100) {lastSensors=millis();serviceSensors();}
  while(Serial.available()) {
    char c=Serial.read();
    if(c=='\n') {handleCommand(serialLine);serialLine="";}
    else if(c!='\r' && serialLine.length()<100) serialLine+=c;
  }
  serviceGsm();
  serviceAudio();
}
