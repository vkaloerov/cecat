cmake_minimum_required(VERSION 3.10)
project(ecat-cli C)

set(CMAKE_C_STANDARD 99)

# Режим отладки по умолчанию
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Флаги отладки для GCC/Clang
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(NOT MSVC)
    add_compile_options(-g -O0)
    add_link_options(-g)
  endif()
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Создаём симлинк compile_commands.json в корне проекта для clangd (только Linux/macOS)
if(NOT WIN32)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E create_symlink
      "${CMAKE_BINARY_DIR}/compile_commands.json"
      "${CMAKE_SOURCE_DIR}/compile_commands.json"
  )
endif()

# Опции компиляции
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Путь к SOEM библиотеке — можно переопределить через -DSOEM_DIR=...
if(NOT DEFINED SOEM_DIR)
  set(SOEM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SOEM/build")
endif()
message(STATUS "Using SOEM_DIR: ${SOEM_DIR}")

# Авто-определение директории с soem.lib: либо SOEM_DIR/lib, либо корень SOEM_DIR
if(NOT DEFINED SOEM_LIB_DIR)
  if(EXISTS "${SOEM_DIR}/lib")
    set(SOEM_LIB_DIR "${SOEM_DIR}/lib")
  else()
    set(SOEM_LIB_DIR "${SOEM_DIR}")
  endif()
endif()
message(STATUS "Using SOEM_LIB_DIR: ${SOEM_LIB_DIR}")

# Поиск Npcap SDK (для WinPcap headers) только для Windows
if(WIN32)
  if(NOT DEFINED NPCAP_SDK_DIR)
    # Попробуем стандартные пути установки
    set(NPCAP_SEARCH_PATHS
            "C:/npcap-sdk"
            "C:/Npcap-sdk"
            "C:/Program Files/Npcap SDK"
            "C:/Program Files (x86)/Npcap SDK"
            "$ENV{PROGRAMFILES}/Npcap SDK"
            "$ENV{PROGRAMFILES\(X86\)}/Npcap SDK"
        )

    foreach(SEARCH_PATH ${NPCAP_SEARCH_PATHS})
      if(EXISTS "${SEARCH_PATH}/Include")
        set(NPCAP_SDK_DIR "${SEARCH_PATH}")
        message(STATUS "Found Npcap SDK at: ${NPCAP_SDK_DIR}")
        break()
      endif()
    endforeach()
  endif()

  if(NOT DEFINED NPCAP_SDK_DIR)
    message(WARNING "Npcap SDK not found. Please install from https://npcap.com/#download")
    message(WARNING "Or specify path with -DNPCAP_SDK_DIR=<path>")
  endif()
endif()

# Добавляем путь к библиотекам SOEM
link_directories(${SOEM_LIB_DIR} ${TERMBOX2_DIR})

# Добавляем путь к библиотекам Npcap (Windows)
if(WIN32 AND DEFINED NPCAP_SDK_DIR)
  link_directories(${NPCAP_SDK_DIR}/Lib/x64)
endif()

# ============================================================================
# using directly linenoise,due to BSD license
# ============================================================================

set(LINENOISE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../linenoise")
message(STATUS "Using linenoise from: ${LINENOISE_DIR}")

if(NOT EXISTS "${LINENOISE_DIR}/linenoise.h")
  message(FATAL_ERROR "linenoise.h not found at ${LINENOISE_DIR}.")
endif()

if(NOT EXISTS "${LINENOISE_DIR}/linenoise.c")
  message(FATAL_ERROR "linenoise.c not found at ${LINENOISE_DIR}.")
endif()


# ============================================================================
# Main Executable - dummy-ecat-cli
# ============================================================================

add_executable(dummy-ecat-cli
    ecat_cli.c
    my_hex_dump.c
    helper_cmd_c.c
    ${LINENOISE_DIR}/linenoise.c
)

# Compile definitions
# TUI_BUILD guards the standalone main() in ecat_cli.c from conflicting with tui_main.c
# target_compile_definitions(dummy-ecat-cli PRIVATE TUI_BUILD)

# Include directories
target_include_directories(dummy-ecat-cli PRIVATE)

if(WIN32)
  target_include_directories(dummy-ecat-cli PRIVATE
        ${SOEM_DIR}/include
        ${SOEM_DIR}/include/soem
        ${SOEM_DIR}/osal
        ${SOEM_DIR}/osal/win32
        ${SOEM_DIR}/oshw/win32
        ${LINENOISE_DIR}
    )

  if(DEFINED NPCAP_SDK_DIR)
    target_include_directories(dummy-ecat-cli PRIVATE
            ${NPCAP_SDK_DIR}/Include
        )
  endif()
else()
  target_include_directories(dummy-ecat-cli PRIVATE
        ${SOEM_DIR}/include
        ${SOEM_DIR}/../include
        ${SOEM_DIR}/../osal
        ${SOEM_DIR}/../osal/linux
        ${SOEM_DIR}/../oshw/linux
        ${LINENOISE_DIR}
    )
endif()

# Link libraries
if(WIN32)
  target_link_libraries(dummy-ecat-cli PRIVATE soem ws2_32 winmm wpcap packet)
else()
  target_link_libraries(dummy-ecat-cli PRIVATE soem pthread rt)
endif()

# ============================================================================
# Diagnostic Utility - list-adapters
# ============================================================================

add_executable(list-adapters list_adapters.c)

if(WIN32)
  target_include_directories(list-adapters PRIVATE
        ${SOEM_DIR}/include
        ${SOEM_DIR}/osal
        ${SOEM_DIR}/osal/win32
        ${SOEM_DIR}/oshw/win32
    )

  if(DEFINED NPCAP_SDK_DIR)
    target_include_directories(list-adapters PRIVATE
            ${NPCAP_SDK_DIR}/Include
        )
  endif()

  target_link_libraries(list-adapters PRIVATE soem ws2_32 winmm wpcap packet iphlpapi)
else()
  target_include_directories(list-adapters PRIVATE
        ${SOEM_DIR}/include
        ${SOEM_DIR}/../include
        ${SOEM_DIR}/../osal
        ${SOEM_DIR}/../osal/linux
        ${SOEM_DIR}/../oshw/linux
    )

  target_link_libraries(list-adapters PRIVATE soem pthread rt pcap)
endif()

# ============================================================================
# Installation
# ============================================================================

install(TARGETS dummy-ecat-cli list-adapters DESTINATION bin)
