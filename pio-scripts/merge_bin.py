Import("env")
from os.path import join

def after_build(source, target, env):
    firmware_path = join(env.subst("$BUILD_DIR"), "firmware.bin")
    merged_firmware_path = join(env.subst("$BUILD_DIR"), "merged-firmware.bin")
    # Command to merge binaries (modify paths and arguments as needed)
    env.Execute("esptool.py --chip esp32 merge_bin -o {} --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 app.bin".format(merged_firmware_path))

env.AddPostAction("buildprog", after_build)
