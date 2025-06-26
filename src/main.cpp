#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <DNSServer.h>

#include "virtualHomee.hpp"

// Version und Konstanten
const double FIRMWARE_VERSION_d = 2.01;
const String FIRMWARE_VERSION = String(FIRMWARE_VERSION_d, 1);

const char* const TITLE = "Rolladen-Fernsteuerung";

// Pins für Rolladen-Steuerung
const uint8_t PIN_UP = 14;
const uint8_t PIN_STOP = 12;
const uint8_t PIN_DOWN = 13;
const uint8_t PIN_LED = 16; 

// homee Attribute IDs
const uint32_t ID_SHUTTER = 1;
const uint32_t ID_DISABLE = 2;
const uint32_t ID_SW_VER = 3;

// Access Point Konfiguration (fest)
const char* const AP_SSID = "VELUX Control";
const char* const AP_PASSWORD = "12345678";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

// EEPROM Layout
const uint16_t EEPROM_SIZE = 512;
const uint8_t EEPROM_MAGIC_BYTE = 0x42;
const uint16_t EEPROM_CFG_ADDR = 0;

// Konfigurationsstruktur
struct ConfigData 
{
    char wifi_ssid[32];
    char wifi_password[64];
    uint8_t gateway_ip[4];
    uint8_t client_ip[4];
    uint8_t subnet_mask[4];
    char homee_name[48];
    uint8_t homee_id;
    uint8_t checkValue;
};

// Globale Variablen
ConfigData config;
bool isConfigMode = false;
AsyncWebServer server(80);
DNSServer dnsServer;
virtualHomee vhih;
unsigned long lastWifiCheckTime = 0;
const unsigned long wifiCheckInterval = 30000; // Alle 30 Sekunden WLAN prüfen
bool wifiConnected = false;
unsigned long lastBlinkTime = 0;
const unsigned long blinkInterval = 500; // 500ms Blink-Intervall
bool ledState = false;
bool shutterDisabled = false;

// Funktionsprototypen
void setupConfigurationMode();
void setupControlMode();
void handleRoot(AsyncWebServerRequest *request);
void handleSave(AsyncWebServerRequest *request);
void handleRestart(AsyncWebServerRequest *request);
void handleNotFound(AsyncWebServerRequest *request);
void moveUp();
void moveDown();
void moveStop();
bool saveConfiguration();
bool loadConfiguration();
void IRAM_ATTR callBack_homeeReceiveValue(nodeAttributes* attr);
void ledOn();
void ledOff();
void ledToggle();
void ledBlink();
String loadAndProcessHTML(const String& filename);
String replaceVariables(String html);

// HTML-Template-Verarbeitung
String loadAndProcessHTML(const String& filename) {
    String html = "";
    
    if (!LittleFS.exists(filename)) {
        Serial.println("HTML file not found: " + filename);
        return "<html><body><h1>Error: Template file not found</h1><p>File: " + filename + "</p></body></html>";
    }
    
    File file = LittleFS.open(filename, "r");
    if (!file) {
        Serial.println("Failed to open HTML file: " + filename);
        return "<html><body><h1>Error: Could not open template file</h1></body></html>";
    }
    
    html = file.readString();
    file.close();
    
    // Variablen ersetzen
    html = replaceVariables(html);
    
    return html;
}

String replaceVariables(String html) {
    // Version
    html.replace("{{VERSION}}", FIRMWARE_VERSION);
    
    // WiFi-Konfiguration
    html.replace("{{WIFI_SSID}}", String(config.wifi_ssid));
    html.replace("{{WIFI_PASSWORD}}", String(config.wifi_password));
    
    // Gateway IP
    html.replace("{{GATEWAY_IP1}}", String(config.gateway_ip[0]));
    html.replace("{{GATEWAY_IP2}}", String(config.gateway_ip[1]));
    html.replace("{{GATEWAY_IP3}}", String(config.gateway_ip[2]));
    html.replace("{{GATEWAY_IP4}}", String(config.gateway_ip[3]));
    
    // Client IP
    html.replace("{{CLIENT_IP1}}", String(config.client_ip[0]));
    html.replace("{{CLIENT_IP2}}", String(config.client_ip[1]));
    html.replace("{{CLIENT_IP3}}", String(config.client_ip[2]));
    html.replace("{{CLIENT_IP4}}", String(config.client_ip[3]));
    
    // Subnet Mask
    html.replace("{{SUBNET_IP1}}", String(config.subnet_mask[0]));
    html.replace("{{SUBNET_IP2}}", String(config.subnet_mask[1]));
    html.replace("{{SUBNET_IP3}}", String(config.subnet_mask[2]));
    html.replace("{{SUBNET_IP4}}", String(config.subnet_mask[3]));
    
    // Homee-Konfiguration
    html.replace("{{HOMEE_NAME}}", String(config.homee_name));
    html.replace("{{HOMEE_ID}}", String(config.homee_id));
    
    return html;
}

