from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class A2ACppConan(ConanFile):
    name = "a2a-cpp"
    version = "0.1.0"
    license = "Apache-2.0"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "CMakeLists.txt", "cmake/*", "include/*", "src/*", "proto/*"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("protobuf/3.21.12")
        self.requires("grpc/1.54.3")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
