## Mackie Showbox Interceptor Library

Gain more control over your Showbox. This project allows for controlling your Showbox and breakaway Mixer using an ESP32-S3, ESP32, ESP8266 or Arduino. (ESP32-S3 will probably have the least bugs because I will primarily test against that. Also it supports most features.)

### Hardware
I will try to make make this project as accessible as possible. Soldering skills will not be necessary, but they may allow for a cleaner and more stable build.

Here is an example on how you could wire everything if you wanted to add two foot switches and MIDI (which is not implemented and has not been tested at all):

![Interceptor Schematics](https://www.musiker-board.de/attachments/interceptorschematics-png.947497/)

In this example I used:
- ESP32 D1 Mini - e.g. https://www.amazon.de/dp/B0CM9BR387
- 2x RJ45 Breakout Board - e.g. https://www.ebay.de/itm/166667936034
- Wemos D1 Mini DC Shield - e.g. https://www.ebay.de/itm/166449488149
- 2x TRS sockets  - e.g. https://www.aliexpress.com/item/32745341055.html
- 2x MIDI sockets - e.g. https://www.ebay.de/itm/126549251439
- Opto Isolator 4N35
- Resistors: 33OR, 10R, 220R, 420R
- 1N4149 Diode

If you don't want to solder, look for breakout boards/sockets with screw terminals or pre-soldered pin headers (in the latter case you might want to buy a bunch of so called dupont wires as well). I highly discourage anyone from using dupont wires + pin headers though because of how unreliable they are. I only recommend it for prototyping.

### TODO
- [x] Class to programmatically do anything the Breakaway Mixer can do.
- [x] Example on how to add footswitches to control anything you want
- [ ] Add support for MIDI
- [ ] Add support for MIDI over Bluetooth
- [ ] Add support for MIDI over WiFi
- [ ] Add support for MIDI over USB
- [ ] Add support for MIDI over Ethernet
- [ ] Add support for Mackie Control Protocol (which is technically just specially MIDI commands)
- [ ] Add support for using the breakaway mixer as a DAW controller (Would anyone even want this??)
- [ ] Add support for Bluetooth HID (to allow using it as a Bluetooth page turner as well)
- [ ] Add support for USB HID (to allow using it as a USB page turner as well)
- [ ] Add more examples
- [ ] Split UARTInterceptor and MackieShowbox into two libraries
- [ ] GitHub Actions for PlatformIO publishing
- [ ] GitHub Actions to provide binary builds
- [ ] Add webinterface to allow controlling the Showbox from the browser (over either WiFi or Ethernet)
- [ ] Web flasher to allow flashing the micro controller from the browser
- [ ] Add support for OTA updates

### Reading
You can easily read and log what the two devices are talking about. Here is a short example:

```
MIXER->BASE [Decoded] Type: ENTITY | Entity: LOOPER_LEVEL | { Set LOOPER_LEVEL to 9.615385 }
MIXER->BASE [Decoded] Type: ENTITY | Entity: MAIN_MUTE | { Set MAIN_MUTE to false }
MIXER->BASE [Decoded] Type: ENTITY | Entity: STEREO_INPUT1_EQ_HIGH_GAIN | { Set STEREO_INPUT1_EQ_HIGH_GAIN to -12.857142 }
MIXER->BASE [Decoded] Type: ENTITY | Entity: SELECTED_CHAN | { Set SELECTED_CHAN to 0 }
MIXER->BASE [Decoded] Type: ENTITY | Entity: INPUT1_COMPRESSOR_AMOUNT | { Set INPUT1_COMPRESSOR_AMOUNT to 0.600000 }
MIXER->BASE [Decoded] Type: ENTITY | Entity: INPUT1_GAIN | { Set INPUT1_GAIN to 36.000000 }
MIXER->BASE [Decoded] Type: ENTITY | Entity: INPUT1_EQ_ENABLE | { Set INPUT1_EQ_ENABLE to false }
BASE->MIXER [Decoded] Type: ENTITY | Entity: INPUT1_EQ_ENABLE | { Set INPUT1_EQ_ENABLE to true }
MIXER->BASE [Decoded] Type: ENTITY | Entity: INPUT1_EFFECT_2_AMOUNT | { Set INPUT1_EFFECT_2_AMOUNT to 0.400000 }
MIXER->BASE [Decoded] Type: ENTITY | Entity: INPUT1_EXT_FX_SENDS | { Set INPUT1_EXT_FX_SENDS to 0.066667 }
```

Since the MackieControl Class is intercepting the entire communication, you can always ask it for information it has already intercepted. E.g.:

``` C++
bool mute = showbox.getBoolEntityValue(entity_id::MAIN_MUTE);
if (mute) {
    Serial.println("Showbox is currently muted")
}
```

### Injecting
You can easily inject packets into the communication of the devices which may look like this:

```
BASE->MIXER Acknowoleged LOOPER_BUTTON command
BASE->MIXER [Decoded] Type: ENTITY | Entity: LOOPER_STATE | { Set LOOPER_STATE to RECORD_INITIAL_LOOP }
BASE->MIXER Acknowoleged LOOPER_BUTTON command
BASE->MIXER Acknowoleged LOOPER_BUTTON command
BASE->MIXER [Decoded] Type: ENTITY | Entity: LOOPER_STATE | { Set LOOPER_STATE to PLAY }
BASE->MIXER Acknowoleged LOOPER_BUTTON command
BASE->MIXER [Decoded] Type: ENTITY | Entity: LOOPER_STATE | { Set LOOPER_STATE to DELETE }
```

In order to inject a packet, all you have to do is call a method on the MackieShowbox Class.

For example to mute the Mackie Showbox, you would simply do:

``` C++
showbox.setEntityValue(entity_id::MAIN_MUTE, true);
```

### Manipulating
It is technically possible to manipulate packets on the fly. This functionality is just not exposed to the outside of the class for now because I haven't found a good use-case for it yet.

### Dropping
It is technically possible to drop packets entirely, preventing them from arriving at their destination. This functionality is just not exposed to the outside of the class for now because I haven't found a good use-case for it yet.
