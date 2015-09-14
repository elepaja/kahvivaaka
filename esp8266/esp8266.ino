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

//#define UPDATETOSERVER

#define NTP_PORT 8888
#define NTP_PACKET_SIZE 48
#define BUTTON1_MASK 0b00000001 //PB0
#define BUTTON2_MASK 0b00001000 //PB3
#define ADC_DIVIDER 632
IPAddress timeServer(193, 166, 5, 177); // time.nist.gov NTP server
byte packetBuffer[NTP_PACKET_SIZE]; //48 = NTP packet size

//WLAN SSID (and password)
const char* ssid     = "aalto open";
const char* password = "";

//IRC related variables
const char* host = "irc.cs.hut.fi";
const String nick  = "CoffeeBot";
const String defaultChannel = "#coffeebot";
String nickplus = "";
const int port = 6667;

const char* wwwserver = "elepaja.aalto.fi"; 

//Variables related to weight, calibration and etc.
unsigned int temp = 0, temp1 = 0, weight = 0, minimumWeight = -1, coffeePot = 0, maxTemp = 0;
uint8_t buttons = 0;
float cups = 0;
boolean updateNeeded = false;
time_t lastBrewTime = 0;

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
  updatewwwTicker.attach(1, cupsUpdater);
}

//Ticker calls this function on 60 second intervals. Turns on updateNeeded flag which is checked at the end of the main loop.
//Also writes EEPROM to flash from RAM
void requestUpdate(){
  updateNeeded = true;
  EEPROM.commit();
}



//This function is called every second. It is used to check if amount of cups should be updated.
void cupsUpdater(){
  static uint8_t cupsCounter = 0; //How many seconds weight has been stable?
  static uint32_t prevWeight = 0;
  
  //If the value of weight has been fairly stable during last 5 seconds and we have 
  if(cupsCounter > 5){
    if((weight -coffeePot) > minimumWeight)
      cups = (weight - minimumWeight - coffeePot)/10; //TODO: Change divider
  }

  //Value of weight must be 'stable' for 5 seconds
  if(weight < 1.025*prevWeight && weight > 0.975*prevWeight)
    cupsCounter++;
  else
    cupsCounter = 0;

  //Save weight for future use
  prevWeight = weight;
}

//Main loop. 
void loop() {
  //Read IO data from serial
  readSerial();
    
  //If we havent connected and can't reconnect -> return
  if (!client.connected() && !connect())
    return;

  //Process messages from irc server
  while(client.available())
    processLine(client.readStringUntil('\r'));

#ifdef UPDATETOSERVER
  //If updateNeeded flag is set by Ticker -> we need to contact webserver
  if(updateNeeded)
    updateValuesToServer();
#endif
}

//This function updates values to the server
void updateValuesToServer(){
    if (wwwclient.connect(wwwserver, 80)) {
      wwwclient.println("GET /kahvi/update.php?weight=" + String(weight) + "&temp=" + String(temp) +  " HTTP/1.0"); //TODO: Change address type?
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
    //Clear flag
    updateNeeded = false;
}

//Processes all incoming messages
void processLine(String line){
    
    if(line.startsWith("PING")){
      line[1] = 'O';
      client.println(line);
    }else if(line.indexOf("PRIVMSG") != -1){
      int channelIdx = line.indexOf("PRIVMSG"); //Or actually this +8 chars
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
      else if(line.indexOf(".k",messageIdx) != -1 || line.indexOf(".kahvi",messageIdx) != -1 || line.indexOf(".coffee",messageIdx) != -1){
        String tmp = "";
        if(!lastBrewTime)
          tmp = "Kahvia ei ole keitetty edellisen rebootin jälkeen.";
        else
          tmp = "Keitetty " + String((now()-lastBrewTime)/60) + " minuuttia sitten.";
        client.println("PRIVMSG " + channel + " :Kahvia on " + String(cups) + " kuppia jäljellä. " + tmp);
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
//TODO: Mapping to human readable values?
char c,c1,c2;
void readSerial(){
  while(Serial.available()){
    c = Serial.read();
    Serial.print(c);
    if(c == 0xfe && c1 == 0xff && c2 == 0xff){
      buttons = Serial.read();
      weight = read32();
      temp = read32();
      if(temp <  300000 && state == IDLE){ //TODO: Adjust temperature limit (value gets smaller when higher temp)
        client.println("PRIVMSG " + defaultChannel + " :Kahvi keittyy nyt!");
        if(weight > minimumWeight*1.5) //We can presume that the coffee pot is at correct place at this stage
          coffeePot = weight;
        state = BREWING;
      }else if(temp > 320000 && state == BREWING){ //TODO: Adjust temperature limit (value gets bigger when lower temp)
        client.println("PRIVMSG " + defaultChannel + " :Kahvi valmis!");
        lastBrewTime = now();
        state = IDLE;
      }
      temp1 = read32();
      
      if(weight < minimumWeight)
        minimumWeight = weight;
      if(temp < maxTemp) //Max temp will be around 76-80 celcius -> this is used for calibration (we don't need exact temperature, value gets smaller when higher temp)
        maxTemp = temp;
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

