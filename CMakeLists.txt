cmake_minimum_required(VERSION 3.16)
project(fbcp_dma LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build unit tests" OFF)
option(FBCP_DMA_USE_GPU_MAILBOX "Enable GPU mailbox buffer allocation" ON)

add_library(fbcp_dma
    src/fbcp_dma.cpp
    src/spi_dma_linux.cpp
    src/gpu_mailbox.cpp
    src/pixel_utils.cpp
    src/gpu_rotate.cpp
)

target_include_directories(fbcp_dma PUBLIC include)

target_compile_definitions(fbcp_dma PRIVATE
    $<$<BOOL:${FBCP_DMA_USE_GPU_MAILBOX}>:FBCP_DMA_USE_GPU_MAILBOX=1>
)

add_executable(fbcp_daemon src/fbcp_daemon.cpp)
target_link_libraries(fbcp_daemon PRIVATE fbcp_dma)

include(GNUInstallDirs)

install(TARGETS fbcp_daemon
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES systemd/fbcp-daemon.service
    DESTINATION lib/systemd/system
)
install(FILES systemd/fbcp-daemon.conf
    DESTINATION /etc/default
    RENAME fbcp-daemon
)

set(CPACK_PACKAGE_NAME "fbcp-daemon")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "fbcp_dma")
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
  add_executable(fbcp_tests tests/test_pixel_accuracy.cpp)
  target_link_libraries(fbcp_tests PRIVATE fbcp_dma)
  add_test(NAME fbcp_tests COMMAND fbcp_tests)

  add_executable(fbcp_perf_tests tests/test_refresh_perf.cpp)
  target_link_libraries(fbcp_perf_tests PRIVATE fbcp_dma)
  add_test(NAME fbcp_perf_tests COMMAND fbcp_perf_tests)
endif()
