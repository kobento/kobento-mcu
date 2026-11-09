# RemoteCompose core engine from the sibling kobento-libs checkout (where it moved from
# remotecompose-experiments). Override with -DKOBENTO_LIBS=/path/to/kobento-libs.
if(NOT DEFINED KOBENTO_LIBS)
    get_filename_component(KOBENTO_LIBS ${CMAKE_CURRENT_LIST_DIR}/../../../kobento-libs ABSOLUTE)
endif()
set(RCCORE_DIR ${KOBENTO_LIBS}/lib/rccore)
file(GLOB_RECURSE RCCORE_SRCS ${RCCORE_DIR}/src/*.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${RCCORE_SRCS} INCLUDE_DIRS ${RCCORE_DIR}/include)
    # -ffp-contract=off: required for bit-exact 3D parity with the Java reference.
    target_compile_options(${COMPONENT_LIB} PRIVATE -ffp-contract=off -O2 -Wno-error
        -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-sign-compare
        -Wno-missing-field-initializers -Wno-reorder -Wno-format)
else()
    add_library(rccore STATIC ${RCCORE_SRCS})
    target_include_directories(rccore PUBLIC ${RCCORE_DIR}/include)
    target_compile_features(rccore PUBLIC cxx_std_17)
    target_compile_options(rccore PRIVATE -ffp-contract=off -w)
endif()
