set(TARGET_NAME ggml-hexagon)
ggml_add_backend_library(${TARGET_NAME}
    ggml-hexagon.cpp htp-kparams.cpp htp-opnode.cpp htp-fusion.cpp htp-repack.cpp htp-drv.cpp)
target_include_directories(${TARGET_NAME} PRIVATE "${QCOM_NPU_SOURCE}/ggml" "${QCOM_NPU_SOURCE}/runtime" "${QCOM_NPU_SOURCE}/runtime/vendor" "${CMAKE_CURRENT_SOURCE_DIR}/htp")
target_compile_definitions(${TARGET_NAME} PRIVATE STATIC_LIB _CRT_SECURE_NO_WARNINGS)
target_compile_options(${TARGET_NAME} PRIVATE /wd4068 /wd4244 /wd4267)
target_compile_options(${TARGET_NAME} PRIVATE "/FI${QCOM_NPU_SOURCE}/ggml/compat.h")
target_link_libraries(${TARGET_NAME} PRIVATE advapi32)
