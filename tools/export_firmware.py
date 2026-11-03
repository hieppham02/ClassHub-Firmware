"""Export the successful PlatformIO application build for web OTA uploads."""
from pathlib import Path
import shutil

Import("env")


def export_firmware(source, target, env):
    build_file = Path(env.subst("$BUILD_DIR/${PROGNAME}.bin"))
    release_file = Path(env.subst("$PROJECT_DIR")) / "releases" / "firmware.bin"
    release_file.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build_file, release_file)
    print("OTA firmware saved: {}".format(release_file))


env.AddPostAction("buildprog", export_firmware)
env.AlwaysBuild("buildprog")
