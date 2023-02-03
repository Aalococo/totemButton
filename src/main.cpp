#include "LittleFS.h"

#include <ESP8266WiFi.h> //https://github.com/esp8266/Arduino
// needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h> //https://github.com/bblanchon/ArduinoJson
#include <PubSubClient.h>
#include <AceButton.h>
using namespace ace_button;

#define BUTTON_PIN D2
#define LED_PIN D4

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
    char hostname[40] = "192.168.0.142";
    char port[6] = "1883";
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
char topicButtonStatus[30];

WiFiClient espClient;
WifiSettings wifiSettings;
PubSubClient client(espClient);
MqttSettings mqttSettings;

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

void initializeMqttClient()
{
    Serial.println("local ip");
    Serial.println(WiFi.localIP());
    client.setServer(mqttSettings.hostname, atoi(mqttSettings.port));
}

bool tryConnectToMqttServer()
{
    if (strlen(mqttSettings.user) == 0)
    {
        return client.connect(mqttSettings.clientId);
    }
    else
    {
        return client.connect(mqttSettings.clientId, mqttSettings.user, mqttSettings.password);
    }
}

void reconnect()
{
    // Loop until we're reconnected
    while (!client.connected())
    {
        Serial.print("Attempting MQTT connection...");
        // Attempt to connect
        // If you do not want to use a username and password, change next line to

        if (tryConnectToMqttServer())
        {
            Serial.println("connected");
        }
        else
        {
            Serial.print(mqttSettings.hostname);
            Serial.print(" failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 5 seconds");
            // Wait 5 seconds before retrying
            delay(5000);
        }
    }
}

void publishMessage()
{
    Serial.print("Publish to");
    Serial.print(topicButtonStatus);
    Serial.println("OK");
    client.publish(topicButtonStatus, "OK", true);
}

void ledBlink()
{
    digitalWrite(LED_PIN, LOW);
    delay(250);
    digitalWrite(LED_PIN, HIGH);
    delay(250);
}

void handleMqttState()
{
    if (!client.connected())
    {
        reconnect();
    }
    client.loop();
}

#include <button.h>

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
        break;
    case AceButton::kEventReleased:
        digitalWrite(LED_PIN, HIGH);
        break;
    }
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

    if (shouldSaveConfig)
    {
        saveConfig();
    }

    initializeMqttClient();
}

void loop()
{
    handleMqttState();
    publishMessage();
    button.check();
    // ledBlink();
}