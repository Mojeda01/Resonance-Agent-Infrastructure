cmake_minimum_required(VERSION 3.16)
project(agentinfra LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # POSIX/GNU features (sockets, strdup, ...)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- llama.cpp -------------------------------------------------------------
# Use an installed llama.cpp (e.g. `brew install llama.cpp`), or point
# -DLLAMA_CPP_DIR=/path/to/llama.cpp at a source checkout to build it in-tree.
set(LLAMA_CPP_DIR "" CACHE PATH "Path to a llama.cpp source checkout (optional)")

if(LLAMA_CPP_DIR)
  add_subdirectory(${LLAMA_CPP_DIR} llama.cpp EXCLUDE_FROM_ALL)
  set(LLAMA_TARGET llama)
else()
  find_package(llama CONFIG QUIET)
  if(TARGET llama)
    set(LLAMA_TARGET llama)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LLAMA REQUIRED IMPORTED_TARGET llama)
    set(LLAMA_TARGET PkgConfig::LLAMA)
  endif()
endif()

find_package(Threads REQUIRED)

# --- targets ---------------------------------------------------------------
add_library(agentinfra_common STATIC
  src/tools.c
  src/session.c
  src/models.c
  src/sock.c
  src/server.c
  src/generate.c
  src/chat.c
  src/skills.c
  src/skills/ls/ls.c
)
target_include_directories(agentinfra_common PUBLIC src)
target_link_libraries(agentinfra_common PUBLIC ${LLAMA_TARGET} Threads::Threads)
target_compile_options(agentinfra_common PUBLIC -Wall -Wextra)

# Interactive agent / one-shot CLI
add_executable(agent src/main.c)
target_link_libraries(agent PRIVATE agentinfra_common)

# Long-lived daemon (Unix socket server)
add_executable(agentd src/serve_main.c)
target_link_libraries(agentd PRIVATE agentinfra_common)

# --- install ---------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS agent agentd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY src/skills/ DESTINATION ${CMAKE_INSTALL_DATADIR}/agentinfra/skills
        FILES_MATCHING PATTERN "*.md")
