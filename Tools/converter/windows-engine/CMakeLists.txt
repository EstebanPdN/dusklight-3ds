cmake_minimum_required(VERSION 3.24)
project(ForgeMK64Windows LANGUAGES C CXX)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
add_subdirectory("${MK64_EXTRACTOR}/native" engine)

# The traversal keys YAML files using generic_string(). External references
# must use the same separator or a Windows build emits duplicate resources.
set(generated "${CMAKE_CURRENT_BINARY_DIR}/engine/Companion.cpp")
file(READ "${generated}" companion)
set(key "(this->gSourceDirectory / externalFile.as<std::string>())")
string(REPLACE "${key}.string()" "${key}.generic_string()" companion "${companion}")
file(WRITE "${generated}" "${companion}")

# Torch's StringHelper uses std::strtoull without including its declaration.
get_target_property(torch_dir torch SOURCE_DIR)
set_property(SOURCE "${torch_dir}/src/utils/StringHelper.cpp" TARGET_DIRECTORY torch
             APPEND PROPERTY COMPILE_OPTIONS "-include" "cstdlib")
