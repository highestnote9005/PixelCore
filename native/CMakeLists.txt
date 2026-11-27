cmake_minimum_required(VERSION 3.10)
project(pocketgba C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -fPIC -fvisibility=hidden")

add_library(pocketgba SHARED gba.c host.c)
target_link_libraries(pocketgba m)

if(ANDROID)
  target_link_libraries(pocketgba OpenSLES log)
  # Android 15+ devices may use 16 KB memory pages
  target_link_options(pocketgba PRIVATE -Wl,-z,max-page-size=16384)
else()
  target_link_libraries(pocketgba pthread)
endif()
