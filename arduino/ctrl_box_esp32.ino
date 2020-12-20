/****************************************************************************************************************************************************************
  The majority of this code is cobbled together from the various sketches provided by the manufacturer of each piece of equipment or from the Arduino forums.

  Notes:
  ----------------------------------------------------------------------------------------------------------------------------------------------------------------
  - The ESP32 is responsible for relaying all data between the Mega and Home Assistant via MQTT, since the Mega has no WiFi capabilities.
  - All references to Serial are for printing to console only. Serial2 is the actual wired connection to the Mega in the control box.
  - For my pumps, flow rates are as follows: 300ms "on" time = 1mL, 500ms = 1.5mL, 1000ms = 3.25mL, 1500ms = 5mL, 2000ms = 6.75mL, 2500ms = 8.5mL, 3000ms = 10mL.
    - Voltage at V+/V- terminals on 12V PSU was 13.045V when pump flow rates were measured.
    - Starting PWM values were: phDownSpeed = 210, calMagSpeed = 204, microSpeed = 211, bloomSpeed = 206, growSpeed = 205, phUpSpeed = 210, noctuaFanSpeed = 125.
  - Dosing pump speeds are set by Home Assistant when the ESP32 connects via MQTT.
  - The ESP32 will send serial commands to the Mega to poll the Atlas Scientific sensors at a preset interval (atlasPeriod), alternating between them. It will send
    a temperature compensation value for the AS sensors based on the measured temperature of the mixing res solution at a preset interval as well (tempCompPeriod).
******************************************************************************************************************************************************************/

#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#define NUM_ELEMENTS(x)  (sizeof(x) / sizeof((x)[0]))
#define ONE_WIRE_BUS 4
#define BUILTIN_LED 2
#define NOCTUA 6                                      // PWM pin for fan that cools control box

Adafruit_BME280 bme;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);                  // Pass our oneWire reference to Dallas DS18B20 Temperature sensor
char bmeBuffer[16];                                   // For reading BME280 in control box

float celcius = 0;
char waterTemp[16];

// Timing
unsigned long currentMillis;                         // Grab a snapshot of current millis each pass through void loop()
unsigned long tempCompMillis = millis();             // Timer for sending temperature compensation factor to EZO pH circuit
unsigned long tempCheckMillis = millis();            // Timer to check control box temp and mix reservoir solution temp
unsigned long atlasMillis = millis();                // Timer for polling the Atlas EC and pH circuits for readings
unsigned long dosingPumpMillis[6];                   // Timer to check to see if pump needs to be shut off
const unsigned long tempCheckPeriod = 30000;         // How long to wait, in milliseconds, between checking box/water temp
const unsigned long tempCompPeriod = 600000;         // How long to wait, in milliseconds, between sending temperature compensation factor to ezo pH circuit
unsigned long atlasPeriod = 5000;                    // How long to wait, in milliseconds, between polling Atlas sensors for values
unsigned long dosingPumpPeriod[6];                   // Array of modifiable "pump on" times that are sent by Home Assistant. These determine how long to keep pump on before shutting off.

// WiFi/MQTT/Serial
const char* ssid = "YOUR___SSID";
const char* password = "YOUR___PASS";
const char* mqtt_server = "YOUR___MQTTSERVERIP";
WiFiClient espClient;
PubSubClient client(espClient);
const byte numChars = 100;
char receivedChars[numChars];
boolean newData = false;                            //Is there new data coming in over the serial port from the Arduino Mega?

// Atlas
boolean pHCalledLast = false;                       // Tracks whether pH was polled last or EC.
boolean stopReadings = false;                       // I set this flag to tell system to stop taking readings at certain points when calibrating sensors to avoid errors.

// GPIO Pin numbers for Enable
const int dosingPumpEnablePin[6]
{
  19, 33, 26, 14, 13, 23
};

// GPIO Pin numbers for PWM
const int dosingPumpPWMpin[6]
{
  18, 32, 25, 27, 12, 5
};
const int pwmNoctuaFanPin = 15;

// PWM properties
const int freq = 5000;
const int resolution = 8;
const int pwmChannel[7]                       // This array has 7 due to 6 Dosing pumps + the noctua fan in the control box. Noctua pwm rate never changes though.
{
  0, 1, 2, 3, 4, 5, 6
};
int pumpSpeeds[6];