bool saveConfiguration() {
    Serial.print("Saving configuration to EEPROM...");
    
    // Set the checkValue before saving
    config.checkValue = EEPROM_MAGIC_BYTE;
    
    // Write the complete configuration at once
    EEPROM.put(EEPROM_CFG_ADDR, config);
    
    // Commit the changes and check result
    bool success = EEPROM.commit();
    if(success)
    {
        Serial.println(" done.");
    }
    else
    {
        Serial.println(" FAILED!");
    }
    
    //for debugging purposes
    IPAddress gateway(config.gateway_ip[0], config.gateway_ip[1], config.gateway_ip[2], config.gateway_ip[3]);
    Serial.println("Gateway IP: " + gateway.toString());

    delay(500); // Wait for EEPROM operations
    return success;
}

bool loadConfiguration()
{
    Serial.print("Loading configuration from EEPROM...");
    
    // Load the complete configuration at once
    EEPROM.get(EEPROM_CFG_ADDR, config);

    // Check the magic byte
    if (config.checkValue != EEPROM_MAGIC_BYTE) {
        Serial.println(" no valid configuration found (checkValue: 0x" + 
                      String(config.checkValue, HEX) + ", expected: 0x" + 
                      String(EEPROM_MAGIC_BYTE, HEX) + ")");
        return false;
    }
    
    // Additional validation for WiFi credentials
    bool hasValidConfig = true;
    
    // Check if SSID is empty
    if (strlen(config.wifi_ssid) == 0) {
        Serial.println(" SSID is empty, possibly invalid configuration");
        hasValidConfig = false;
    }
    
    // Validate homee_id
    if (config.homee_id < 1 || config.homee_id > 255) {
        Serial.println(" invalid homee_id value detected");
        hasValidConfig = false;
    }
    
    if (!hasValidConfig)
    {
        return false;
    }
    
    Serial.println(" done. Configuration loaded successfully.");
    return true;
}

void handleRoot(AsyncWebServerRequest *request) {
    String html = loadAndProcessHTML("/config.html");
    request->send(200, "text/html", html);
}

