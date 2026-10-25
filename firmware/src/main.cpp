// ReMix v0.2 prototype. Faders carry DC position only, never audio.
#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <Bounce.h>

AudioInputI2SQuad analogIn;
AudioInputUSB usbIn;
AudioMixer4 mixL, mixR, sendL, sendR;
AudioAmplifier mainL, mainR, phonesL, phonesR;
AudioOutputI2SQuad analogOut;
AudioOutputUSB usbOut;
AudioControlSGTL5000 codec;
AudioConnection c01(analogIn,0,mixL,0), c02(analogIn,1,mixR,0);
AudioConnection c03(analogIn,2,mixL,1), c04(analogIn,3,mixR,1);
AudioConnection c05(usbIn,0,mixL,2), c06(usbIn,1,mixR,2);
AudioConnection c07(analogIn,0,sendL,0), c08(analogIn,1,sendR,0);
AudioConnection c09(analogIn,2,sendL,1), c10(analogIn,3,sendR,1);
AudioConnection c11(usbIn,0,sendL,2), c12(usbIn,1,sendR,2);
AudioConnection c13(mixL,mainL), c14(mixR,mainR);
AudioConnection c15(mixL,phonesL), c16(mixR,phonesR);
AudioConnection c17(mainL,0,analogOut,0), c18(mainR,0,analogOut,1);
AudioConnection c19(phonesL,0,analogOut,2), c20(phonesR,0,analogOut,3);
AudioConnection c21(sendL,0,usbOut,0), c22(sendR,0,usbOut,1);

const uint8_t faderPins[4]={A0,A2,A3,A8}; // 14,16,17,22
const uint8_t buttonPins[4]={0,1,2,3};
const uint8_t muteLED[4]={26,27,28,29};
Bounce b0(0,20),b1(1,20),b2(2,20),b3(3,20);
Bounce* buttons[4]={&b0,&b1,&b2,&b3};
int lowEnd[4]={0,0,0,0},highEnd[4]={4095,4095,4095,4095};
bool calibrated[4]={false,false,false,false};
float filtered[4]={0,0,0,0},previous[4]={0,0,0,0};
float audioLevel[4]={0,0,0,0},appLevel[4]={0,0,0,0};
bool audioMute[4]={true,true,true,true},appMute[4]={false,false,false,false};
bool picked[4]={false,false,false,false};
bool appMode=false,useUSB=false,fullSend=false,audioOK=false;
unsigned long sampleAt=0,reportAt=0,chordAt=0;
bool chordUsed=false,armedChord=false;
char commandLine[96]; size_t commandSize=0; bool discardCommand=false;

