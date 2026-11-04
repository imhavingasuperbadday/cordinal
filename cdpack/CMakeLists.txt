cmake_minimum_required(VERSION 3.20)
project(cdpack LANGUAGES NONE)
enable_testing()
find_program(CDP_POWERSHELL NAMES pwsh powershell REQUIRED)
find_program(CDP_PYTHON NAMES py REQUIRED)
add_test(NAME cdpack-build COMMAND "${CDP_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/build.ps1" -Out "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(cdpack-build PROPERTIES FIXTURES_SETUP cdpack-tools TIMEOUT 120)
add_test(NAME cdpack-format-loader COMMAND "${CDP_PYTHON}" -3.14
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify.py" --tools "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(cdpack-format-loader PROPERTIES FIXTURES_REQUIRED cdpack-tools TIMEOUT 120)