void handleSave(AsyncWebServerRequest *request) {
    bool paramsFound = false;
    
    if (request->hasParam("ssid", true)) {
        request->getParam("ssid", true)->value().toCharArray(config.wifi_ssid, 32);
        paramsFound = true;
    }
    
    if (request->hasParam("password", true)) {
        request->getParam("password", true)->value().toCharArray(config.wifi_password, 64);
        paramsFound = true;
    }
    
    // Handle Gateway IP (4 separate fields)
    if (request->hasParam("gateway_ip1", true) && request->hasParam("gateway_ip2", true) && 
        request->hasParam("gateway_ip3", true) && request->hasParam("gateway_ip4", true)) {
        
        config.gateway_ip[0] = request->getParam("gateway_ip1", true)->value().toInt();
        config.gateway_ip[1] = request->getParam("gateway_ip2", true)->value().toInt();
        config.gateway_ip[2] = request->getParam("gateway_ip3", true)->value().toInt();
        config.gateway_ip[3] = request->getParam("gateway_ip4", true)->value().toInt();

        paramsFound = true;
        
        //for debugging purposes
        IPAddress gateway(config.gateway_ip[0], config.gateway_ip[1], config.gateway_ip[2], config.gateway_ip[3]);        
        Serial.println("got Gateway IP from WebForm: " + gateway.toString());
    }
    
    // Handle Client IP (4 separate fields)
    if (request->hasParam("client_ip1", true) && request->hasParam("client_ip2", true) && 
        request->hasParam("client_ip3", true) && request->hasParam("client_ip4", true)) {
        
        config.client_ip[0] = request->getParam("client_ip1", true)->value().toInt();
        config.client_ip[1] = request->getParam("client_ip2", true)->value().toInt();
        config.client_ip[2] = request->getParam("client_ip3", true)->value().toInt();
        config.client_ip[3] = request->getParam("client_ip4", true)->value().toInt();

        paramsFound = true;
        
        //for debugging purposes
        IPAddress client(config.client_ip[0], config.client_ip[1], config.client_ip[2], config.client_ip[3]);
        Serial.println("got Client IP from WebForm: " + client.toString());
    }
    
    // Handle Subnet Mask (4 separate fields)
    if (request->hasParam("subnet_ip1", true) && request->hasParam("subnet_ip2", true) && 
        request->hasParam("subnet_ip3", true) && request->hasParam("subnet_ip4", true)) {
        
        config.subnet_mask[0] = request->getParam("subnet_ip1", true)->value().toInt();
        config.subnet_mask[1] = request->getParam("subnet_ip2", true)->value().toInt();
        config.subnet_mask[2] = request->getParam("subnet_ip3", true)->value().toInt();
        config.subnet_mask[3] = request->getParam("subnet_ip4", true)->value().toInt();
        
        paramsFound = true;
    }

    if (request->hasParam("homeeName", true)) {
        request->getParam("homeeName", true)->value().toCharArray(config.homee_name, 48);
        paramsFound = true;
    }
    
    if (request->hasParam("homee_id", true)) {
        config.homee_id = request->getParam("homee_id", true)->value().toInt();
        if (config.homee_id < 1 || config.homee_id > 255) {
            config.homee_id = 1;
        }
        paramsFound = true;
    }
    
    // Nur speichern, wenn auch Parameter gefunden wurden
    bool saved = false;
    if (paramsFound) {
        saved = saveConfiguration();
        Serial.println("Parameters found and configuration saved");
    } else {
        Serial.println("No parameters found! Configuration NOT saved");
    }
    
    // HTML-Template laden und Variablen ersetzen
    String html = loadAndProcessHTML("/save_response.html");
    
    if (saved) {
        html.replace("{{STATUS_CLASS}}", "success");
        html.replace("{{MESSAGE}}", "Parameter successfully stored. The device will be restarted soon.");
    } else {
        html.replace("{{STATUS_CLASS}}", "error");
        html.replace("{{MESSAGE}}", "No changed values found or storing failed.");
    }
    
    request->send(200, "text/html", html);
    
    // Verzögerter Neustart nach dem Senden der Antwort
    delay(1000);
    if (paramsFound) ESP.restart();
}

void handleRestart(AsyncWebServerRequest *request) 
{
    String html = loadAndProcessHTML("/restart.html");
    request->send(200, "text/html", html);
    
    // Nach dem Senden neu starten
    delay(1000);
    ESP.restart();
}

void handleNotFound(AsyncWebServerRequest *request) 
{
    request->send(404, "text/plain", "Seite nicht gefunden");
}

void setupConfigurationMode() 
{
    Serial.println("Starting configuration mode, connect to AP:");
    isConfigMode = true;
    
    // LED dauerhaft einschalten im Konfigurationsmodus
    ledOn();
    
    // Access Point einrichten
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    
    Serial.println("  SSID : " + String(AP_SSID));
    Serial.println("  Password : " + String(AP_PASSWORD));
    Serial.println("  IP address: " + WiFi.softAPIP().toString());
    Serial.println("  Subnet Mask: " + String(AP_SUBNET[0]) + "." + String(AP_SUBNET[1]) + "." + String(AP_SUBNET[2]) + "." + String(AP_SUBNET[3]));
    Serial.println("");
    
    // Webserver einrichten
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/restart", HTTP_GET, handleRestart);
    
    // Update-Handler einrichten
    server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
        bool shouldReboot = !Update.hasError();
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", 
            shouldReboot ? 
            "<html><body>Update erfolgreich! Gerät startet neu...</body></html>" : 
            "<html><body>Update fehlgeschlagen!</body></html>"
        );
        response->addHeader("Connection", "close");
        request->send(response);
        if (shouldReboot) {
            delay(1000);
            ESP.restart();
        }
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!index) {
            Serial.printf("Update: %s\n", filename.c_str());
            Serial.println("Update gestartet...");
            
            // Check if the update is for the sketch or filesystem
            int cmd = U_FLASH;
            size_t updateSize = request->contentLength();
            
            Serial.printf("Update size: %u bytes\n", updateSize);
            
            // Check if we have enough space
            if (!Update.begin(updateSize, cmd)) {
                Serial.println("Not enough space for update!");
                Update.printError(Serial);
                return request->send(400, "text/plain", "Not enough space for update");
            }
        }
        
        // Write data
        if (Update.write(data, len) != len) {
            Update.printError(Serial);
            return request->send(400, "text/plain", "Write failed");
        }
            
        if (final) {
            if (Update.end(true)) {
                Serial.println("Update erfolgreich abgeschlossen");
            } else {
                Update.printError(Serial);
            }
        }
    });
    
    server.onNotFound(handleNotFound);
    
    // OTA-Update einrichten
    ArduinoOTA.setHostname("velux-rolladen");
    ArduinoOTA.onStart([]() 
    {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) 
        {
            type = "sketch";
        } 
        else 
        {
            type = "filesystem";
        }
        Serial.println("Start updating " + type);
    });
    
    ArduinoOTA.onEnd([]() 
    {
        Serial.println("\nUpdate complete");
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) 
    {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    
    ArduinoOTA.onError([](ota_error_t error) 
    {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) 
        {
            Serial.println("Auth Failed");
        } 
        else if (error == OTA_BEGIN_ERROR) 
        {
            Serial.println("Begin Failed");
        } 
        else if (error == OTA_CONNECT_ERROR) 
        {
            Serial.println("Connect Failed");
        } 
        else if (error == OTA_RECEIVE_ERROR) 
        {
            Serial.println("Receive Failed");
        } 
        else if (error == OTA_END_ERROR) 
        {
            Serial.println("End Failed");
        }
    });
    
    ArduinoOTA.begin();
    server.begin();
    Serial.println("HTTP server started");
}

