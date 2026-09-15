cmake_minimum_required(VERSION 3.10)
project(graph_compressor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(run
    src/main.cpp
    src/app/application.cpp
    src/app/options.cpp
    src/codec/integer_model.cpp
    src/codec/range_coder.cpp
    src/graph/graph.cpp
    src/graph/serializer.cpp
    src/graph/tsv_reader.cpp
    src/graph/tsv_writer.cpp
    src/io/byte_input.cpp
    src/io/byte_output.cpp
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(run PRIVATE -O2 -Wall -Wextra)
endif()