bool mux(uint8_t port){
  Wire.beginTransmission(0x70); Wire.write(1u<<port);
  return Wire.endTransmission()==0;
}
bool codecStart(uint8_t port){
  if(!mux(port) || !codec.enable()) return false;
  if(!codec.inputSelect(AUDIO_INPUT_LINEIN)) return false;
  codec.lineInLevel(0); // minimum gain; external pad still required
  codec.lineOutLevel(29); // modest initial level, measure before changing
  codec.volume(port==1 ? 0.30f : 0.0f);
  if(port==0) codec.muteHeadphone();
  else codec.muteLineout();
  return true;
}
bool ready(){for(bool c:calibrated) if(!c) return false; return audioOK;}
float normalise(uint8_t i,float raw){
  float v=(raw-lowEnd[i])/float(highEnd[i]-lowEnd[i]);
  return constrain(v,0.0f,1.0f);
}
float taper(float x){return x<0.005f ? 0.0f : powf(10.0f,(-60.0f+60.0f*x)/20.0f);}
void setMode(bool app){
  appMode=app;
  for(int i=0;i<4;i++){picked[i]=false;previous[i]=normalise(i,filtered[i]);}
}
void updateAudio(){
  // 0.5 per source guarantees <= unity for two coherent full-scale inputs.
  float pc=ready()&&!audioMute[0] ? 0.5f*taper(audioLevel[0]) : 0;
  float aux=ready()&&!audioMute[1] ? 0.5f*taper(audioLevel[1]) : 0;
  float targetMain=ready()&&!audioMute[2]?taper(audioLevel[2]):0;
  float targetHP=ready()&&!audioMute[3]?taper(audioLevel[3]):0;
  // All audio gains slew over roughly 50 ms to reduce control clicks.
  static float pcG=0,auxG=0,mainG=0,hpG=0;
  pcG+=0.08f*(pc-pcG);auxG+=0.08f*(aux-auxG);
  mainG+=0.08f*(targetMain-mainG);hpG+=0.08f*(targetHP-hpG);
  AudioNoInterrupts();
  for(AudioMixer4* m:{&mixL,&mixR}){
    m->gain(0,useUSB?0:pcG);m->gain(1,auxG);m->gain(2,useUSB?pcG:0);m->gain(3,0);
  }
  for(AudioMixer4* m:{&sendL,&sendR}){
    m->gain(0,fullSend&&!useUSB?pcG:0);m->gain(1,auxG);
    m->gain(2,fullSend&&useUSB?pcG:0);m->gain(3,0);
  }
  mainL.gain(mainG);mainR.gain(mainG);phonesL.gain(hpG);phonesR.gain(hpG);
  AudioInterrupts();
}
void execute(char* s){
  int i,lo,hi; char tail;
  if(!strcmp(s,"MODE APP")) setMode(true);
  else if(!strcmp(s,"MODE AUDIO")) setMode(false);
  else if(!strcmp(s,"SOURCE USB")) useUSB=true;
  else if(!strcmp(s,"SOURCE ANALOG")) useUSB=false;
  else if(!strcmp(s,"SEND FULL")) fullSend=true;
  else if(!strcmp(s,"SEND CALL")) fullSend=false;
  else if(sscanf(s,"CAL %d %d %d %c",&i,&lo,&hi,&tail)==3 && i>=0&&i<4&&lo>=0&&lo<=4095&&hi>=0&&hi<=4095&&abs(hi-lo)>=512){
    lowEnd[i]=lo;highEnd[i]=hi;calibrated[i]=true;picked[i]=false;
  }else {Serial.println("ERR command");return;}
  Serial.println("OK");
}
void setup(){
  analogReadResolution(12);analogReadAveraging(16);
  for(int i=0;i<4;i++){
    pinMode(buttonPins[i],INPUT_PULLUP);pinMode(muteLED[i],OUTPUT);
    filtered[i]=analogRead(faderPins[i]);
  }
  pinMode(24,OUTPUT);pinMode(25,OUTPUT);
  Wire.begin();Wire.setClock(100000);
  AudioMemory(80);
  // Both shield SCL/SDA pairs MUST be isolated behind separate mux ports.
  bool a=codecStart(0),b=codecStart(1); audioOK=a&&b;
  if(!audioOK) Serial.println("ERR codec-or-mux");
  updateAudio();
}
void loop(){
  while(Serial.available()){
    char ch=Serial.read();
    if(ch=='\n'){
      if(!discardCommand){commandLine[commandSize]=0;execute(commandLine);}
      else Serial.println("ERR line-too-long");
      commandSize=0;discardCommand=false;
    }else if(ch!='\r'){
      if(commandSize<sizeof(commandLine)-1&&!discardCommand) commandLine[commandSize++]=ch;
      else discardCommand=true;
    }
  }
  if(millis()-sampleAt<5) return;sampleAt=millis();
  for(auto b:buttons)b->update();
  bool chord=buttons[0]->read()==LOW&&buttons[3]->read()==LOW;
  if(chord){
    if(!armedChord){armedChord=true;chordAt=millis();}
    if(!chordUsed&&millis()-chordAt>=1200){setMode(!appMode);chordUsed=true;}
  }
  // Buttons 1 and 4 trigger on release. A mode chord suppresses both mutes.
  for(int i=0;i<4;i++){
    if(buttons[i]->risingEdge()&&!chordUsed&&!armedChord){
      bool* m=appMode?appMute:audioMute;m[i]=!m[i];
    }
  }
  if(buttons[0]->read()==HIGH&&buttons[3]->read()==HIGH){armedChord=false;chordUsed=false;}
  float* level=appMode?appLevel:audioLevel;
  for(int i=0;i<4;i++){
    filtered[i]+=0.18f*(analogRead(faderPins[i])-filtered[i]);
    float v=normalise(i,filtered[i]);
    if(calibrated[i]){
      if(fabsf(v-level[i])<0.02f||(previous[i]-level[i])*(v-level[i])<=0)picked[i]=true;
      if(picked[i])level[i]=v;
    }
    previous[i]=v;digitalWrite(muteLED[i],(appMode?appMute:audioMute)[i]);
  }
  digitalWrite(24,!appMode);digitalWrite(25,appMode);
  updateAudio();
  if(Serial&&millis()-reportAt>=50){
    reportAt=millis();
    Serial.printf("{\"v\":1,\"mode\":\"%s\",\"ready\":%s,\"source\":\"%s\",\"send\":\"%s\",\"raw\":[%d,%d,%d,%d],\"app\":[%.4f,%.4f,%.4f,%.4f],\"mute\":[%d,%d,%d,%d]}\n",appMode?"APP":"AUDIO",ready()?"true":"false",useUSB?"USB":"ANALOG",fullSend?"FULL":"CALL",(int)filtered[0],(int)filtered[1],(int)filtered[2],(int)filtered[3],appLevel[0],appLevel[1],appLevel[2],appLevel[3],appMute[0],appMute[1],appMute[2],appMute[3]);
  }
  while(usbMIDI.read()){} // Composite interface present; serial carries controls.
}
