# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
"""Build and run a native callback service against the installed SDK only."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def cache_values(build):
    values = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.split(":", 1)[0]] = value
    return values


def run_consumer(source, build, config):
    values = cache_values(build)
    with tempfile.TemporaryDirectory(prefix="installed-async-grpc-", dir=build) as scratch:
        root = Path(scratch)
        prefix = root / "sdk"
        consumer = root / "consumer"
        subprocess.run(["cmake", "--install", str(build), "--config", config, "--prefix", str(prefix)], check=True)
        inherited = values.get("CMAKE_PREFIX_PATH", os.environ.get("CMAKE_PREFIX_PATH", ""))
        # CMake cache variables always use semicolons, including on Unix.
        if "CMAKE_PREFIX_PATH" not in values and os.name != "nt":
            inherited = inherited.replace(os.pathsep, ";")
        command = ["cmake", "-S", str(source / "examples" / "installed_package_consumer"), "-B", str(consumer),
                   "-DA2A_EXAMPLE_APP=async_grpc_server", "-DCMAKE_PREFIX_PATH=" + str(prefix) + ";" + inherited,
                   "-DCMAKE_BUILD_TYPE=" + config, "-G", values["CMAKE_GENERATOR"]]
        if values.get("CMAKE_CONFIGURATION_TYPES"):
            # The SDK installation contains only the configuration under test.
            command.append("-DCMAKE_CONFIGURATION_TYPES=" + config)
        forwards = ["CMAKE_CXX_COMPILER", "CMAKE_TOOLCHAIN_FILE", "VCPKG_TARGET_TRIPLET", "VCPKG_HOST_TRIPLET",
                    "VCPKG_INSTALLED_DIR", "VCPKG_OVERLAY_TRIPLETS", "CMAKE_MAKE_PROGRAM", "CMAKE_OSX_SYSROOT",
                    "CMAKE_OSX_ARCHITECTURES", "CMAKE_OSX_DEPLOYMENT_TARGET", "CMAKE_MSVC_RUNTIME_LIBRARY",
                    "CMAKE_CXX_FLAGS", "CMAKE_EXE_LINKER_FLAGS", "CMAKE_SHARED_LINKER_FLAGS",
                    "CMAKE_CXX_FLAGS_DEBUG", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_CXX_FLAGS_RELWITHDEBINFO",
                    "CMAKE_EXE_LINKER_FLAGS_DEBUG", "CMAKE_EXE_LINKER_FLAGS_RELEASE",
                    "CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO"]
        for key in forwards:
            if values.get(key):
                command.append("-D" + key + "=" + values[key])
        for key, flag in [("CMAKE_GENERATOR_PLATFORM", "-A"), ("CMAKE_GENERATOR_TOOLSET", "-T")]:
            if values.get(key):
                command.extend([flag, values[key]])
        if values.get("VCPKG_INSTALLED_DIR"):
            command.append("-DVCPKG_MANIFEST_MODE=OFF")
        subprocess.run(command, check=True)
        subprocess.run(["cmake", "--build", str(consumer), "--config", config, "--parallel", "2"], check=True)
        suffix = ".exe" if os.name == "nt" else ""
        candidates = [consumer / config / ("a2a_example" + suffix), consumer / ("a2a_example" + suffix)]
        binary = next(path for path in candidates if path.is_file())
        completed = subprocess.run([str(binary), "--smoke"], check=False, timeout=30)
        if completed.returncode == 77:
            raise SystemExit(77)
        completed.check_returncode()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--config", default="Debug")
    args = parser.parse_args()
    run_consumer(args.source, args.build, args.config)


if __name__ == "__main__":
    main()
