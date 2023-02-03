#include "LittleFS.h"

#include <ESP8266WiFi.h> //https://github.com/esp8266/Arduino
// needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h> //https://github.com/bblanchon/ArduinoJson
#include <AceButton.h>
#include <Ticker.h>
#include <AsyncMqttClient.h>
#include <Adafruit_NeoPixel.h>
#include <NeoPixelPainter.h>

using namespace ace_button;

#define BUTTON_PIN D2
#define LED_PIN D4
#define NUMBEROFPIXELS 6
#define PIXEL_PIN D5

AceButton button(BUTTON_PIN);
void initializeButton();
void handleEvent(AceButton *, uint8_t, uint8_t);

// set the settins for your Wifi Access Point (AP)
struct WifiSettings
{
    char apName[20] = "TotemAP";
    char apPassword[20] = "hansmeiser";
};

// Here you can pre-set the settings for the MQTT connection. The settings can later be changed via Wifi Manager.
struct MqttSettings
{
    char clientId[20] = "Totem1";
    char hostname[40] = "192.168.0.141";
    char port[6] = "1884";
    char user[20] = "Totem1";
    char password[20] = "hansmeiser";
    char wm_mqtt_client_id_identifier[15] = "mqtt_client_id";
    char wm_mqtt_hostname_identifier[14] = "mqtt_hostname";
    char wm_mqtt_port_identifier[10] = "mqtt_port";
    char wm_mqtt_user_identifier[10] = "mqtt_user";
    char wm_mqtt_password_identifier[14] = "mqtt_password";
};

// save config to file
bool shouldSaveConfig = false;

// topic to send the mqtt message to
const char *humidity_topic = "/humidity";
const char *buttonStatus_topic = "/buttonStatus";
char topicButtonStatus[30];

// Declaration of objects
WiFiClient espClient;
WifiSettings wifiSettings;
MqttSettings mqttSettings;
AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;

Adafruit_NeoPixel neopixels = Adafruit_NeoPixel(NUMBEROFPIXELS, PIXEL_PIN, NEO_GRB + NEO_KHZ800);

// create one canvas and one brush with global scope
NeoPixelPainterCanvas pixelcanvas = NeoPixelPainterCanvas(&neopixels); // create canvas, linked to the neopixels (must be created before the brush)
NeoPixelPainterBrush pixelbrush = NeoPixelPainterBrush(&pixelcanvas);  // crete brush, linked to the canvas to paint to
HSV brushcolor;

void readSettingsFromConfig()
{
    // clean FS for testing
    //  LittleFS.format();

    // read configuration from FS json
    Serial.println("mounting FS...");

    if (LittleFS.begin())
    {
        Serial.println("mounted file system");
        if (LittleFS.exists("/config.json"))
        {
            // file exists, reading and loading
            Serial.println("reading config file");
            File configFile = LittleFS.open("/config.json", "r");
            if (configFile)
            {
                Serial.println("opened config file");
                size_t size = configFile.size();
                // Allocate a buffer to store contents of the file.
                std::unique_ptr<char[]> buf(new char[size]);

                configFile.readBytes(buf.get(), size);
                // Use arduinojson.org/v6/assistant to compute the capacity.
                StaticJsonDocument<1024> doc;
                DeserializationError error = deserializeJson(doc, configFile);
                if (error)
                {
                    Serial.println(F("Failed to read file, using default configuration"));
                }
                else
                {
                    Serial.println("\nparsed json");

                    strcpy(mqttSettings.clientId, doc[mqttSettings.wm_mqtt_client_id_identifier]);
                    strcpy(mqttSettings.hostname, doc[mqttSettings.wm_mqtt_hostname_identifier]);
                    strcpy(mqttSettings.port, doc[mqttSettings.wm_mqtt_port_identifier]);
                    strcpy(mqttSettings.user, doc[mqttSettings.wm_mqtt_user_identifier]);
                    strcpy(mqttSettings.password, doc[mqttSettings.wm_mqtt_password_identifier]);

                    // generate topics
                    strcpy(topicButtonStatus, mqttSettings.clientId);
                    strcat(topicButtonStatus, buttonStatus_topic);
                }
            }
        }
    }
    else
    {
        Serial.println("failed to mount FS");
    }
}

// callback notifying us of the need to save config
void saveConfigCallback()
{
    Serial.println("Should save config");
    shouldSaveConfig = true;
}

