Import("env")
from os.path import join
import os

def after_build(source, target, env):
    env_name = env.get("PIOENV")
    firmware_path = join(env.subst("$BUILD_DIR"), "firmware.bin")
    bootloader_path = join(env.subst("$BUILD_DIR"), "bootloader.bin")
    partitions_path = join(env.subst("$BUILD_DIR"), "partitions.bin")
    merged_firmware_path = join(env.subst("$BUILD_DIR"), f"sbxm-firmware-{env_name}.bin")

    if env_name is None:
        print("Error: PIO Environment not set. Exiting build process.")
        sys.exit(1)  # Exit with code 1 to indicate an error

    print(f"Checking for files in: {join(env.subst('$BUILD_DIR'))}")
    print(f"Firmware Path: {firmware_path}")
    print(f"Bootloader Path: {bootloader_path}")
    print(f"Partitions Path: {partitions_path}")

    missing_files = []
    for path in [firmware_path, bootloader_path, partitions_path]:
        if not os.path.exists(path):
            missing_files.append(path)

    if not missing_files:
        # Remove check for boot_app0.bin
        env.Execute(f"esptool.py --chip esp32 merge_bin -o {merged_firmware_path} "
                    f"--flash_mode dio --flash_freq 40m --flash_size 4MB "
                    f"0x1000 {bootloader_path} 0x8000 {partitions_path} "
                    f"0x10000 {firmware_path}")
    else:
        print(f"One or more required files for environment {env_name} are missing: {missing_files}")

env.AddPostAction("buildprog", after_build)
