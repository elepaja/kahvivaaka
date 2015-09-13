/*
  SIK Guild Room's Coffee Scale - ESP8266 Code

  Ircbot + updates current coffee amount to sika.hut.fi.

  Initial code by Temez 2015

  "Purkkaahan se on, mut jos se toimii, niin eikös se riitä?" -Temez
  
 */
 
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WiFiUdp.h>
#include <Time.h>
#include <Ticker.h>
#include <EEPROM.h>

//#define UPDATETOSERVER

#define NTP_PORT 8888
#define NTP_PACKET_SIZE 48
#define BUTTON1_MASK 0b00000001 //PB0
#define BUTTON2_MASK 0b00001000 //PB3
#define ADC_DIVIDER 632
IPAddress timeServer(193, 166, 5, 177); // time.nist.gov NTP server
byte packetBuffer[NTP_PACKET_SIZE]; //48 = NTP packet size

const char* ssid     = "aalto open";
const char* password = "";

const char* host = "irc.cs.hut.fi";
const String nick  = "CoffeeBot";
const String defaultChannel = "#coffeebot";
String nickplus = "";
const int port = 6667;

const char* wwwserver = "sika.hut.fi"; 

unsigned int temp = 0, temp1 = 0, weight = 0, minimumWeight = -1, coffeePot = 0, maxTemp = 0;
uint8_t buttons = 0;
float cups = 0;
boolean updateNeeded = false;

WiFiUDP Udp;
WiFiClient client;
WiFiClient wwwclient;
Ticker timeupdateTicker;
Ticker updatewwwTicker;
Ticker oldcoffeeTicker;

void setup() {
  Serial.begin(9600);
  delay(10);

  EEPROM.begin(4);
  coffeePot = EEPROM.read(0);

  WiFi.begin(ssid);
  //WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  fetchTime();
  timeupdateTicker.attach(87600, fetchTime); //Update time every day
  updatewwwTicker.attach(60, requestUpdate);
  Serial.println("ready");
}

//Ticker calls this function on 60 second intervals. Turns on updateNeeded flag which is checked at the end of the main loop.
//Also writes EEPROM to flash from RAM
void requestUpdate(){
  updateNeeded = true;
  EEPROM.commit();
}

//Main loop. 
void loop() {
  if (!client.connected() && !connect())
    return;
    
  while(client.available()){
    processLine(client.readStringUntil('\r'));
  }
  readSerial();
#ifdef UPDATETOSERVER
  if(updateNeeded)
    updateValuesToServer();
#endif
}

//This function updates values to the server
void updateValuesToServer(){
    if (wwwclient.connect(wwwserver, 80)) {
      wwwclient.println("GET /update?weight=" + String(weight) + "&temp=" + String(temp) +  " HTTP/1.0"); //TODO: Change address type?
      wwwclient.println();
      unsigned long starttime = millis();
      while (millis() - starttime < 5000)
      {
        if (wwwclient.available()) {
          String line = wwwclient.readStringUntil('\r');
          if (line.indexOf("200 OK") != -1)
            break;
        }
      }
    }
    updateNeeded = false;
}

//Processes all incoming messages
void processLine(String line){
    int channelIdx = line.indexOf("PRIVMSG");
    

    if(line.startsWith("PING")){
      line[1] = 'O';
      client.println(line);
    }else if(line.indexOf("PRIVMSG") != -1){
      int messageIdx = line.indexOf(':', 1)+1;
      int channelIdxEnd = line.indexOf(' ', channelIdx+8);
      int senderIdxEnd = line.indexOf('!');
      
      String channel = line.substring(channelIdx+8, channelIdxEnd);
      String sender = line.substring(1, senderIdxEnd);
      if(channel[0] != '!' && channel[0] != '#') //Query
        channel = sender;
      if(line.indexOf(".kello",messageIdx) != -1){
        char timeNow[17];
        sprintf(timeNow, "%04d-%02d-%02d %02d:%02d", year(), month(), day(), hour(), minute());
        client.println("PRIVMSG " + channel + " :Aika on: " + timeNow);
      }
      else if(line.indexOf(".k",messageIdx) != -1 || line.indexOf(".kahvi",messageIdx) != -1 || line.indexOf(".coffee",messageIdx) != -1)
        client.println("PRIVMSG " + channel + " :Kahvia on " + String(cups) + " kuppia jäljellä.");
    }
}

//Update time from NTP
void fetchTime(){
  Udp.begin(NTP_PORT);
  sendNTPpacket(timeServer);

  long starttime = millis();
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
  if (!client.connect(host, port))
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
  return true;
}



//Function used to read 32bit number from serial stream
int read32(){
  int c = Serial.read();
  c |= Serial.read() << 8;
  c |= Serial.read() << 16;
  c |= Serial.read() << 24;
  return c;
}

enum coffeeState {
  IDLE,
  BREWING
} state;

//Will be used for retrieving info from IO
//TODO: Check order of the data.
char c,c1,c2;
void readSerial(){
  while(Serial.available()){
    c = Serial.read();
    Serial.print(c);
    if(c == 0xfe && c1 == 0xff && c2 == 0xff){
      buttons = Serial.read();
      weight = read32();
      temp = read32();
      if(temp > 80 && state == IDLE){ //TODO: Adjust temperature limit
        client.println("PRIVMSG " + defaultChannel + " :Kahvi keittyy nyt!");
        state = BREWING;
      }else if(temp < 40 && state == BREWING){ //TODO: Adjust temperature limit
        client.println("PRIVMSG " + defaultChannel + " :Kahvi valmis!");
        state = IDLE;
      }
      temp1 = read32();
      
      if(weight < minimumWeight)
        minimumWeight = weight;
      if(temp > maxTemp)
        maxTemp = temp;
      if(buttons & BUTTON1_MASK && buttons & BUTTON2_MASK){
        coffeePot = weight;
        EEPROM.write(0, coffeePot);
      }
      cups = (weight - minimumWeight - coffeePot)/10; //TODO: Measure coffeePot (or do calibration) and change divider
      cups = weight;
    }
    c2 = c1;
    c1 = c;
  }
}

//Following two functions
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