void initializeWifiManager()
{
    // The extra parameters to be configured (can be either global or just in the setup)
    // After connecting, parameter.getValue() will get you the configured value
    // id/name placeholder/prompt default length
    WiFiManagerParameter custom_mqtt_client_id("client_id", "mqtt client id", mqttSettings.clientId, 40);
    WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqttSettings.hostname, 40);
    WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqttSettings.port, 6);
    WiFiManagerParameter custom_mqtt_user("user", "mqtt user", mqttSettings.user, 20);
    WiFiManagerParameter custom_mqtt_pass("pass", "mqtt pass", mqttSettings.password, 20);

    // WiFiManager
    // Local intialization. Once its business is done, there is no need to keep it around
    WiFiManager wifiManager;

    // Reset Wifi settings for testing
    //  wifiManager.resetSettings();

    // set config save notify callback
    wifiManager.setSaveConfigCallback(saveConfigCallback);

    // set static ip
    //  wifiManager.setSTAStaticIPConfig(IPAddress(10,0,1,99), IPAddress(10,0,1,1), IPAddress(255,255,255,0));

    // add all your parameters here
    wifiManager.addParameter(&custom_mqtt_client_id);
    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_port);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_pass);

    // reset settings - for testing
    // wifiManager.resetSettings();

    // set minimum quality of signal so it ignores AP's under that quality
    // defaults to 8%
    // wifiManager.setMinimumSignalQuality();

    // sets timeout until configuration portal gets turned off
    // useful to make it all retry or go to sleep
    // in seconds
    // wifiManager.setTimeout(120);
    if (button.isPressedRaw())
    {
        Serial.println(F("setup(): button was pressed while booting. Start config portal "));
        if (!wifiManager.startConfigPortal(wifiSettings.apName, wifiSettings.apPassword))
        {
            Serial.println("failed to connect and hit timeout");
            delay(3000);
            // reset and try again, or maybe put it to deep sleep
            ESP.reset();
            delay(5000);
        }
    }

    // fetches ssid and pass and tries to connect
    // if it does not connect it starts an access point with the specified name
    // here  "AutoConnectAP"
    // and goes into a blocking loop awaiting configuration
    if (!wifiManager.autoConnect(wifiSettings.apName, wifiSettings.apPassword))
    {
        Serial.println("failed to connect and hit timeout");
        delay(3000);
        // reset and try again, or maybe put it to deep sleep
        ESP.reset();
        delay(5000);
    }

    // if you get here you have connected to the WiFi
    Serial.println("connected to:)");
    Serial.println(WiFi.SSID());

    // read updated parameters
    strcpy(mqttSettings.clientId, custom_mqtt_client_id.getValue());
    strcpy(mqttSettings.hostname, custom_mqtt_server.getValue());
    strcpy(mqttSettings.port, custom_mqtt_port.getValue());
    strcpy(mqttSettings.user, custom_mqtt_user.getValue());
    strcpy(mqttSettings.password, custom_mqtt_pass.getValue());
}

void saveConfig()
{
    Serial.println("saving config");
    StaticJsonDocument<1024> doc;
    doc[mqttSettings.wm_mqtt_client_id_identifier] = mqttSettings.clientId;
    doc[mqttSettings.wm_mqtt_hostname_identifier] = mqttSettings.hostname;
    doc[mqttSettings.wm_mqtt_port_identifier] = mqttSettings.port;
    doc[mqttSettings.wm_mqtt_user_identifier] = mqttSettings.user;
    doc[mqttSettings.wm_mqtt_password_identifier] = mqttSettings.password;

    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile)
    {
        Serial.println("failed to open config file for writing");
    }

    configFile.close();
}

void ledBlink()
{
    digitalWrite(LED_PIN, LOW);
    delay(250);
    digitalWrite(LED_PIN, HIGH);
    delay(250);
}

void initializeButton()
{
    ButtonConfig *buttonConfig = button.getButtonConfig();
    buttonConfig->setEventHandler(handleEvent);
    buttonConfig->setFeature(ButtonConfig::kFeatureClick);
    buttonConfig->setFeature(ButtonConfig::kFeatureDoubleClick);
    buttonConfig->setFeature(ButtonConfig::kFeatureLongPress);
    buttonConfig->setFeature(ButtonConfig::kFeatureRepeatPress);
}

// The event handler for the button.
void handleEvent(AceButton * /* button */, uint8_t eventType,
                 uint8_t buttonState)
{
    // Print out a message for all events.
    Serial.print(F("handleEvent(): eventType: "));
    Serial.print(eventType);
    Serial.print(F("; buttonState: "));
    Serial.println(buttonState);

    // Control the LED only for the Pressed and Released events.
    // Notice that if the MCU is rebooted while the button is pressed down, no
    // event is triggered and the LED remains off.
    switch (eventType)
    {
    case AceButton::kEventPressed:
        digitalWrite(LED_PIN, LOW);
        brushcolor.h = 20;
        pixelbrush.setColor(brushcolor);
        break;
    case AceButton::kEventReleased:
        digitalWrite(LED_PIN, HIGH);
        break;
    case AceButton::kEventLongPressed:
        digitalWrite(LED_PIN, HIGH);
        brushcolor.h = 80;
        pixelbrush.setColor(brushcolor);
        break;
    }
}

