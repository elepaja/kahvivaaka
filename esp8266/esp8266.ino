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
#include "sha256.h"
#include "secret.h"

#define UPDATETOSERVER
#define UPDATE_INTERVAL 5

ADC_MODE(ADC_VCC);

#define NTP_PORT 8888
#define NTP_PACKET_SIZE 48
#define BUTTON1_MASK 0b00000001 //PB0
#define BUTTON2_MASK 0b00001000 //PB3
#define ADC_DIVIDER 632
IPAddress timeServer(193, 166, 5, 177); // time.nist.gov NTP server
byte packetBuffer[NTP_PACKET_SIZE]; //48 = NTP packet size

//WLAN SSID (and password)
const char* ssid     = "aalto open";

//IRC related variables
const char* host = "irc.cs.hut.fi";
const String nick  = "CoffeeBot";
const String defaultChannel = "#sik_ry";
const String debugChannel = "#coffeebot";
String nickplus = "";
const int port = 6667;

const char* wwwserver = "elepaja.aalto.fi"; 

//Variables related to weight, calibration and etc.
uint32_t rawTemp = 0,temp1 = 0, weight = 0, coffeePot = 413267;
uint8_t buttons = 0, temp = 0;
float cups = 0;
volatile boolean updateNeeded = false;
boolean debug = false;
time_t lastBrewTime = 0;
enum coffeeState {
  IDLE,
  BREWING
} state;

WiFiUDP Udp;
WiFiClient client;
WiFiClient wwwclient;
Ticker timeupdateTicker;
Ticker updatewwwTicker;
Ticker cupsTicker;


//Start Wlan connection and initialize Tickers
void setup() {
  Serial.begin(9600);
  delay(10);

  WiFi.begin(ssid);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  fetchTime();
  timeupdateTicker.attach(87600, fetchTime); //Update time every day
  
  updatewwwTicker.attach(UPDATE_INTERVAL, requestUpdate);
  cupsTicker.attach(1, cupsUpdater);
}

//Ticker calls this function on 60 second intervals. Turns on updateNeeded flag which is checked at the end of the main loop.
//Also writes EEPROM to flash from RAM
void requestUpdate(){
  updateNeeded = true;
}



//This function is called every second. It is used to check if amount of cups should be updated.
void cupsUpdater(){
  static uint16_t cupsCounter = 0; //How many seconds weight has been stable?
  static uint32_t prevWeight = 0;
  
  //Value of weight must be 'stable' for 5 seconds
  if(weight < prevWeight + 3600 && weight > prevWeight - 3600) // 3600 is about 25grams
    cupsCounter++;
  else{
    cupsCounter = 0;
    //Save weight for future usei 
    prevWeight = weight;
  }
  
  //If the value of weight has been fairly stable during last 5 seconds
  if(cupsCounter > 5){
    if(weight > 0.9*coffeePot) //coffeePot placed on the scale
      cups = ((int)(weight - coffeePot))/18263.0;
    else if(cupsCounter > 180) //Pot was away over 180 seconds. Let's presume that it is too dirty to be used :P
      cups = 0;
    if(cups < 0.5) //Small amounts are irrelevant :P -Temez
      cups = 0;
  }
}

String ircstring = "";
String wwwstring = "";

//Main loop. 
void loop() {
  //Read IO data from serial
  readSerial();
  
  //If updateNeeded flag is set by Ticker -> we need to contact webserver
  if(updateNeeded && WiFi.status() == WL_CONNECTED){
    if(debug)
      client.println("PRIVMSG " + debugChannel + " :Cups: " + String(cups) + " Weight: " + String(weight) + " Temp: " + String(temp) );
    #ifdef UPDATETOSERVER
      updateValuesToServer();
    #endif
    updateNeeded = false;
  }
  
  //If we havent connected and can't reconnect -> return
  if (!client.connected() && !connect())
    return;

  //Process messages from irc server
  while(client.available()){
    char c = client.read();
    if(c == '\n'){
     processLine(ircstring);
     ircstring = "";
    }else
     ircstring += c;
  }

  while(wwwclient.available()){
    char c = wwwclient.read();
    if(wwwstring.endsWith("200 OK")){
      wwwclient.stop();
    }else
     wwwstring += c;
  }
}

