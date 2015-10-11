/*
  SIK Guild Room's Coffee Scale - ESP8266 Code

  Updates current coffee amount to elepaja.aalto.fi.

  Initial code by Temez 2015

  "Purkkaahan se on, mut jos se toimii, niin eikös se riitä?" -Temez

  Build tools etc for Arduino IDE https://github.com/esp8266/Arduino
  Boards manager link: http://arduino.esp8266.com/stable/package_esp8266com_index.json
  
 */
 
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WiFiUdp.h>
#include <Time.h>
#include <Ticker.h>
#include "median.h"
#include "sha256.h" //HMAC-SHA256-functions
#include "secret.h" //Contains #define SECRET "password".


//WLAN SSID (and password)
#define SSID "aalto open"
//#define PASSWORD "optional"
#define NTP_PORT 8888
#define NTP_PACKET_SIZE 48
#define MEDIAN_LENGTH 20
#define MEDIAN_AVERAGE_N 5
ADC_MODE(ADC_VCC);

WiFiUDP Udp;
Ticker timeupdateTicker;

const IPAddress timeServer(130, 233, 224, 52); // ntp1.aalto.fi
byte packetBuffer[NTP_PACKET_SIZE]; //48 = NTP packet size
const char* wwwserver = "elepaja.aalto.fi"; 
uint8_t buttons = 0;

unsigned long lastWWWUpdate = 0;

struct dataset {
  uint32_t rawWeight;
  uint32_t rawTemp;
  uint32_t rawTemp1;
  uint16_t espVCC;
  uint32_t avrVCC;
} ;

dataset current;
boolean reboot = true;

FastRunningMedian<uint32_t, MEDIAN_LENGTH, 0> rawWeightMedian;
FastRunningMedian<uint32_t, MEDIAN_LENGTH, 0> rawtempMedian;
FastRunningMedian<uint32_t, MEDIAN_LENGTH, 0> rawTemp1Median;

WiFiClient wwwclient;

//Start Wlan connection
void setup() {
  Serial.begin(9600);
  delay(10);
  
#ifndef PASSWORD
  WiFi.begin(SSID);
#else
  WiFi.begin(SSID,PASSWORD);
#endif

  uint8_t i= 0;
  while(i < MEDIAN_LENGTH*2){
    i += readSerial();
    yield();
  }
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
  }

  maintenance();
  timeupdateTicker.attach(87600, maintenance); //Update time every day

  reboot = false;
}

String wwwstring = "";

//This function updates values to the server
void updateValuesToServer(struct dataset d){
    if(WiFi.status() != WL_CONNECTED) //Check that wifi is connected
      return;
    lastWWWUpdate = millis();
    if (wwwclient.connected() || wwwclient.connect(wwwserver, 80) == 1) { // 1 = Success
      uint8_t *hash;
      Sha256.initHmac((uint8_t *)SECRET,strlen(SECRET));
      String request = "timestamp=" + String(now()) + "&weight=" + String(d.rawWeight) + "&int_temp=" + String(d.rawTemp1) +
                       "&temp=" + String(d.rawTemp) + "&espvcc=" + String(d.espVCC) +    "&avrvcc=" + String(d.avrVCC);
      Sha256.print(request);
      hash = Sha256.resultHmac();

      wwwclient.print("GET /kahvi/updater.php?" + request + "&hash=");
      for (int i=0; i<32; i++) {
        wwwclient.print("0123456789abcdef"[hash[i]>>4]);
        wwwclient.print("0123456789abcdef"[hash[i]&0xf]);
      }
      wwwclient.println(" HTTP/1.0");
      wwwclient.println("Host: " + String(wwwserver));
      wwwclient.println("Connection: keep-alive");
      wwwclient.println();
    }
}

//Main loop. 
void loop(){ 
  //Read IO data from serial
  readSerial();
  
  while(wwwclient.read() != -1)
    yield();
  
  if(millis()-lastWWWUpdate > 1000 && !wwwclient.connected())
    updateValuesToServer(current);
  
  if(!wwwclient.connected())
    wwwclient.stop();
}

//Function used to read 32bit number from serial stream
uint32_t read32(){
  uint32_t c = 0;
  c = Serial.read();
  c |= Serial.read() << 8;
  c |= Serial.read() << 16;
  c |= Serial.read() << 24;
  return c;
}

//Will be used for retrieving info from IO
int c1 = 0, c2 = 0;
uint8_t readSerial(){
  uint8_t count = 0;
  while(Serial.available() > 14){
    int c = Serial.read();
    if(c == 0xfe && c1 == 0xff && c2 == 0xff){
      buttons = Serial.read();

      rawWeightMedian.addValue(read32());
      current.rawWeight = rawWeightMedian.getAverage(MEDIAN_AVERAGE_N);

      rawtempMedian.addValue(read32());
      current.rawTemp = rawtempMedian.getAverage(MEDIAN_AVERAGE_N);
      //current.temp = 21 - ((int)(current.rawTemp-312980))*1100.0/1023/2.329568788501027/ADC_DIVIDER; //Very rough calibration

      rawTemp1Median.addValue(read32());
      current.rawTemp1 = rawTemp1Median.getAverage(MEDIAN_AVERAGE_N);

      current.avrVCC = read32();
      current.espVCC = ESP.getVcc();
      ++count;
    }
    c2 = c1;
    c1 = c;
    yield();
  }
  return count;
}

unsigned long sendNTPpacket(const IPAddress& address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

void maintenance(){
  Udp.begin(NTP_PORT);
  sendNTPpacket(timeServer);

  unsigned long starttime = millis();
  while (millis() - starttime < 5000) {
    if (Udp.parsePacket()) {     // We've received a packet, read the data from it
      Udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer
      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
      unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
      // combine the four bytes (two words) into a long integer
      // this is NTP time (seconds since Jan 1 1900):
      unsigned long secsSince1900 = highWord << 16 | lowWord;
      // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:u
      const unsigned long seventyYears = 2208988800UL;
      unsigned long epoch = secsSince1900 - seventyYears;
      unsigned long epochlocal = epoch;
      //if (IsDST(day(epochlocal), month(epochlocal), weekday(epochlocal))) epochlocal += 3600;
      setTime(epochlocal);
      if (timeStatus() == timeSet)
        break;
    }
    yield(); //Allow wifi stack to run
  }
  Udp.stop();
}