void setup_wifi()
{
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length)
{
  char payloadStr[length + 1];              // Create a char array that's 1 byte longer than the incoming payload to copy it to and make room for the null terminator so it can be treated as string.
  memcpy(payloadStr, payload, length);
  payloadStr[length + 1] = '\0';

  /***************** CALLBACK: 8-Channel Relay Board (In Control Box) *****************/

  if (strcmp(topic, "control/relays") == 0) // Incoming message format will be <BOARD#>:<RELAY#>:<STATE>. STATE is "1" for on, "0" for off. Example payload: "1:1:0" = on board 1, turn relay 1 ON.
  {
    Serial2.print("<Relay:");               // Print this command to the Mega since it handles the relays.
    Serial2.print(payloadStr);
    Serial2.println('>');
  }

  /***************** CALLBACK: Dosing *****************/
  if (strcmp(topic, "control/dosing") == 0)   // Incoming message format will be <PUMP#>:<ONTIME>. ONTIME is in milliseconds.
  {
    int pumpNumber;
    long onTime;
    char* strtokIndx;

    strtokIndx = strtok(payloadStr, ":");     // Get the pump number
    pumpNumber = atoi(strtokIndx);
    strtokIndx = strtok(NULL, ":");
    onTime = atol(strtokIndx);              // Get the on time

    Serial.println(pumpNumber);
    Serial.println(onTime);
    setPumpPower(pumpNumber, onTime);
  }

  /***************** CALLBACK: Pump Speed Adjustments *****************/

  if (strcmp(topic, "calibrate/dosing") == 0)
  {
    int pumpNumber;
    int pwmVal;
    char* strtokIndx;

    strtokIndx = strtok(payloadStr, ":");    // Get the pump number
    pumpNumber = atoi(strtokIndx);
    strtokIndx = strtok(NULL, ",");
    pwmVal = atoi(strtokIndx);              // Get the PWM val

    setPumpSpeeds(pumpNumber, pwmVal);
  }
  
  /***************** CALLBACK: pH Calibration *****************/

  if (strcmp(topic, "calibrate/atlas_pH") == 0)           // pH cal values of 7.00, 4.00, and 10.00 are hard coded to match Atlas calibration solutions. Change these if you're using different solutions.
  {
    switch (payloadStr[0])                                  // Payload will be "mid", "low", or "high". Switching on first char to tell which it is.
    {
      case 'm':
        {
          stopReadings = true;
          delay(2000);
          Serial2.println("<99:Cal,mid,7.00>");
          atlasMillis = millis();
          stopReadings = false;
          break;
        }
      case 'l':
        {
          stopReadings = true;
          delay(2000);
          Serial2.println("<99:Cal,low,4.00>");
          atlasMillis = millis();
          stopReadings = false;
          break;
        }
      case 'h':
        {
          stopReadings = true;
          delay(2000);
          Serial2.println("<99:Cal,high,10.00>");
          delay(2000);
          Serial2.println("<99:Cal,?>");               //I'm asking the EZO pH circuit here how many points it has calibrated. To know I was successful, I'm looking for an answer of 3.
          atlasMillis = millis();
          stopReadings = false;
          break;
        }
    }
  }

  /***************** CALLBACK: EC Calibration *****************/

  if (strcmp(topic, "calibrate/atlas_EC") == 0)           // EC cal values of 700 & 2000 are hard coded to match Atlas calibration solutions. Change these if you're using diff solutions.
  {
    switch (payloadStr[0])                                   // Payload will be "dry", "low", or "high". Switching on first char to tell which it is.
    {
      case 'd':
        {
          stopReadings = true;
          delay(2000);
          Serial2.println("<100:Cal,dry>");
          delay(1000);
          atlasMillis = millis();
          stopReadings = false;
          break;
        }
      case 'l':
        {
          stopReadings = true;
          delay(2000);
          Serial2.println("<100:Cal,low,700>");
          delay(1000);
          atlasMillis = millis();
          stopReadings = false;
          break;
        }
      case 'h':
        {
          stopReadings = true;
          delay(2000);
          Serial2.println("<100:Cal,high,2000>");
          delay(2000);
          Serial2.println("<100:Cal,?>");              // Again, how many points of calibration?
          atlasMillis = millis();
          stopReadings = false;
          break;
        }
    }
  }

  /***************** CALLBACK: Mixing Res HX711 Scale Calibration *****************/

  if (strcmp(topic, "calibrate/scale") == 0)
  {
    switch (payloadStr[0])                   // Payload will be "Begin", "Exit", or a calibration factor for adjustment. Switching on first char to tell which it is.
    {
      case 'B':
        {
          Serial2.println("<HX711: Begin>");
          break;
        }
      case 'E':
        {
          Serial2.println("<HX711: Exit>");
          break;
        }
      default:
        {
          Serial2.print("<HX711: ");
          Serial2.println(payloadStr);
          break;
        }
    }
  }
}

