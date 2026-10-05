
include_directories(. ../.. ../debug ../includes ../gui-sdl ${SDL3_INCLUDE_DIRS}) 

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-write-strings")

add_library(Dimension
    dimension.cpp nd_mem.cpp nd_nbic.cpp nd_devs.cpp nd_vio.cpp nd_rom.cpp
    i860.cpp i860dis.cpp nd_sdl.cpp
)

target_link_libraries(Dimension PRIVATE ${SDL3_LIBRARIES})

if(ENABLE_ND_RUST)
	find_program(CARGO cargo REQUIRED)
	set(ND_FFI_LIB ${ND_RUST_DIR}/target/release/libnd_ffi.a)
	add_custom_target(nd_ffi_build
		COMMAND ${CARGO} build --release -p nd-ffi
		WORKING_DIRECTORY ${ND_RUST_DIR}
		BYPRODUCTS ${ND_FFI_LIB}
		COMMENT "Building nd-ffi with cargo"
		USES_TERMINAL)
	target_sources(Dimension PRIVATE nd_rust.cpp)
	target_include_directories(Dimension PUBLIC ${ND_RUST_DIR}/nd-ffi/include)
	add_dependencies(Dimension nd_ffi_build)
	target_link_libraries(Dimension PUBLIC ${ND_FFI_LIB})
endif()
