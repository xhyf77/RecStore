find_package(Torch REQUIRED)

add_library(framework_gpu_cache STATIC
    gpu_embedding_cache.cu
)

target_compile_definitions(framework_gpu_cache PUBLIC RECSTORE_ENABLE_GPU_CACHE)

target_include_directories(framework_gpu_cache PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${Python_INCLUDE_DIRS}
    ${TORCH_INCLUDE_DIRS}
)

target_link_libraries(framework_gpu_cache PUBLIC
    gpu_cache
    ${TORCH_LIBRARIES}
)

target_compile_options(framework_gpu_cache PUBLIC
    $<$<COMPILE_LANGUAGE:CUDA>:--expt-relaxed-constexpr>
)

set_target_properties(framework_gpu_cache PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CUDA_ARCHITECTURES OFF
    POSITION_INDEPENDENT_CODE ON
)
