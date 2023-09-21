cmake_minimum_required(VERSION 3.20)

project(scan-watch
        VERSION 0.1
        DESCRIPTION "Scan monitor and OCR for use with scanners that can upload over SFTP"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

find_package(magic_enum CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(Lua REQUIRED)

find_library(Tesseract "tesseract" REQUIRED)
find_library(OMP "omp" REQUIRED)
find_library(Leptonica "leptonica" REQUIRED)

find_file(CLI11 "CLI/CLI.hpp" REQUIRED)
string(LENGTH "${CLI11}" CLI11Length)
math(EXPR CLI11PrefixLength "${CLI11Length} - 12")
string(SUBSTRING "${CLI11}" 0 ${CLI11PrefixLength} CLI11)

add_definitions(-D_LIBCPP_ENABLE_EXPERIMENTAL)
add_definitions(-DPREFIX="${CMAKE_INSTALL_PREFIX}")

add_executable(scan-watch-sftp-server sftp-upload-server.cc)
target_link_libraries(scan-watch-sftp-server magic_enum::magic_enum)
target_link_libraries(scan-watch-sftp-server c++experimental)

add_executable(scan-watch-manual-submit send-file.cc)
target_include_directories(scan-watch-manual-submit PRIVATE ${CLI11})

add_executable(scan-watch-ocr ocr.cc)
target_link_libraries(scan-watch-ocr sol2::sol2)
target_link_libraries(scan-watch-ocr c++experimental)
target_link_libraries(scan-watch-ocr ${LUA_LIBRARIES})
target_link_libraries(scan-watch-ocr ${Tesseract})
target_link_libraries(scan-watch-ocr ${OMP})
target_link_libraries(scan-watch-ocr ${Leptonica})
target_include_directories(scan-watch-ocr PRIVATE ${LUA_INCLUDE_DIR})
target_include_directories(scan-watch-ocr PRIVATE ${CLI11})

configure_file("scan_watch.in" "scan_watch" @ONLY)

install(TARGETS scan-watch-manual-submit scan-watch-ocr scan-watch-sftp-server)
install(PROGRAMS "${CMAKE_BINARY_DIR}/scan_watch" DESTINATION "etc/rc.d")

set(CPACK_GENERATOR "FREEBSD")

set(CPACK_PACKAGE_NAME "scan-watch")
set(CPACK_PACKAGE_COMMENT "Watch for image uploads over SFTP, OCR them, and file them")
set(CPACK_PACKAGE_HOMEPAGE_URL "http://github.com/davidchisnall/scan-watch")
set(CPACK_FREEBSD_PACKAGE_LICENSE "MIT")
set(CPACK_FREEBSD_PACKAGE_MAINTAINER "theraven@FreeBSD.org")
set(CPACK_FREEBSD_PACKAGE_DEPS "lang/lua${LUA_VERSION_MAJOR}${LUA_VERSION_MINOR};graphics/tesseract")

include(CPack)
