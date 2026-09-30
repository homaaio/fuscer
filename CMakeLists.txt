cmake_minimum_required(VERSION 3.10)
project(fuscer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # POSIX/GNU features (_GNU_SOURCE)

find_package(Threads REQUIRED)

file(GLOB LIB_SRC src/*.c)
list(REMOVE_ITEM LIB_SRC ${CMAKE_SOURCE_DIR}/src/main.c)

add_library(fuscer_core STATIC ${LIB_SRC})
target_include_directories(fuscer_core PUBLIC src)
target_compile_options(fuscer_core PRIVATE -Wall -Wextra -Wshadow)
target_link_libraries(fuscer_core PUBLIC Threads::Threads)

add_executable(fuscer src/main.c)
target_compile_options(fuscer PRIVATE -Wall -Wextra -Wshadow)
target_link_libraries(fuscer PRIVATE fuscer_core)

enable_testing()
add_executable(test_proto tests/test_proto.c)
target_link_libraries(test_proto PRIVATE fuscer_core)
add_test(NAME proto COMMAND test_proto)
add_test(NAME smoke COMMAND sh ${CMAKE_SOURCE_DIR}/tests/smoke.sh $<TARGET_FILE:fuscer>)

install(TARGETS fuscer RUNTIME DESTINATION bin)
