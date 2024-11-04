#include "CustomImprovWifi.h"

WiFiServer server(80);
ImprovWiFi improvSerial(&Serial);
CustomImprovWifi* CustomImprovWifi::instance = nullptr;  // Initialize the instance pointer

CustomImprovWifi::CustomImprovWifi() {
    instance = this;  // Set the instance pointer to this object
}

void CustomImprovWifi::setupImprov() {
    ImprovTypes::ChipFamily detectedChipFamily;
    #if defined(ESP32)
        #if CONFIG_IDF_TARGET_ESP32
            detectedChipFamily = ImprovTypes::ChipFamily::CF_ESP32;
        #elif CONFIG_IDF_TARGET_ESP32C3
            detectedChipFamily = ImprovTypes::ChipFamily::CF_ESP32_C3;
        #elif CONFIG_IDF_TARGET_ESP32S2
            detectedChipFamily = ImprovTypes::ChipFamily::CF_ESP32_S2;
        #elif CONFIG_IDF_TARGET_ESP32S3
            detectedChipFamily = ImprovTypes::ChipFamily::CF_ESP32_S3;
        #else
            //Serial.println("Unsupported ESP32 variant detected.");
            return;  // Exit setup if the chip is unsupported
        #endif
    #elif defined(ESP8266)
        detectedChipFamily = ImprovTypes::ChipFamily::CF_ESP8266;
    #else
        //Serial.println("Unsupported platform detected.");
        return;  // Exit setup if the platform is unsupported
    #endif
    improvSerial.setDeviceInfo(detectedChipFamily, "ShowboxExpansionModule", SXM_VERSION, "ShowboxExpansionModule Server", "http://{LOCAL_IPV4}?name=Guest");
    improvSerial.onImprovError(onImprovWiFiErrorCb);
    improvSerial.onImprovConnected(onImprovWiFiConnectedCb);
    improvSerial.setCustomConnectWiFi(connectWifi); // Register static callback function
    blinkLed(100, 5);
}

void CustomImprovWifi::onImprovWiFiErrorCb(ImprovTypes::Error err) {
    server.stop();
    if (instance) {
        instance->blinkLed(2000, 3);
    }
}

void CustomImprovWifi::onImprovWiFiConnectedCb(const char* ssid, const char* password) {
    server.begin();
    if (instance) {
        instance->blinkLed(100, 3);
    }
}

bool CustomImprovWifi::connectWifi(const char* ssid, const char* password) {
    WiFi.begin(ssid, password);
    while (!improvSerial.isConnected()) {
        if (instance) {
            instance->blinkLed(500, 1);
        }
    }
    return true;
}

void CustomImprovWifi::handleLoop() {
    improvSerial.handleSerial();
    if (improvSerial.isConnected()) {
        // Handle HTTP requests (can be moved to another function if needed)
        WiFiClient client = server.available();
        if (client) {
            blinkLed(100, 1);
            char linebuf[80];
            int charcount = 0;
            memset(linebuf, 0, sizeof(linebuf));
            boolean currentLineIsBlank = true;

            while (client.connected()) {
                if (client.available()) {
                    char c = client.read();
                    linebuf[charcount] = c;
                    if (charcount < sizeof(linebuf) - 1) charcount++;

                    if (c == '\n' && currentLineIsBlank) {
                        client.println("HTTP/1.1 200 OK");
                        client.println("Content-Type: text/html");
                        client.println("Connection: close");
                        client.println();
                        client.println("<!DOCTYPE HTML><html><body>");
                        client.println("<h1>Welcome!</h1>");
                        client.println("<p>This is a simple webpage served by your ESP32.</p>");
                        client.println("</body></html>");
                        break;
                    }
                    if (c == '\n') {
                        currentLineIsBlank = true;
                    } else if (c != '\r') {
                        currentLineIsBlank = false;
                    }
                }
            }
            delay(1);
            client.stop();
        }
    }
}

void CustomImprovWifi::blinkLed(int d, int times) {
    for (int j = 0; j < times; j++) {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(d);
        digitalWrite(LED_BUILTIN, LOW);
        delay(d);
    }
}
