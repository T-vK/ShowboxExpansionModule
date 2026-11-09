Import("env")
from os.path import join, exists
from shutil import copyfile
import sys

def after_build(source, target, env):
    """Publish sbxm-firmware-<env>.bin for GitHub release / gh-pages.

    pioarduino already merges bootloader+partitions+boot_app0+firmware into
    firmware.factory.bin at the board's real app offset. Calling esptool.py
    again broke CI on tool-esptoolpy 5.4 (ModuleNotFoundError: rich_click)
    and used a stale 0x10000 app offset (this board's app0 is 0x20000).
    """
    env_name = env.get("PIOENV")
    if not env_name:
        print("Error: PIOENV is not set.")
        sys.exit(1)

    build_dir = env.subst("$BUILD_DIR")
    factory_path = join(build_dir, "firmware.factory.bin")
    merged_firmware_path = join(build_dir, f"sbxm-firmware-{env_name}.bin")

    if not exists(factory_path):
        print(f"Error: missing factory image {factory_path}")
        sys.exit(1)

    copyfile(factory_path, merged_firmware_path)
    if not exists(merged_firmware_path):
        print(f"Error: failed to write {merged_firmware_path}")
        sys.exit(1)
    print(f"Release image: {merged_firmware_path}")

env.AddPostAction("buildprog", after_build)
