# RemoteDebug rejects a second telnet client when the first PCB is still
# "connected" (RST lost next to BLE). The live suite then gets TCP accept
# with no banner. Always replace the old client.
Import("env")
import glob
import os

proj = env["PROJECT_DIR"]
candidates = glob.glob(
    os.path.join(proj, ".pio", "libdeps", "*", "RemoteDebug", "src", "RemoteDebug.cpp")
)

OLD = """            if (ip == TelnetClient.remoteIP().toString()) {
                // Reconnect

                TelnetClient.stop();
                TelnetClient = newClient;

            } else {
                // Disconnect (not allow more than one connection)
                newClient.stop();

                return;
            }"""

NEW = """            TelnetClient.stop();
            TelnetClient = newClient;"""


def main():
    for cand in candidates:
        if not os.path.isfile(cand):
            continue
        src = open(cand, encoding="utf-8").read()
        if "TelnetClient.stop();\n            TelnetClient = newClient;" in src and "not allow more than one connection" not in src:
            print("remote_debug_replace_client.py: %s already patched" % cand)
            continue
        if OLD not in src:
            print("remote_debug_replace_client.py: %s has no match — skipping" % cand)
            continue
        open(cand, "w", encoding="utf-8").write(src.replace(OLD, NEW, 1))
        print("remote_debug_replace_client.py: patched %s" % cand)


main()