void reconnect()
{
  byte mqttFailCount = 0;
  byte tooManyFailures = 10;
  // Loop until we're reconnected
  while (!client.connected())
  {
    if (mqttFailCount <= tooManyFailures)
    {
      Serial.print("Attempting MQTT connection...");
      if (client.connect("YOUR___ID", "YOUR___USERNAME", "YOUR___PASSWORD"))
      {
        delay(1000);
        client.publish("feedback/general", "Garden controller connecting...");
        delay(1000);
        client.publish("feedback/general", "Garden controller connected.");
        digitalWrite(BUILTIN_LED, HIGH);

        client.subscribe("control/relays");
        client.subscribe("control/dosing");
        client.subscribe("calibrate/atlas_pH");
        client.subscribe("calibrate/atlas_EC");
        client.subscribe("calibrate/dosing");
        client.subscribe("calibrate/scale");
      }

      else
      {
        digitalWrite(BUILTIN_LED, LOW);
        mqttFailCount ++;
        Serial.print("Failed. Count = ");
        Serial.println(mqttFailCount);
        Serial.println("...trying again in 5 seconds");
        // Wait 5 seconds before retrying
        delay(5000);
      }
    }
    else
    {
      Serial.print(tooManyFailures);
      Serial.println(" MQTT failures in a row. Resetting WiFi connection.");
      WiFi.disconnect();
      delay(5000);
      setup_wifi();
      mqttFailCount = 0;
    }
  }
}

void setup()
{
  char buff[60];
  Serial.begin(115200);
  Serial2.begin(115200);
  Serial.read();
  Serial2.read();
  for (int i = 0; i < NUM_ELEMENTS(dosingPumpEnablePin); i++)
  {
    pinMode(dosingPumpEnablePin[i], OUTPUT);
  }
  pinMode(BUILTIN_LED, OUTPUT);
  setup_wifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  sensors.begin();
  bme.begin(0x76);
  getWaterTemp();
  getBoxTemp();

  //Configure LED PWM functionalitites
  for (int i = 0; i < NUM_ELEMENTS(dosingPumpPWMpin); i++)   // The condition in for loop is -1 because the noctua fan pwm channel is the last one and we will update it separately below
  {
    ledcSetup(pwmChannel[i], freq, resolution);
    ledcAttachPin(dosingPumpPWMpin[i], pwmChannel[i]);
  }
  ledcAttachPin(pwmNoctuaFanPin, pwmChannel[6]);
  ledcWrite(pwmChannel[NOCTUA], 125);
}

void loop()                                                                           // I'm using millis() to try to keep my loop running as fast as possible. I tried to avoid having any "delay(x)" lines, which block the program.
{
  currentMillis = millis();
  if (!client.connected())
  {
    reconnect();
  }

  if (currentMillis - tempCheckMillis >= tempCheckPeriod)                             // Is it time to check temps?
  {
    getWaterTemp();
    getBoxTemp();
    tempCheckMillis = currentMillis;
  }

  if ((currentMillis - atlasMillis > atlasPeriod) && (stopReadings == false))         // Is it time to read the EZO circuits?
  {
    if (currentMillis - tempCompMillis > tempCompPeriod)                              // Is it time to compensate for temperature?
    {
      if ((celcius >= 10) && (celcius <= 30))                                         // Make sure I'm not getting a garbage reading from temp sensor prior to sending temp compensation to pH circuit
      {
        Serial2.print("<99:T,");
        Serial2.print(waterTemp);
        Serial2.println('>');
        tempCompMillis = millis();
        atlasMillis = millis();
      }
    }
    else if (pHCalledLast == false)                                                 // If pH circuit was not read last, read it.
    {
      Serial2.println("<99:R>");
      pHCalledLast = true;
      atlasMillis = millis();
    }
    else                                                                            // Otherwise read EC.
    {
      Serial2.println("<100:R>");
      pHCalledLast = false;
      atlasMillis = millis();
    }
  }

  for (int i = 0; i < NUM_ELEMENTS(dosingPumpPeriod); i++)
  {
    if ((dosingPumpPeriod[i] > 0) && (currentMillis - dosingPumpMillis[i] >= dosingPumpPeriod[i]))      // If pump is on and its timer has expired...
    {
      setPumpPower(i, 0);                                                                               // Shut it off by setting timer to 0.
    }
  }
  
  recvWithStartEndMarkers();                                            // Gather data sent from Mega over serial
  processSerialData();
  client.loop();                                                       // MQTT client loop is required at end of void loop().
}

void recvWithStartEndMarkers()                                        // Function to receive serial data from Mega in format of "<MESSAGE>". Thanks Robin2!
{
  static boolean recvInProgress = false;
  static byte ndx = 0;
  char startMarker = '<';
  char endMarker = '>';
  char rc;

  while (Serial2.available() > 0 && newData == false)
  {
    rc = Serial2.read();

    if (recvInProgress == true)
    {
      if (rc != endMarker)
      {
        receivedChars[ndx] = rc;
        ndx++;
        if (ndx >= numChars)
        {
          ndx = numChars - 1;
        }
      }
      else
      {
        receivedChars[ndx] = '\0'; //Terminate the string
        recvInProgress = false;
        ndx = 0;
        newData = true;
      }
    }

    else if (rc == startMarker)
    {
      recvInProgress = true;
    }
  }
}