//This function updates values to the server
void updateValuesToServer(){
    if (wwwclient.connect(wwwserver, 80) == 1) { // 1 = Success
      uint8_t *hash;
      Sha256.initHmac((uint8_t *)SECRET,strlen(SECRET));
      String request = "cups=" + String(cups) + "&temp=" + String(temp) + "&weight=" + String(weight) + "&int_temp=" + String(temp1) + "&rawtemp=" + String(rawTemp) + "&vcc=" + String(ESP.getVcc());
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
        if(state = BREWING)
          client.println("PRIVMSG " + channel + " :Tuloillaan!");
        else{
          String tmp = "";
          if(lastBrewTime && cups > 0){
            int minutes = (now()-lastBrewTime)/60;
            tmp = "Keitetty " + (minutes > 59? String(minutes/60) + " tuntia sitten." : String(minutes) + " minuuttia sitten.");
          }
          client.println("PRIVMSG " + channel + " :Kahvia on " + String(cups) + " kuppia jäljellä. " + tmp);
        }
      }
      else if(command == ".debug"){
        debug = !debug;
      }
      else if(command == ".kello" || command == ".clock"){
        char timeNow[17];
        sprintf(timeNow, "%04d-%02d-%02d %02d:%02d", year(), month(), day(), hour(), minute());
        client.println("PRIVMSG " + channel + " :Aika on: " + timeNow);
      }
    }
}

//Update time from NTP
void fetchTime(){
  Udp.begin(NTP_PORT);
  sendNTPpacket(timeServer);

  unsigned long starttime = millis();
  while (millis() - starttime < 10000) {
    if ( Udp.parsePacket() ) {     // We've received a packet, read the data from it
      Udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer
      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
      unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
      // combine the four bytes (two words) into a long integer
      // this is NTP time (seconds since Jan 1 1900):
      unsigned long secsSince1900 = highWord << 16 | lowWord;
      // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:u
      const unsigned long seventyYears = 2208988800UL;
      unsigned long epoch = secsSince1900 - seventyYears;
      unsigned long epochlocal = epoch + 7200;
      if (IsDST(day(epochlocal), month(epochlocal), weekday(epochlocal))) epochlocal += 3600;
      setTime(epochlocal);
    }
    if (timeStatus() == timeSet)
      break;
  }
  Udp.stop();
}

//Connects to irc server
boolean connect(){
  if (client.connect(host, port) != 1) // 1 = Success
    return false;

  //Check that nick is not in use
  boolean nickOk = false;
  nickplus = "";
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
  uint32_t c;
  c = Serial.read();
  c |= Serial.read() << 8;
  c |= Serial.read() << 16;
  c |= Serial.read() << 24;
  return c;
}

//Will be used for retrieving info from IO
//TODO: Mapping to human readable values?
int c1,c2;
void readSerial(){
  while(Serial.available() > 14){
    int c = Serial.read();
    if(c == 0xfe && c1 == 0xff && c2 == 0xff){
      buttons = Serial.read();
      weight = read32();
      rawTemp = read32();
      temp = 21 - ((int)(rawTemp/ADC_DIVIDER-553))/2.15; //Very rough calibration
      temp1 = read32();
      if(temp >  70 && state == IDLE){
        client.println("PRIVMSG " + defaultChannel + " :Kahvi keittyy nyt!");
        state = BREWING;
      }else if(temp < 55 && state == BREWING){
        client.println("PRIVMSG " + defaultChannel + " :Kahvi valmis!");
        lastBrewTime = now();
        state = IDLE;
      }
    }
    c2 = c1;
    c1 = c;
  }
}

//Following two functions handle NTP and DST related things
unsigned long sendNTPpacket(IPAddress& address)
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

boolean IsDST(int day, int month, int dow)
{
  if (month < 3 || month > 10) {
    return false;
  }
  if (month > 3 && month < 10) {
    return true;
  }
  int previousSunday = day - dow;
  if (month == 3) {
    return previousSunday >= 24;
  }
  return previousSunday <= 24;
}