// Rolladen-Steuerungsfunktionen
void moveUp() 
{   
    Serial.println("Moving up...");
    ledOn(); //simulated button pressing started
    pinMode(PIN_UP, OUTPUT_OPEN_DRAIN);  //not sure if OPEN_DRAIN works, so configure pin as output only temporarily
    digitalWrite(PIN_UP, LOW);
    delay(500);
    digitalWrite(PIN_UP, HIGH);
    pinMode(PIN_UP, INPUT);
    ledOff(); //simulated button press finished
}

void moveDown() 
{
    Serial.println("Moving down...");
    ledOn(); //simulated button pressing started
    pinMode(PIN_DOWN, OUTPUT_OPEN_DRAIN);  //not sure if OPEN_DRAIN works, so configure pin as output only temporarily
    digitalWrite(PIN_DOWN, LOW);
    delay(500);
    digitalWrite(PIN_DOWN, HIGH);
    pinMode(PIN_DOWN, INPUT);
    ledOff(); //simulated button press finished
}

void moveStop() 
{
    Serial.println("Stopping...");
    ledOn(); //simulated button pressing started
    pinMode(PIN_STOP, OUTPUT_OPEN_DRAIN);  //not sure if OPEN_DRAIN works, so configure pin as output only temporarily
    digitalWrite(PIN_STOP, LOW);
    delay(500);
    digitalWrite(PIN_STOP, HIGH);
    pinMode(PIN_STOP, INPUT);
    ledOff(); //simulated button press finished
}

// Homee-Callback-Funktion
static bool mvUp=false, mvDown=false, mvStop=false;

void IRAM_ATTR callBack_homeeReceiveValue(nodeAttributes* attr)
{
    if (attr == nullptr) 
    {
        Serial.println("Error: attr is null");
        return;
    }

    attr->setCurrentValue(attr->getTargetValue());
    vhih.updateAttribute(attr);    
    uint32_t id = attr->getId();
    double_t value = attr->getCurrentValue();

    Serial.println("Received value: " + String(value) + " for ID: " + String(id));
    
    // Je nach empfangener Nachricht die entsprechende Aktion ausführen
    if (id == ID_DISABLE)
    {
        if (value == 0)
        {
            shutterDisabled = false;
            Serial.println("Shutter enabled");
        }
        else
        {
            shutterDisabled = true;
            Serial.println("Shutter disabled");
        }
        return;
    }

    if (id != ID_SHUTTER) 
    {
        Serial.println("Unknown ID received: " + String(id));   
        return;
    }

    switch((uint8_t)value)
    {
        case 0:
            if (!shutterDisabled) mvUp = true; 
            break;
        case 1:
            if (!shutterDisabled) mvDown = true;
            break;
        case 2:
            mvStop = true;  //Stop will also work if Shutter is disabled
            break;
        default:
            Serial.println("Unknown value received: " + String(value));   
            return;
    }
}



