# Native UWP (Xbox Dev Mode / Windows) build of Pokemon Silver.
#
# Configure with:
#   cmake -S uwp -B uwp/build -G "Visual Studio 17 2022" -A x64 \
#         -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0
#
# Expects:
#   uwp/gambatte/       checkout of libretro/gambatte-libretro (GPLv2 GBC core)
#   uwp/rom/pokesilver.gbc   ROM built from this repo (make pokesilver.gbc)

cmake_minimum_required(VERSION 3.20)
project(PokeSilver C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  message(FATAL_ERROR "Configure with -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GAMBATTE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gambatte" CACHE PATH "gambatte-libretro checkout")
set(ROM_FILE "${CMAKE_CURRENT_SOURCE_DIR}/rom/pokesilver.gbc" CACHE FILEPATH "Silver ROM to embed")

if(NOT EXISTS "${GAMBATTE_DIR}/libgambatte/src/gambatte.cpp")
  message(FATAL_ERROR "gambatte-libretro not found at ${GAMBATTE_DIR}")
endif()
if(NOT EXISTS "${ROM_FILE}")
  message(FATAL_ERROR "ROM not found at ${ROM_FILE}; run `make pokesilver.gbc` and copy it there")
endif()

# ---- Game Boy Color core (libgambatte only, no libretro frontend glue) ----
set(GB "${GAMBATTE_DIR}/libgambatte")
file(GLOB_RECURSE GAMBATTE_SOURCES CONFIGURE_DEPENDS "${GB}/src/*.cpp")
add_library(gambatte STATIC ${GAMBATTE_SOURCES})
target_include_directories(gambatte PUBLIC
  "${GB}/include"
  "${GB}/src"
  "${GB}/libretro"
  "${GB}/libretro-common/include"
  "${GAMBATTE_DIR}/common")
target_compile_definitions(gambatte PUBLIC
  __LIBRETRO__ HAVE_STDINT_H HAVE_INTTYPES_H _CRT_SECURE_NO_WARNINGS _CRT_SECURE_NO_DEPRECATE)
target_compile_options(gambatte PRIVATE /W1 /EHsc /O2 /sdl- /Zc:__cplusplus)

# ---- App ----
set(ASSETS
  Assets/StoreLogo.png
  Assets/Square44x44Logo.png
  Assets/Square150x150Logo.png
  Assets/Wide310x150Logo.png
  Assets/SplashScreen.png)
set_property(SOURCE ${ASSETS} PROPERTY VS_DEPLOYMENT_CONTENT 1)
set_property(SOURCE ${ASSETS} PROPERTY VS_DEPLOYMENT_LOCATION "Assets")
set_property(SOURCE "${ROM_FILE}" PROPERTY VS_DEPLOYMENT_CONTENT 1)
set_property(SOURCE "${ROM_FILE}" PROPERTY VS_DEPLOYMENT_LOCATION "Rom")

set(APP_SOURCES src/App.cpp Package.appxmanifest ${ASSETS} "${ROM_FILE}")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/PokeSilver.pfx")
  list(APPEND APP_SOURCES PokeSilver.pfx)
endif()

add_executable(PokeSilver WIN32 ${APP_SOURCES})
target_link_libraries(PokeSilver PRIVATE gambatte windowsapp d3d11 dxgi xaudio2)
target_compile_definitions(PokeSilver PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(PokeSilver PRIVATE /EHsc /bigobj /permissive- /O2 /sdl- /Zc:__cplusplus)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT PokeSilver)
