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

# Опции компиляции
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Путь к SOEM библиотеке (относительный путь от текущей директории)
set(SOEM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SOEM-2.0.0")

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
link_directories(${SOEM_DIR}/lib)

# Добавляем путь к библиотекам Npcap (Windows)
if(WIN32 AND DEFINED NPCAP_SDK_DIR)
    link_directories(${NPCAP_SDK_DIR}/Lib/x64)
endif()

# Основной исполняемый файл
add_executable(dummy-ecat-cli ecat_cli.c)

# Добавляем include directories для target
if(WIN32)
    target_include_directories(dummy-ecat-cli PRIVATE
        ${SOEM_DIR}/include
        ${SOEM_DIR}/osal
        ${SOEM_DIR}/osal/win32
        ${SOEM_DIR}/oshw/win32
    )

    # Добавляем Npcap SDK если найден
    if(DEFINED NPCAP_SDK_DIR)
        target_include_directories(dummy-ecat-cli PRIVATE
            ${NPCAP_SDK_DIR}/Include
        )
    endif()
else()
    target_include_directories(dummy-ecat-cli PRIVATE
        ${SOEM_DIR}/include
        ${SOEM_DIR}/osal
        ${SOEM_DIR}/osal/linux
        ${SOEM_DIR}/oshw/linux
    )
endif()

# Линковка с SOEM и системными библиотеками
if(WIN32)
    target_link_libraries(dummy-ecat-cli soem ws2_32 winmm wpcap packet)
else()
    target_link_libraries(dummy-ecat-cli soem pthread rt)
endif()

# Диагностическая утилита для сетевых адаптеров
add_executable(list-adapters list_adapters.c)

# Добавляем include directories для diagnostic target
if(WIN32)
    target_include_directories(list-adapters PRIVATE
        ${SOEM_DIR}/include
        ${SOEM_DIR}/osal
        ${SOEM_DIR}/osal/win32
        ${SOEM_DIR}/oshw/win32
    )

    # Добавляем Npcap SDK если найден
    if(DEFINED NPCAP_SDK_DIR)
        target_include_directories(list-adapters PRIVATE
            ${NPCAP_SDK_DIR}/Include
        )
    endif()
else()
    target_include_directories(list-adapters PRIVATE
        ${SOEM_DIR}/include
        ${SOEM_DIR}/osal
        ${SOEM_DIR}/osal/linux
        ${SOEM_DIR}/oshw/linux
    )
endif()

# Линковка diagnostic утилиты
if(WIN32)
    target_link_libraries(list-adapters soem ws2_32 winmm wpcap packet iphlpapi)
else()
    target_link_libraries(list-adapters soem pthread rt)
endif()

# Установка
install(TARGETS dummy-ecat-cli list-adapters DESTINATION bin)
