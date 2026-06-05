cmake_minimum_required(VERSION 3.20)

project(fraud_api)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_API "Build the fraud API server" ON)

find_package(Boost REQUIRED COMPONENTS iostreams)
find_package(nlohmann_json CONFIG REQUIRED)

if(BUILD_API)
    find_package(unofficial-uwebsockets CONFIG REQUIRED)

    add_executable(fraud_api
        src/main.cpp
        src/vectorize.cpp
        src/vectorSearch.cpp
    )

    target_link_libraries(fraud_api
        PRIVATE
            unofficial::uwebsockets::uwebsockets
            nlohmann_json::nlohmann_json
    )

    # Haswell e a ISA garantida do runner oficial (Mac Mini Late 2014). Evitamos
    # -march=native porque o build ocorre em outra maquina.
    target_compile_options(fraud_api PRIVATE -O3 -march=haswell -mtune=haswell -mavx2 -funroll-loops)
    set_target_properties(fraud_api PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
endif()

add_executable(preprocess_references
    src/preProccess.cpp
)

target_link_libraries(preprocess_references
    PRIVATE
        Boost::iostreams
        nlohmann_json::nlohmann_json
)

add_executable(vector_benchmark
    src/benchmark.cpp
    src/vectorize.cpp
    src/vectorSearch.cpp
)

target_link_libraries(vector_benchmark
    PRIVATE
        nlohmann_json::nlohmann_json
)

target_compile_options(vector_benchmark PRIVATE -mavx2)
