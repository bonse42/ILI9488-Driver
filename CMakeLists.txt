cmake_minimum_required(VERSION 3.16)
project(ili9488_dma LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build unit tests" OFF)
option(ILI9488_DMA_USE_GPU_MAILBOX "Enable GPU mailbox buffer allocation" ON)

add_library(ili9488_dma
    src/ili9488_dma.cpp
    src/spi_dma_linux.cpp
    src/ili9488_mailbox.cpp
    src/pixel_utils.cpp
    src/ili9488_rotate.cpp
)

target_include_directories(ili9488_dma PUBLIC include)

target_link_libraries(ili9488_dma PRIVATE pthread)

target_compile_definitions(ili9488_dma PRIVATE
    $<$<BOOL:${ILI9488_DMA_USE_GPU_MAILBOX}>:ILI9488_DMA_USE_GPU_MAILBOX=1>
)

add_executable(ili9488-daemon src/ili9488_daemon.cpp)
target_link_libraries(ili9488-daemon PRIVATE ili9488_dma)

include(GNUInstallDirs)

install(TARGETS ili9488-daemon
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES systemd/ili9488-daemon.service
    DESTINATION lib/systemd/system
)
install(FILES systemd/ili9488-daemon.conf
    DESTINATION /etc/default
    RENAME ili9488-daemon
)

set(CPACK_PACKAGE_NAME "ili9488-daemon")
set(CPACK_PACKAGE_VERSION "1.1.0")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "ili9488_dma")
set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "systemd")

if(NOT DEFINED CPACK_DEBIAN_PACKAGE_ARCHITECTURE)
  set(_deb_arch "${CMAKE_SYSTEM_PROCESSOR}")
  if(_deb_arch STREQUAL "aarch64")
    set(_deb_arch "arm64")
  elseif(_deb_arch STREQUAL "armv7l")
    set(_deb_arch "armhf")
  endif()
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_deb_arch}")
endif()

set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}")

set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postinst")
set(CPACK_GENERATOR "DEB")
include(CPack)

if(BUILD_TESTS)
  enable_testing()
  add_executable(ili9488_tests tests/test_pixel_accuracy.cpp)
  target_link_libraries(ili9488_tests PRIVATE ili9488_dma)
  add_test(NAME ili9488_tests COMMAND ili9488_tests)

  add_executable(ili9488_perf_tests tests/test_refresh_perf.cpp)
  target_link_libraries(ili9488_perf_tests PRIVATE ili9488_dma)
  add_test(NAME ili9488_perf_tests COMMAND ili9488_perf_tests)
endif()
