# Project build configuration file. CMake reads this file and generates build files.
# This file is part of the Adaptive FPGA Market Engine project.


cmake_minimum_required(VERSION 3.22)    # Requires at least CMake version 3.22.

project(adaptive_fpga_market_engine VERSION 0.1.0 LANGUAGES CXX)    # Defines project name, version and language.

# Build options
option(MARKET_ENGINE_ENABLE_WERROR "Treat project warnings as errors" ON)   # Warnings are treated as errors by default.
option(MARKET_ENGINE_ENABLE_SANITIZERS "Enable address and undefined-behaviour sanitizers" OFF)   # Enables runtime memory and undefined-behaviour checks when requested.


set(CMAKE_CXX_STANDARD 20)   # Requests the C++20 language standard.
set(CMAKE_CXX_STANDARD_REQUIRED ON)    # Makes C++20 mandatory.
set(CMAKE_CXX_EXTENSIONS OFF)     # Disables compiler-specific language extensions.

include(FetchContent)     # Allows external dependencies to be downloaded during configuration.
include(CTest)      # Loads CMake's testing support.

add_library(market_engine_project_options INTERFACE)     # Reusable container for compiler and linker settings.
target_compile_options(market_engine_project_options INTERFACE        # Adds warning flags depending on the compiler.
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>      # GCC and Clang warning flags.
    $<$<CXX_COMPILER_ID:MSVC>:/W4>      # MSVC warning level 4.
)

# If configured, treat compiler warnings as errors.
if(MARKET_ENGINE_ENABLE_WERROR)
    target_compile_options(market_engine_project_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/WX>
    )
endif()

# Add sanitizer options when enabled with GCC or Clang.
if(MARKET_ENGINE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(market_engine_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(market_engine_project_options INTERFACE -fsanitize=address,undefined)
endif()

# Required and optional dependencies.
# REQUIRED stops configuration if the package is missing; QUIET suppresses messages.
find_package(Boost 1.74 REQUIRED COMPONENTS system)  # Requires Boost.System 1.74 or newer.
find_package(OpenCL QUIET)  # OpenCL is optional.
find_package(Python3 QUIET COMPONENTS Interpreter)  # Python is optional.
find_program(VERILATOR_EXECUTABLE verilator)  # Searches for the Verilator executable.

# Download external dependencies. Exact commits make builds reproducible.
FetchContent_Declare(   # Declares the nlohmann_json dependency.
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03 # nlohmann/json v3.11.3
    GIT_SHALLOW TRUE
)

FetchContent_Declare(   # Declares the Catch2 dependency.
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG fa43b77429ba76c462b1898d6cd2f2d7a9416b14 # Catch2 v3.7.1
    GIT_SHALLOW TRUE
)
# Download and make both dependencies available to the project.
FetchContent_MakeAvailable(nlohmann_json Catch2)

# Main application library.
add_library(market_engine_app    # Separates reusable application code from the executable entry point.
    src/app/config.cpp
    src/app/opencl_devices.cpp
    src/market/event.cpp
    src/market/order_book.cpp
)

# Add the include directory to this target and targets that link to it.
target_include_directories(market_engine_app PUBLIC include)  

# Link shared compiler settings and the JSON library.
target_link_libraries(market_engine_app
    PUBLIC market_engine_project_options nlohmann_json::nlohmann_json
)

# Optional OpenCL support.
# If OpenCL was found, link it to the application and tell the C++ code
# that OpenCL support is available.
if(OpenCL_FOUND)
    target_link_libraries(market_engine_app PUBLIC OpenCL::OpenCL)
    target_compile_definitions(market_engine_app PUBLIC MARKET_ENGINE_HAS_OPENCL=1)
else()
    # OpenCL is optional, so the application can still build without it.
    # C++ code can check this macro with #if MARKET_ENGINE_HAS_OPENCL.
    target_compile_definitions(market_engine_app PUBLIC MARKET_ENGINE_HAS_OPENCL=0)
endif()

# Detect whether the Verilator executable was found.
# Store the result as 1 or 0 so it can be passed to the C++ preprocessor.
if(VERILATOR_EXECUTABLE)
    set(MARKET_ENGINE_VERILATOR_AVAILABLE 1)
else()
    set(MARKET_ENGINE_VERILATOR_AVAILABLE 0)
endif()

# Detect whether a Python 3 interpreter was found.
# Again, store the result as a numeric 1 or 0.
if(Python3_Interpreter_FOUND)
    set(MARKET_ENGINE_PYTHON_AVAILABLE 1)
else()
    set(MARKET_ENGINE_PYTHON_AVAILABLE 0)
endif()

# Pass the Verilator and Python availability flags to C++ as preprocessor
# definitions, for example -DMARKET_ENGINE_PYTHON_AVAILABLE=1.
target_compile_definitions(market_engine_app PUBLIC
    MARKET_ENGINE_VERILATOR_AVAILABLE=${MARKET_ENGINE_VERILATOR_AVAILABLE}
    MARKET_ENGINE_PYTHON_AVAILABLE=${MARKET_ENGINE_PYTHON_AVAILABLE}
)

# Build the main demo executable from src/main.cpp.
add_executable(market_engine_demo src/main.cpp)

# Link the demo with the reusable application library.
target_link_libraries(market_engine_demo PRIVATE market_engine_app)

# Build and register the C++ tests when CTest testing is enabled.
if(BUILD_TESTING)
    # Create the test executable from the listed test source files.
    add_executable(market_engine_tests
        tests/cpp/config_tests.cpp
        tests/cpp/protocol_tests.cpp
        tests/cpp/order_book_tests.cpp
    )

    # Link the tests with the application library and Catch2's supplied main().
    target_link_libraries(market_engine_tests PRIVATE market_engine_app Catch2::Catch2WithMain)

    # Load Catch2's CMake helpers and automatically discover the test cases.
    include(Catch)
    catch_discover_tests(market_engine_tests)
endif()

if(VERILATOR_EXECUTABLE)
    add_custom_target(rtl_tests
        COMMAND ${VERILATOR_EXECUTABLE}
            --binary
            --timing
            --assert
            --Wall
            --top-module order_book_tb
            --Mdir ${CMAKE_BINARY_DIR}/verilator_order_book_tb
            ${CMAKE_CURRENT_SOURCE_DIR}/rtl/market_types_pkg.sv
            ${CMAKE_CURRENT_SOURCE_DIR}/rtl/order_book.sv
            ${CMAKE_CURRENT_SOURCE_DIR}/tb/order_book_tb.sv
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Building and running standalone RTL order-book tests"
    )
endif()

# Print a summary of the dependencies and optional capabilities found during
# configuration.
message(STATUS "Adaptive FPGA Market Engine capability report")
message(STATUS "  Boost.System: ${Boost_VERSION}")
message(STATUS "  OpenCL: ${OpenCL_FOUND}")
message(STATUS "  Python interpreter: ${Python3_Interpreter_FOUND}")
message(STATUS "  Verilator: ${VERILATOR_EXECUTABLE}")
message(STATUS "  Sanitizers: ${MARKET_ENGINE_ENABLE_SANITIZERS}")
