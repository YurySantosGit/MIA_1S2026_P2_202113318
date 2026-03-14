cmake_minimum_required(VERSION 3.16)

project(ExtreamFS VERSION 1.0 LANGUAGES CXX)

# =========================
# Configuración del compilador
# =========================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =========================
# Archivos fuente
# =========================
set(SOURCES
    src/main.cpp
    src/core/Analyzer.cpp
    src/disk/DiskManagement.cpp
    src/disk/MountManager.cpp
    src/fs/FileSystemManager.cpp
    src/fs/SessionManager.cpp
    src/reports/ReportManager.cpp
)

# =========================
# Crear ejecutable
# =========================
add_executable(extreamfs ${SOURCES})

# =========================
# Incluir headers
# =========================
target_include_directories(extreamfs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# =========================
# Flags de compilación
# =========================
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(extreamfs PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# =========================
# Modo Debug
# =========================
set(CMAKE_BUILD_TYPE Debug)