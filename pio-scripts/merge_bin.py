Import("env")
from os.path import join, exists
import sys

def after_build(source, target, env):
    env_name = env.get("PIOENV")
    flash_size = env.BoardConfig().get("upload.flash_size", "4MB")
    flash_mode = env["__get_board_flash_mode"](env)
    flash_freq = env["__get_board_f_flash"](env)
    chip_type = env.get("BOARD_MCU")

    # Check if all necessary variables are set
    if not all([env_name, flash_size, flash_mode, flash_freq, chip_type]):
        print("Error: One or more necessary variables are not set.")
        sys.exit(1)

    # Define paths to necessary files
    firmware_path = join(env.subst("$BUILD_DIR"), "firmware.bin")
    bootloader_path = join(env.subst("$BUILD_DIR"), "bootloader.bin")
    partitions_path = join(env.subst("$BUILD_DIR"), "partitions.bin")
    boot_app0_path = join(env.subst("$PROJECT_PACKAGES_DIR"), "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
    merged_firmware_path = join(env.subst("$BUILD_DIR"), f"sbxm-firmware-{env_name}.bin")

    # Check for the existence of necessary files
    if not all(exists(path) for path in [firmware_path, bootloader_path, partitions_path, boot_app0_path]):
        print(f"Error: One or more required files for environment {env_name} are missing.")
        sys.exit(1)

    # Prepare the esptool command
    esptool_command = (
        f"esptool.py --chip {chip_type} "
        f"merge_bin -o {merged_firmware_path} "
        f"--flash_mode {flash_mode} --flash_freq {flash_freq} --flash_size {flash_size} "
        f"0x0000 {bootloader_path} "
        f"0x8000 {partitions_path} "
        f"0xe000 {boot_app0_path} "
        f"0x10000 {firmware_path}"
    )
    
    # Print the command before executing
    print("Executing command:", esptool_command)

    # Execute the merge command
    env.Execute(esptool_command)

env.AddPostAction("buildprog", after_build)

# Dump global construction environment (for debug purpose)
#print(env.Dump())

# Dump project construction environment (for debug purpose)
#print(projenv.Dump())
