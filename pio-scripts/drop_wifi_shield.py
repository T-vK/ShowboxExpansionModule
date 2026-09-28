Import("env")
import shutil
from os.path import join

# Improv declares depends=WiFi. PlatformIO then installs the Arduino WiFi
# shield library, which hides the ESP32 WiFi stack. Remove that copy before
# the library finder runs so the framework WiFi is the one that compiles.
shield = join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "WiFi")
props = join(shield, "library.properties")
try:
    text = open(props, encoding="utf-8").read()
except OSError:
    text = ""
if "Arduino WiFi shield" in text:
    shutil.rmtree(shield)

# An earlier build turned the MIDI library's BLE client off. The types the
# firmware uses live behind that flag, so put it back.
midi_config = join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "audio-tools midi", "src", "ConfigMidi.h")
try:
    midi = open(midi_config, encoding="utf-8").read()
except OSError:
    midi = ""
updated = midi.replace("#  define MIDI_BLE_ACTIVE false", "#  define MIDI_BLE_ACTIVE true", 1)
if updated != midi:
    open(midi_config, "w", encoding="utf-8").write(updated)
