"""Host regression tests and a complete ARM firmware build; no device flashing.
Run with Python; override --arm-gcc/--host-gcc for another installation.
"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "Maintenance_Manager"
OUT = ROOT / "tmp" / "maintenance-build"
DEFAULT_ARM = r"C:\Renesas\RA\e2studio_v2022-04_fsp_v3.8.0\toolchains\gcc_arm\gcc-arm-none-eabi-10.3-2021.10\bin\arm-none-eabi-gcc.exe"


def run(args):
    subprocess.run([str(a) for a in args], check=True, cwd=ROOT)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--arm-gcc", default=DEFAULT_ARM)
    parser.add_argument("--host-gcc", default="gcc")
    parser.add_argument("--host-only", action="store_true")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    core = [PROJECT / "src" / f for f in
            ["Maintenance.c", "maintenance_storage.c", "maintenance_protocol.c"]]
    for display in [False, True]:
        binary = OUT / ("test-display.exe" if display else "test-core.exe")
        defines = (["-DMAINTENANCE_SCREEN_ID=1", "-DMAINTENANCE_REMAIN_CONTROL_ID=11",
                    "-DMAINTENANCE_PERIOD_CONTROL_ID=12", "-DMAINTENANCE_STATUS_CONTROL_ID=13"]
                   if display else [])
        run([args.host_gcc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2",
             "-I", PROJECT / "src", *defines, *core, ROOT / "tests/test_maintenance.c", "-o", binary])
        run([binary])
    if args.host_only:
        return
    includes = ["src", "ra_gen", "ra_cfg/fsp_cfg", "ra_cfg/fsp_cfg/bsp", "ra/fsp/inc",
                "ra/fsp/inc/api", "ra/fsp/inc/instances", "ra/arm/CMSIS_5/CMSIS/Core/Include"]
    flags = ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
             "-std=c99", "-D_RENESAS_RA_", "-D_RA_CORE=CM4", "-Wall", "-Wextra", "-Og", "-g3",
             "-ffunction-sections", "-fdata-sections", "-fstack-usage"]
    for include in includes:
        flags += ["-I", PROJECT / include]
    objects = []
    for folder in ["src", "ra_gen", "ra"]:
        for source in sorted((PROJECT / folder).rglob("*.c")):
            obj = OUT / ("_".join(source.relative_to(PROJECT).parts) + ".o")
            strict = ["-Werror"] if folder == "src" else []
            run([args.arm_gcc, *flags, *strict, "-c", source, "-o", obj])
            objects.append(obj)
    elf = OUT / "Maintenance_Manager.elf"
    run([args.arm_gcc, "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
         "--specs=rdimon.specs", "--specs=nano.specs", "-L", PROJECT / "Debug",
         "-T", PROJECT / "script/fsp.ld", "-Wl,--gc-sections", "-Wl,-Map=" + str(OUT / "firmware.map"),
         *objects, "-o", elf])
    run([str(Path(args.arm_gcc).with_name("arm-none-eabi-size.exe")), elf])
    run([str(Path(args.arm_gcc).with_name("arm-none-eabi-objcopy.exe")), "-O", "ihex",
         elf, OUT / "Maintenance_Manager.hex"])
    print("ARM firmware:", elf)


if __name__ == "__main__":
    main()