void setupHomee() 
{
    Serial.println("Setting up homee (ID " + String(config.homee_id) + " - " + config.homee_name + ")");
    
    //config.homee_name
    node* n1 = new node(config.homee_id, 2002, config.homee_name); // 2002 = Rolladensteuerung
    nodeAttributes* attr;
    
    // Attribut: Rolladen hoch
    attr = new nodeAttributes(135, ID_SHUTTER);
    attr->setEditable(true);
    attr->setCallback(callBack_homeeReceiveValue);
    n1->AddAttributes(attr);

    // Attribut: OnOff
    attr = new nodeAttributes(1, ID_DISABLE);
    attr->setName("disabled");
    attr->setUnit("");
    attr->setCurrentValue(0.0);
    attr->setMaximumValue(1.0);
    attr->setMinimumValue(0.0);
    attr->setEditable(true);
    attr->setCallback(callBack_homeeReceiveValue);
    n1->AddAttributes(attr);
    
    // Attribut: Firmware-Version
    attr = new nodeAttributes(44, ID_SW_VER);
    attr->setName("Firmware Version");
    attr->setUnit("");
    attr->setCurrentValue(FIRMWARE_VERSION_d);
    attr->setEditable(false);
    attr->setCallback(nullptr);
    n1->AddAttributes(attr);

    // Node zur homee hinzufügen
    vhih.addNode(n1);
    
    // homee starten
    vhih.start();


    Serial.println("Homee configured");
}


void setupControlMode() 
{
    Serial.println("Starting control mode");
    isConfigMode = false;
    
    // configure pins as INPUTS so nothing happens if somebody presses keys manually
    pinMode(PIN_UP, INPUT);
    pinMode(PIN_DOWN, INPUT);
    pinMode(PIN_STOP, INPUT);
    
    // WLAN-Verbindung herstellen
    WiFi.mode(WIFI_STA);

    IPAddress gateway(config.gateway_ip[0], config.gateway_ip[1], config.gateway_ip[2], config.gateway_ip[3]);
    IPAddress client(config.client_ip[0], config.client_ip[1], config.client_ip[2], config.client_ip[3]);
    IPAddress subnet(config.subnet_mask[0], config.subnet_mask[1], config.subnet_mask[2], config.subnet_mask[3]);
    
    Serial.println("try to connect to WiFi");

    Serial.println("        SSID : " + String(config.wifi_ssid));
    Serial.println("   Gateway IP: " + gateway.toString());
    Serial.println("    Client IP: " + client.toString());
    Serial.println("  Subnet Mask: " + subnet.toString());
    Serial.println("");
    Serial.print("Connecting ");
    

    WiFi.config(client, gateway, subnet);
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) 
    {
        ledToggle(); // LED umschalten
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    ledOff(); // LED ausschalten

    if (WiFi.status() == WL_CONNECTED) 
    {
        Serial.println();
        Serial.println(" success");
        
        // Homee einrichten
        setupHomee();
    } 
    else 
    {
        Serial.println(" failed");
        Serial.println("Restarting ESP8266...");
        ESP.reset(); // ESP8266 zurücksetzen, wenn keine Verbindung hergestellt werden kann

        // Fallback auf Konfigurationsmodus
//        setupConfigurationMode();
    }

    Serial.println("Setup complete");
}


// LED-Steuerungsfunktionen
void setupLED()
{
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH); // LED aus (meist ist LOW = an bei ESP8266)
}

void ledOn()
{
    digitalWrite(PIN_LED, LOW); // LED einschalten (LOW = an beim ESP8266)
    ledState = true; // LED-Status setzen
    lastBlinkTime = millis() - blinkInterval - 1; // Zeitstempel aktualisieren
}

void ledOff()
{
    digitalWrite(PIN_LED, HIGH); // LED ausschalten (HIGH = aus beim ESP8266)
    ledState = false; // LED-Status zurücksetzen
    lastBlinkTime = millis() - blinkInterval - 1; // Zeitstempel aktualisieren    
}

void ledToggle()
{
    ledState = !ledState;
    digitalWrite(PIN_LED, ledState ? LOW : HIGH); // LED umschalten
    lastBlinkTime = millis() - blinkInterval - 1; // Zeitstempel aktualisieren
}

void ledBlink()
{
    unsigned long currentMillis = millis();
    if (currentMillis - lastBlinkTime >= blinkInterval)
    {
        lastBlinkTime = currentMillis;
        ledState = !ledState;
        digitalWrite(PIN_LED, ledState ? LOW : HIGH); // LED umschalten
    }
}


