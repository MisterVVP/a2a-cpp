#!/usr/bin/env python3

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


class RunExamplesScriptTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("bash") is None:
            raise unittest.SkipTest("bash is required")
        cls.source_script = Path(sys.argv[1]).resolve()
        cls.install_script = Path(sys.argv[2]).resolve()

    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        script_dir = self.root / "scripts"
        app_dir = self.root / "examples" / "apps" / "streaming_client"
        toolchain_dir = self.root / "vcpkg" / "scripts" / "buildsystems"
        fake_bin = self.root / "fake-bin"
        script_dir.mkdir(parents=True)
        app_dir.mkdir(parents=True)
        toolchain_dir.mkdir(parents=True)
        fake_bin.mkdir(parents=True)
        shutil.copy2(self.source_script, script_dir / "run_examples.sh")
        shutil.copy2(self.install_script, script_dir / "install_build_deps.sh")
        (app_dir / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        (toolchain_dir / "vcpkg.cmake").write_text("# fake toolchain\n", encoding="utf-8")
        self.cmake_log = self.root / "cmake.log"
        self.example_log = self.root / "example.log"
        fake_cmake = fake_bin / "cmake"
        fake_cmake.write_text(
            textwrap.dedent(
                """\
                #!/usr/bin/env bash
                set -euo pipefail
                printf '%s\\n' "$*" >> "${FAKE_CMAKE_LOG}"
                if [[ "${1:-}" == "--build" ]]; then
                  build_dir="$2"
                  config=""
                  shift 2
                  while [[ "$#" -gt 0 ]]; do
                    if [[ "$1" == "--config" ]]; then
                      config="$2"
                      shift 2
                    else
                      shift
                    fi
                  done
                  mkdir -p "${build_dir}/${config}"
                  cat > "${build_dir}/${config}/a2a_example.exe" <<'EOF'
                #!/usr/bin/env bash
                printf '%s\\n' "$*" >> "${FAKE_EXAMPLE_LOG}"
                EOF
                  chmod +x "${build_dir}/${config}/a2a_example.exe"
                fi
                """
            ),
            encoding="utf-8",
        )
        fake_cmake.chmod(0o755)
        self.environment = os.environ.copy()
        self.environment.update(
            {
                "PATH": f"{fake_bin}{os.pathsep}{self.environment.get('PATH', '')}",
                "HOME": str(self.root / "home"),
                "A2A_EXAMPLE_PLATFORM": "MINGW64_NT",
                "VCPKG_ROOT": str(self.root / "vcpkg"),
                "VCPKG_TARGET_TRIPLET": "x64-windows",
                "VCPKG_HOST_TRIPLET": "x64-windows",
                "A2A_EXAMPLE_BUILD_CONFIG": "RelWithDebInfo",
                "FAKE_CMAKE_LOG": str(self.cmake_log),
                "FAKE_EXAMPLE_LOG": str(self.example_log),
            }
        )

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def run_script(self, environment: dict[str, str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", "scripts/run_examples.sh", "build-example", "streaming_client"],
            cwd=self.root,
            env=environment,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_forwards_vcpkg_and_runs_visual_studio_binary(self) -> None:
        result = self.run_script(self.environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        cmake_calls = self.cmake_log.read_text(encoding="utf-8")
        toolchain = self.root / "vcpkg" / "scripts" / "buildsystems" / "vcpkg.cmake"
        self.assertIn(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", cmake_calls)
        self.assertIn("-DVCPKG_TARGET_TRIPLET=x64-windows", cmake_calls)
        self.assertIn("-DVCPKG_HOST_TRIPLET=x64-windows", cmake_calls)
        self.assertIn("-G Visual Studio 17 2022 -A x64", cmake_calls)
        self.assertIn("--config RelWithDebInfo", cmake_calls)
        self.assertEqual(self.example_log.read_text(encoding="utf-8").strip(), "--help")

    def test_windows_without_toolchain_reports_git_bash_setup_command(self) -> None:
        environment = self.environment.copy()
        environment.pop("VCPKG_ROOT", None)
        environment.pop("CMAKE_TOOLCHAIN_FILE", None)
        result = self.run_script(environment)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("./scripts/install_build_deps.sh", result.stderr)
        self.assertIn("Git Bash", result.stderr)

    def test_windows_dependency_installer_has_dry_run(self) -> None:
        environment = self.environment.copy()
        environment.update(
            {
                "A2A_DEPS_PLATFORM": "MINGW64_NT",
                "VCPKG_ROOT": str(self.root / "fresh-vcpkg"),
            }
        )
        result = subprocess.run(
            ["bash", "scripts/install_build_deps.sh", "--dry-run"],
            cwd=self.root,
            env=environment,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("git clone", result.stdout)
        self.assertIn("bootstrap-vcpkg.bat", result.stdout)
        self.assertIn("vcpkg.exe", result.stdout)
        self.assertIn("--triplet x64-windows", result.stdout)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
