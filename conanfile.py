import os

from conan import ConanFile
from conan.tools.files import copy
from conan.tools.layout import basic_layout


class SofaBuffersCorelibCppConan(ConanFile):
    name = "sofa-buffers-corelib-cpp"
    version = "0.8.0"
    license = "MIT"
    author = "SofaBuffers"
    url = "https://github.com/sofa-buffers/corelib-cpp"
    homepage = "https://github.com/sofa-buffers/corelib-cpp"
    description = (
        "Streaming, dependency-free, pure-C++20 implementation of the "
        "SofaBuffers (Sofab) serialization format. Header-only."
    )
    topics = ("serialization", "sofabuffers", "header-only", "cpp20", "streaming")

    # Header-only: no settings affect the package contents.
    package_type = "header-library"
    settings = "os", "arch", "compiler", "build_type"
    no_copy_source = True

    exports_sources = "include/*", "LICENSE"

    def layout(self):
        basic_layout(self, src_folder=".")

    def package(self):
        copy(self, "*.hpp",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"))
        copy(self, "LICENSE",
             src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

    def package_id(self):
        self.info.clear()

    def package_info(self):
        # Consumed as sofa-buffers::corelib to match the project's CMake target.
        self.cpp_info.set_property("cmake_file_name", "sofa-buffers-corelib-cpp")
        self.cpp_info.set_property("cmake_target_name", "sofa-buffers::corelib")
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []

    def validate(self):
        from conan.tools.build import check_min_cppstd
        check_min_cppstd(self, 20)