void setup() 
{
    // Serieller Monitor
    Serial.begin(74880);
    Serial.println();
    Serial.println("*******************************************");
    Serial.println("** VELUX Rolladen-Fernsteuerung, V" + String(FIRMWARE_VERSION) + " **");
    Serial.println("*******************************************");
    Serial.println("");

    setupLED(); // LED initialisieren
    ledOn(); // LED einschalten

    
    // EEPROM initialisieren
    EEPROM.begin(EEPROM_SIZE);
    
    // LittleFS initialisieren
    if (!LittleFS.begin()) 
    {
        Serial.println("Failed to mount file system");
    }
    
    // PIN_STOP zuerst als Eingang konfigurieren
    pinMode(PIN_STOP, INPUT_PULLUP);
    delay(100); // Kurze Pause für sicheres Einlesen
    
    // Konfiguration laden
    bool cfgValid = loadConfiguration();


    if (cfgValid == false) 
    {
        // Standardwerte setzen, wenn
        Serial.println("No valid configuration found, using default values.");
        
        memset(&config, 0, sizeof(ConfigData));
        strcpy(config.wifi_ssid, "");
        strcpy(config.wifi_password, "");
        
        // Gateway IP (192.168.1.1)
        config.gateway_ip[0] = 192;
        config.gateway_ip[1] = 168;
        config.gateway_ip[2] = 0;
        config.gateway_ip[3] = 1;
        
        // Client IP (192.168.1.100)
        config.client_ip[0] = 192;
        config.client_ip[1] = 168;
        config.client_ip[2] = 0;
        config.client_ip[3] = 100;
        
        // Subnet Mask (255.255.255.0)
        config.subnet_mask[0] = 255;
        config.subnet_mask[1] = 255;
        config.subnet_mask[2] = 255;
        config.subnet_mask[3] = 0;
        
        config.homee_id = 1;
        strcpy(config.homee_name, "VELUX Rolladensteuerung");

        config.checkValue = EEPROM_MAGIC_BYTE;
    }

    // Betriebsmodus bestimmen
    if ((digitalRead(PIN_STOP) == LOW) || (!cfgValid))
    {
        setupConfigurationMode();
    } 
    else 
    {
        setupControlMode();
    }
}


static bool loopFirstCall = true;
static uint32_t wifiConnectAttempts = 0; // Anzahl der Versuche, sich mit dem WLAN zu verbinden
static bool WiFi_reconnect = false;  //system is in reconnection

void loop() 
{
    if (loopFirstCall) 
    {
        loopFirstCall = false;
        Serial.println("Main loop started");
    }


    if (isConfigMode) 
    {
        ArduinoOTA.handle();
        yield(); // Wichtig für OTA-Updates;
        // Webserver wird von ESPAsyncWebServer automatisch gehandelt
        return;
    } 
    
    
    if ((millis() - lastWifiCheckTime >= wifiCheckInterval) || (WiFi_reconnect))
    {
        lastWifiCheckTime = millis();
    
        // Bei Verbindungsverlust erneut verbinden
        if (WiFi.status() != WL_CONNECTED) 
        {
            WiFi_reconnect = true;
            Serial.println("WiFi connection lost. Reconnecting...");
            ledToggle(); // LED blinken lassen, um den Verbindungsverlust anzuzeigen
            WiFi.reconnect();
            wifiConnectAttempts++;
            delay(500);
         }

        if (WiFi.status() == WL_CONNECTED)
        {
            WiFi_reconnect = false;
            wifiConnectAttempts = 0; // WLAN-Verbindung erfolgreich
            ledOff(); // LED ausschalten, wenn WLAN verbunden ist
        }

        if (wifiConnectAttempts >= 20) 
        {
            Serial.println("Failed to reconnect to WiFi after 20 attempts. Restarting ESP8266...");
            ESP.reset(); // ESP8266 zurücksetzen, wenn keine Verbindung hergestellt werden kann
        }    
    }


    if (WiFi.status() == WL_CONNECTED)
    {
        if (mvUp)
        {
            moveUp();
            mvUp = false;
        }

        if (mvDown)
        {
            moveDown();
            mvDown = false;
        }

        if (mvStop)
        {
            moveStop();
            mvStop = false;
        }
    }

     yield(); // Wichtig für ESP8266, um den Watchdog zu triggern
}