void processSerialData()
{
  if (newData == true)
  {
    client.publish("feedback/debug", receivedChars);
    char commandChar = receivedChars[0];
    switch (commandChar)
    {
      case 'P':                                                                                      // If message starts with 'P' (pH)
        {
            char* strtokIndx;
            strtokIndx = strtok(receivedChars, ":");                                                // Skip the first segment which is the identifier
            strtokIndx = strtok(NULL, ":");
            client.publish("feedback/atlas_pH", strtokIndx);
          if (receivedChars[8] == '3')
          {
            client.publish("feedback/general", "pH Cal Successful!");   // When we ask the pH EZO circuit above how many points it has calibrated, if it responds with a 3 here, publish success message to HA.
          }
          break;
        }

      case 'E':                                                                                     // If message starts with 'E' (EC)
        {
            char* strtokIndx;
            strtokIndx = strtok(receivedChars, ":");                                                 // Skip the first segment which is the identifier;
            strtokIndx = strtok(NULL, ":");
            client.publish("feedback/atlas_EC", strtokIndx);
          if (receivedChars[8] == '2')                                                            // EC is considered 2 point calibration for some reason (dry, low, high)
          {
            client.publish("feedback/general", "EC Cal Successful!");
          }
          break;
        }

      case 'F':                                                       // If message starts with F (Flood)
        {
          client.publish("feedback/flood", receivedChars);
          break;
        }

      case 'W':
        {
          char* strtokIndx;
          strtokIndx = strtok(receivedChars, ":");                   // Skip the first segment 
          strtokIndx = strtok(NULL, ":");
          client.publish("feedback/waterLevel", strtokIndx);
          break;
        }

      case 'B':                                                      // Drain basin float sensor status
        {
          client.publish("feedback/drainBasin", receivedChars);
          break;
        }

      case 'H':                                                      // HX711 calibration status (mixing res scale)
        {
          client.publish("feedback/hx711", receivedChars);
          break;
        }

      case 'R':                                                      // Relay feedback. Message format is "Relay FB:<BOARD#>:<RELAY#>:<STATUS>". Example: "Relay FB:0:4:1"
        {
          int boardNumber;
          int relayNumber;
          int relayPower;
          char* strtokIndx;  
          char buff[18];
      
          strtokIndx = strtok(receivedChars, ":");                   // Skip the first segment which is the 'R'
          strtokIndx = strtok(NULL, ":");                            // Get the board number
          boardNumber = atoi(strtokIndx);
          strtokIndx = strtok(NULL, ":");                            // Get the relay number
          relayNumber = atoi(strtokIndx);  
          strtokIndx = strtok(NULL, ":");                            // Get the relay power state
          relayPower = atoi(strtokIndx);
          
          sprintf(buff, "%d:%d:%d", boardNumber, relayNumber, relayPower);
          client.publish("feedback/relays",buff);
          Serial.println(buff);
          break;
        }
    }
    newData = false;
  }
}

void getWaterTemp()
{
  float f = celcius;
  sensors.requestTemperatures();
  celcius = sensors.getTempCByIndex(0);
  snprintf(waterTemp, sizeof(waterTemp), "%.1f", f);
  if (waterTemp[0] != '-')
  {
    client.publish("feedback/waterTemp", waterTemp);
  }
}

void getBoxTemp()
{
  dtostrf(bme.readTemperature(), 3, 1, bmeBuffer);
  client.publish("feedback/boxTemp", bmeBuffer);
}

void setPumpSpeeds(int pumpNumber, int pumpSpeed)
{
  ledcWrite(pwmChannel[pumpNumber], pumpSpeed);
}

void setPumpPower(int pumpNumber, long onTime)
{
  char buff[5];
  digitalWrite(dosingPumpEnablePin[pumpNumber], onTime);           // If onTime is > 0, write the pin high. Otherwise write it low.
  if (onTime > 0)
  {
    dosingPumpMillis[pumpNumber] = millis();                      // If pump is being turned on, start the timer
    dosingPumpPeriod[pumpNumber] = onTime;
  }
  else
  {
    dosingPumpMillis[pumpNumber] = 0;                           // If pump is being turned off, zero millis/period out.
    dosingPumpPeriod[pumpNumber] = 0;
  }
  sprintf(buff, "%d:%d", pumpNumber, onTime > 0);
  Serial.println(buff);
  client.publish("feedback/dosing", buff);                        // Send feedback (<PUMP#>:<STATE>)
}
