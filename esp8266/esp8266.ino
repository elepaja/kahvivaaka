/*
  SIK Guild Room's Coffee Scale - ESP8266 Code

  Ircbot + updates current coffee amount to elepaja.aalto.fi.

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
#include <EEPROM.h>
#include "median.h"
#include "sha256.h" //HMAC-SHA256-functions
#include "secret.h" //Contains #define SECRET "password".

#define UPDATETOSERVER
#define MEDIAN_LENGTH 20
#define MEDIAN_AVERAGE_N 5
ADC_MODE(ADC_VCC);

#define NTP_PORT 8888
#define NTP_PACKET_SIZE 48
#define BUTTON1_MASK 0b00000001 //PB0
#define BUTTON2_MASK 0b00001000 //PB3
#define ADC_DIVIDER 632
const IPAddress timeServer(193, 166, 5, 177); // time.nist.gov NTP server
byte packetBuffer[NTP_PACKET_SIZE]; //48 = NTP packet size

//WLAN SSID (and password)
#define SSID "aalto open"
//#define PASSWORD "optional"

//IRC related variables
const char* host = "irc.cs.hut.fi";
const String nick  = "CoffeeBot";
const String defaultChannel = "#coffeebot"; //"#sik_ry";
const String debugChannel = "#coffeebot";
const int port = 6667;

const char* wwwserver = "elepaja.aalto.fi"; 

//Variables related to calibration and etc.
uint32_t emptyScale = 0, coffeePot = 0;
uint8_t buttons = 0;

unsigned long lastWWWUpdate = 0;
unsigned long weightChanged = 0;
time_t lastBrewTime = 0;

enum coffeeState {
  IDLE,
  BREWING,
  CALIBRATING_COFFEEPOT,
  CALIBRATING_EMPTYSCALE,
  WARMING,
  COOLING
};

struct dataset {
  int16_t weight;
  uint32_t rawWeight;
  float temp;
  float cups;
  uint32_t rawTemp;
  uint32_t rawTemp1;
  uint16_t espVCC;
  uint32_t avrVCC;
  coffeeState state;
} ;

dataset current;
dataset previousUpdated;

boolean debug = false;
boolean reboot = true;

FastRunningMedian<uint32_t, MEDIAN_LENGTH, 0> rawWeightMedian;
FastRunningMedian<uint32_t, MEDIAN_LENGTH, 0> rawtempMedian;
FastRunningMedian<uint32_t, MEDIAN_LENGTH, 0> rawTemp1Median;
FastRunningMedian<uint32_t, MEDIAN_LENGTH, 0> coffeePotMedian;
FastRunningMedian<uint32_t, MEDIAN_LENGTH, 0> emptyScaleMedian;


WiFiUDP Udp;
WiFiClient client;
WiFiClient wwwclient;
Ticker timeupdateTicker;

//Start Wlan connection and initialize Tickers
void setup() {
  Serial.begin(9600);
  
  EEPROM.begin(8);
  for(int i = 0; i < 4; ++i){
    coffeePot |= EEPROM.read(i) << i*8;
    emptyScale |= EEPROM.read(4+i) << i*8;
  }
  
  //Populate medians with values from EEPROM
  for(int i = 0; i < MEDIAN_LENGTH; ++i)
    coffeePotMedian.addValue(coffeePot);
  for(int i = 0; i < MEDIAN_LENGTH; ++i)
    emptyScaleMedian.addValue(emptyScale);

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

//This function is used to check if amount of cups should be updated.
void cupsUpdater(){
  // Weight has changed significantly?
  if((current.weight > previousUpdated.weight + 15 || current.weight < previousUpdated.weight - 15) && millis()-weightChanged > 1000){
    weightChanged = millis();
    updateValuesToServer(current);
  }
  
  //If the value of weight has been fairly stable during last 4 seconds (notice ratelimit, which is 1s) -> update cups
  if(millis()-weightChanged > 5000){
    float tmp = current.weight/125.0;
    if(current.weight > -200 && (tmp < previousUpdated.cups-0.1 || tmp > previousUpdated.cups+0.1)){ //coffeePot placed on the scale
      if(tmp < 0.5)
        current.cups = 0;
      else
        current.cups = tmp;
      updateValuesToServer(current);
    }
    else if((current.weight <= -200 && current.cups != 0 && millis() - weightChanged > 300000)){ //Pot was away over 300 seconds. Let's presume that it is too dirty to be used :P
      current.cups = 0;
      updateValuesToServer(current);
    }
  }
}

String ircstring = "";
String wwwstring = "";

//This function updates values to the server
void updateValuesToServer(struct dataset d){
    previousUpdated = d;
    if(WiFi.status() != WL_CONNECTED || d.state == CALIBRATING_COFFEEPOT || d.state == CALIBRATING_EMPTYSCALE) //Check that wifi is connected
      return;
    lastWWWUpdate = millis();
    if (wwwclient.connect(wwwserver, 80) == 1) { // 1 = Success
      uint8_t *hash;
      Sha256.initHmac((uint8_t *)SECRET,strlen(SECRET));
      String request = "timestamp=" + String(now()) + "&cups=" + String(d.cups) + "&temp=" + String(d.temp) + 
                       "&weight=" + String(d.rawWeight) + "&weightgrams=" + String(d.weight) +
                       "&int_temp=" + String(d.rawTemp1) + "&rawtemp=" + String(d.rawTemp) + "&espvcc=" + String(d.espVCC) + "&avrvcc=" + String(d.avrVCC);
      Sha256.print(request);
      hash = Sha256.resultHmac();

      wwwclient.print("GET /kahvi/update.php?" + request + "&hash=");
      for (int i=0; i<32; i++) {
        wwwclient.print("0123456789abcdef"[hash[i]>>4]);
        wwwclient.print("0123456789abcdef"[hash[i]&0xf]);
      }
      wwwclient.println(" HTTP/1.0");
      wwwclient.println("Host: " + String(wwwserver));
      wwwclient.println();
    }
    //TODO: Save results to file system if no connection (and check that we really have new data to send?)
}

//Main loop. 
void loop(){ 
  //Read IO data from serial
  readSerial();

  yield(); //This yield migh be optional

  //Check if amount of cups should be updated
  cupsUpdater();

  yield();
  
#ifdef UPDATETOSERVER
  if((millis()-lastWWWUpdate > 8*60*1000) || ((debug || current.state != previousUpdated.state) && millis()-lastWWWUpdate > 5*1000))
    updateValuesToServer(current); 
  while(wwwclient.read() != -1)
    yield();  
  if (!wwwclient.connected())
    wwwclient.stop();
#endif

  yield(); //This yield migh be optional
  
  //If we havent connected and can't reconnect -> return
  if (!client.connected() && !connect())
    return;

  yield(); //This yield might be optional
  
  //Process messages from irc server
  while(client.available()){
    char c = client.read();
    if(c == '\n'){
     processLine(ircstring);
     ircstring = "";
    }else
     ircstring += c;
    yield();
  }
}

//Processes all incoming messages
void processLine(String line){
    line.trim(); //Ensure that we have no extra whitespaces
        
    if(line.startsWith("PING")){
      line[1] = 'O';
      client.println(line);
    }else if(line.indexOf("PRIVMSG") != -1){
      int messageIdx = line.indexOf(':', 1)+1;
      String command = line.substring(messageIdx);
      command.trim();
      
      if(command[0] != '.') //Don't do extra work if the message isn't a command
        return;

      //Find indexes of various parts
      int channelIdx = line.indexOf("PRIVMSG")+8;
      int channelIdxEnd = line.indexOf(' ', channelIdx);
      int senderIdxEnd = line.indexOf('!');

      //Split original line to smaller strings
      String channel = line.substring(channelIdx, channelIdxEnd);
      String sender = line.substring(1, senderIdxEnd);   
      
      //If this is a query -> we need to answer to the sender
      if(channel[0] != '!' && channel[0] != '#') //Query
        channel = sender;

      if(command == ".k" || command ==".kahvi" || command == ".coffee" || command == ".kaffe"){
        if(current.state == BREWING)
          client.println("PRIVMSG " + channel + " :Tuloillaan!");
        else{
          String tmp = "Kahvia on " + String(current.cups) + " kuppia jäljellä.";
          if(lastBrewTime && current.cups > 0){
            int minutes = (now()-lastBrewTime)/60;
            int hours = minutes/60;
            tmp += " Keitetty " + (hours > 0? String(hours) + " h sitten." : String(minutes) + " min sitten.");
          }else if(current.cups == 0){
            tmp = "Pannu on tyhjä :(";
          }
          client.println("PRIVMSG " + channel + " :" + tmp);
        }
      }
      else if(command == ".debug" && channel == debugChannel){
        debug = !debug;
        client.println("PRIVMSG " + channel + " :Debug:" + String(debug));
      }
      else if(command == ".kello" || command == ".clock"){
        char timeNow[24];
        sprintf(timeNow, "%04d-%02d-%02d %02d:%02d:%02d UTC", year(), month(), day(), hour(), minute(), second());
        client.println("PRIVMSG " + channel + " :Aika on: " + timeNow);
      }else if (command == ".calibrate" && channel == debugChannel){ 
        if(current.state != CALIBRATING_EMPTYSCALE && current.state != CALIBRATING_COFFEEPOT && millis() < 30000){ //Implement some authentication, this is just temporary
          current.state = CALIBRATING_EMPTYSCALE;
          client.println("PRIVMSG " + debugChannel + " :Calibrating EMPTY Coffeepot");
        }
        else if(current.state == CALIBRATING_EMPTYSCALE){
          current.state = CALIBRATING_COFFEEPOT;
          client.println("PRIVMSG " + debugChannel + " :Calibrating FULL Coffeepot");
        }
        else if(current.state == CALIBRATING_COFFEEPOT){
          current.state = IDLE;
          client.println("PRIVMSG " + debugChannel + " :Calibration stopped!");
        }
      }
    }
}

//Update time from NTP 
void maintenance(){
  if(!reboot){
    for(int i = 0; i < 4; ++i){
      EEPROM.write(i, (coffeePot >> 8*i) & 0xFF);
      EEPROM.write(4+i, (emptyScale >> 8*i) & 0xFF);
    }
    EEPROM.commit();
  }
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

//Connects to irc server
boolean connect(){
  if (client.connect(host, port) != 1) // 1 = Success
    return false;

  //Check that nick is not in use
  boolean nickOk = false;
  String nickplus = "";
  while(!nickOk){
    client.println("NICK " + nick + nickplus);
    delay(1000);
    if(client.available()){
      String line = client.readStringUntil('\r');
      if(line.indexOf("Nickname is already in use.") != -1)
        nickplus = nickplus + '_';
    }else{
      nickOk = true;
    }
  }
  client.println("USER " + nick + " 8 * : " + nick);
  client.println("JOIN " + defaultChannel);
  client.println("JOIN " + debugChannel);
  return true;
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
      current.weight = map(current.rawWeight,emptyScale,coffeePot+1,-815,0); //+1 to ensure that there is no division by zero if no previous

      rawtempMedian.addValue(read32());
      current.rawTemp = rawtempMedian.getAverage(MEDIAN_AVERAGE_N);
      current.temp = 21 - ((int)(current.rawTemp-312980))*1100.0/1023/2.329568788501027/ADC_DIVIDER; //Very rough calibration

      rawTemp1Median.addValue(read32());
      current.rawTemp1 = rawTemp1Median.getAverage(MEDIAN_AVERAGE_N);

      current.avrVCC = read32();
      current.espVCC = ESP.getVcc();
      if(!reboot){
        if(current.temp > 60 && current.state != BREWING){
          current.state = BREWING;
        }else if(current.temp > 30 && current.state == IDLE){
          coffeePotMedian.addValue(current.rawWeight);
          coffeePot = coffeePotMedian.getAverage(MEDIAN_AVERAGE_N);
          current.state = WARMING;
        }else if(current.temp < 55 && current.state == BREWING){
          client.println("PRIVMSG " + defaultChannel + " :Kahvi valmis!");
          lastBrewTime = now();
          current.state = COOLING;
        }else if(current.state == CALIBRATING_EMPTYSCALE){
          emptyScaleMedian.addValue(current.rawWeight);
          emptyScale = emptyScaleMedian.getAverage(MEDIAN_AVERAGE_N);
        }else if(current.state == CALIBRATING_COFFEEPOT){
          coffeePotMedian.addValue(current.rawWeight);
          coffeePot = coffeePotMedian.getAverage(MEDIAN_AVERAGE_N);
        }else if (current.temp < 28)
          current.state = IDLE;
      }
      ++count;
    }
    c2 = c1;
    c1 = c;
    yield();
  }
  return count;
}

//Following two functions handle NTP and DST related things
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

/*
boolean IsDST(int day, int month, int dow)
{
  if (month < 3 || month > 10) {
    return false;
  }
  if (month > 3 && month < 10) {
    return true;
  }
  int previousUpdatedSunday = day - dow;
  if (month == 3) {
    return previousUpdatedSunday >= 24;
  }
  return previousUpdatedSunday <= 24;
}*/