void connectToMqtt()
{
    Serial.println("Connecting to MQTT...");
    mqttClient.connect();
}

void onMqttConnect(bool sessionPresent)
{
    Serial.println("Connected to MQTT.");
    Serial.print("Session present: ");
    Serial.println(sessionPresent);
    uint16_t packetIdSub = mqttClient.subscribe("test/lol", 2);
    Serial.print("Subscribing at QoS 2, packetId: ");
    Serial.println(packetIdSub);
    mqttClient.publish("test/lol", 0, true, "test 1");
    Serial.println("Publishing at QoS 0");
    uint16_t packetIdPub1 = mqttClient.publish("test/lol", 1, true, "test 2");
    Serial.print("Publishing at QoS 1, packetId: ");
    Serial.println(packetIdPub1);
    uint16_t packetIdPub2 = mqttClient.publish("test/lol", 2, true, "test 3");
    Serial.print("Publishing at QoS 2, packetId: ");
    Serial.println(packetIdPub2);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
    Serial.println("Disconnected from MQTT.");

    if (WiFi.isConnected())
    {
        mqttReconnectTimer.once(2, connectToMqtt);
    }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos)
{
    Serial.println("Subscribe acknowledged.");
    Serial.print("  packetId: ");
    Serial.println(packetId);
    Serial.print("  qos: ");
    Serial.println(qos);
}

void onMqttUnsubscribe(uint16_t packetId)
{
    Serial.println("Unsubscribe acknowledged.");
    Serial.print("  packetId: ");
    Serial.println(packetId);
}

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
{
    Serial.println("Publish received.");
    Serial.print("  topic: ");
    Serial.println(topic);
    Serial.print("  qos: ");
    Serial.println(properties.qos);
    Serial.print("  dup: ");
    Serial.println(properties.dup);
    Serial.print("  retain: ");
    Serial.println(properties.retain);
    Serial.print("  len: ");
    Serial.println(len);
    Serial.print("  index: ");
    Serial.println(index);
    Serial.print("  total: ");
    Serial.println(total);
}

void onMqttPublish(uint16_t packetId)
{
    Serial.println("Publish acknowledged.");
    Serial.print("  packetId: ");
    Serial.println(packetId);
}
void initializePixel()
{
    pinMode(PIXEL_PIN, OUTPUT);

    neopixels.begin();

    if (pixelcanvas.isvalid() == false)
        Serial.println(F("canvas allocation problem (out of ram, reduce number of pixels)"));
    else
        Serial.println(F("canvas allocation ok"));

    if (pixelbrush.isvalid() == false)
        Serial.println(F("brush allocation problem"));
    else
        Serial.println(F("brush allocation ok"));

    // initialize the animation, this is where the magic happens:

    brushcolor.h = 0;   // zero is red in HSV. Library uses 0-255 instead of 0-360 for colors (see https://en.wikipedia.org/wiki/HSL_and_HSV)
    brushcolor.s = 255; // full color saturation
    brushcolor.v = 130; // about half the full brightness

    pixelbrush.setSpeed(2000);       // set the brush movement speed (4096 means to move one pixel per update)
    pixelbrush.setColor(brushcolor); // set the brush color
    pixelbrush.setFadeSpeed(200);    // fading speed of pixels (255 max, 200 is fairly fast)
    pixelbrush.setFadeout(true);     // do brightness fadeout after painting
    pixelbrush.setBounce(true);      // bounce the brush when it reaches the end of the strip
}
void setup()
{
    Serial.begin(115200);
    Serial.println();
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(2, OUTPUT);

    // Button uses the built-in pull up register.
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    readSettingsFromConfig();
    initializeWifiManager();
    initializeButton();
    initializePixel();

    if (shouldSaveConfig)
    {
        saveConfig();
    }

    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onSubscribe(onMqttSubscribe);
    mqttClient.onUnsubscribe(onMqttUnsubscribe);
    mqttClient.onMessage(onMqttMessage);
    mqttClient.onPublish(onMqttPublish);
    mqttClient.setServer(mqttSettings.hostname, atoi(mqttSettings.port));
    if (strlen(mqttSettings.user) != 0)
    {
        mqttClient.setCredentials(mqttSettings.user, mqttSettings.password);
    }

    connectToMqtt();
}

void loop()
{
    button.check();
    neopixels.clear(); // always need to clear the pixels, the canvas' colors will be added to whatever is on the pixels before calling an canvas update

    pixelbrush.paint();     // paint the brush to the canvas (and update the brush, i.e. move it a little)
    pixelcanvas.transfer(); // transfer the canvas to the neopixels

    neopixels.show();
    // ledBlink();
}