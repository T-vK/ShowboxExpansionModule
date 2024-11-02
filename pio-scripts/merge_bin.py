Import("env")
from os.path import join, exists
import sys

def after_build(source, target, env):
    env_name = env.get("PIOENV")
    firmware_path = join(env.subst("$BUILD_DIR"), "firmware.bin")
    bootloader_path = join(env.subst("$BUILD_DIR"), "bootloader.bin")
    partitions_path = join(env.subst("$BUILD_DIR"), "partitions.bin")
    boot_app0_path = join(env.subst("$PROJECT_PACKAGES_DIR"), "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
    merged_firmware_path = join(env.subst("$BUILD_DIR"), f"sbxm-firmware-{env_name}.bin")

    flash_size = env.BoardConfig().get("upload.flash_size", "4MB")
    flash_mode = env["__get_board_flash_mode"](env)
    flash_freq = env["__get_board_f_flash"](env)
    chip_type = env.get("BOARD_MCU")

    # # Get required values
    # flash_mode = env.get("BOARD_FLASH_MODE")
    # flash_freq_hz = env.get("BOARD_F_FLASH")
    # chip_type = env.get("BOARD_MCU")
    # uploaderflags = env.get("UPLOADERFLAGS")

    # # Extract flash_size from UPLOADERFLAGS
    # flash_size = None
    # if uploaderflags:
    #     try:
    #         flash_size_index = uploaderflags.index('--flash_size')
    #         flash_size = uploaderflags[flash_size_index + 1].strip('"')
    #     except (ValueError, IndexError):
    #         print("Error: Unable to extract 'flash_size' from UPLOADERFLAGS.")
    #         sys.exit(1)

    # # Check for required values
    # if not all([flash_mode, flash_freq_hz, chip_type, flash_size]):
    #     print("Error: Missing required properties. Ensure 'flash_mode', 'flash_freq', 'chip_type', and 'flash_size' are defined.")
    #     sys.exit(1)

    # # Convert flash frequency from Hz to the format expected by esptool (e.g., '80m' for 80000000)
    # flash_freq = f"{int(flash_freq_hz.rstrip('L')) // 1000000}m"

    # Check for the existence of necessary files
    if not all(exists(path) for path in [firmware_path, bootloader_path, partitions_path, boot_app0_path]):
        print(f"Error: One or more required files for environment {env_name} are missing.")
        sys.exit(1)

    # Prepare the esptool command
    esptool_command = f"esptool.py --chip {chip_type} merge_bin -o {merged_firmware_path} " \
                      f"--flash_mode {flash_mode} --flash_freq {flash_freq} --flash_size {flash_size} " \
                      f"0x1000 {bootloader_path} 0x8000 {partitions_path} " \
                      f"0xe000 {boot_app0_path} 0x10000 {firmware_path}"
    
    # Print the command before executing
    print("Executing command:", esptool_command)

    # Execute the merge command
    env.Execute(esptool_command)

env.AddPostAction("buildprog", after_build)

# Dump global construction environment (for debug purpose)
#print(env.Dump())

# Dump project construction environment (for debug purpose)
#print(projenv.Dump())
