# The poly engine's targets: the GDN-MMA library, the MMQ prompt path, the engine, its tests.

# Included from the root CMakeLists.txt; assumes the CUDA/strata_kernels setup already done there.


# The opt-in lifecycle is a host-only test, including on non-CUDA builds.
if(STRATA_BUILD_TESTS)
  add_executable(poly_hip_value_split_policy_test src/poly/hip_value_split_policy_test.cpp)
  add_test(NAME poly_hip_value_split_policy_test COMMAND poly_hip_value_split_policy_test)
  add_executable(poly_gdn_mma_control_test src/poly/gdn_mma_control_test.cpp)
  add_test(NAME poly_gdn_mma_control_test COMMAND poly_gdn_mma_control_test)
  add_executable(poly_q35_scratch_arena_test src/poly/q35_scratch_arena_test.cpp)
  add_test(NAME poly_q35_scratch_arena_test COMMAND poly_q35_scratch_arena_test)
  add_executable(poly_stream_stage_test src/poly/stage_copy_test.cpp)
  add_test(NAME poly_stream_stage_test COMMAND poly_stream_stage_test)
endif()


# Shared prepared-input GDN recurrence. Either engine may link this library;
# only the Poly Qwen35 adapter is integrated initially. Keep FP32/BF16 hi+lo math
# separate from the MMQ targets' -use_fast_math compilation options.
if(STRATA_NATIVE_EXPERTS AND STRATA_ENABLE_CUDA)
  if(STRATA_GGML_DIR)
    set(_strata_gdn_ggml ${STRATA_GGML_DIR}/ggml)
  else()
    set(_strata_gdn_ggml ${strata_llamacpp_SOURCE_DIR}/ggml)
  endif()
  add_library(strata_gdn_mma STATIC src/kernels/cuda/gdn_mma.cu)
  target_include_directories(strata_gdn_mma PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${_strata_gdn_ggml}/include ${_strata_gdn_ggml}/src ${_strata_gdn_ggml}/src/ggml-cuda)
  target_link_libraries(strata_gdn_mma PRIVATE ggml-base ${_strata_gpu_runtime_target})
  if(STRATA_BUILD_TESTS)
    add_executable(poly_gdn_mma_policy_test src/kernels/gdn_mma_policy_test.cpp)
    target_include_directories(poly_gdn_mma_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME poly_gdn_mma_policy_test COMMAND poly_gdn_mma_policy_test)
    add_executable(gdn_mma_test src/kernels/gdn_mma_test.cu)
    target_link_libraries(gdn_mma_test PRIVATE strata_gdn_mma ${_strata_gpu_runtime_target})
    add_test(NAME gdn_mma_test COMMAND gdn_mma_test)
    set_tests_properties(gdn_mma_test PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()


# ---- strata-poly: GLM 5.3 Flash (llama.cpp's glm5-next), read from its GGUF ----
#
# A second engine beside `strata`.  It shares the quantized matvec kernels and the GGUF reader, and takes the
# CPU experts' dot products from ggml-cpu.  On HIP it is the same CUDA-shaped code through the hip_compat headers
# (the wave32 RDNA backend; gfx906's wave64 compat layer is not supported here).
if(STRATA_NATIVE_EXPERTS AND STRATA_ENABLE_CUDA)
  # The prompt path's quantized products: llama.cpp's MMQ kernels (ggml-cuda, MIT) for the types the model's
  # GGUFs keep their matrices in (the list in src/poly/mmq.cu), from the checkout strata_mmq is compiled from and
  # with the same host symbols.  A library of its own, so that what `strata` multiplies with MMQ stays as it is.
  if(NOT _strata_mmq)   # a HIP build without STRATA_PREFILL_MMQ has not set these up yet
    if(STRATA_GGML_DIR)
      set(_strata_gg ${STRATA_GGML_DIR}/ggml)
    else()
      set(_strata_gg ${strata_llamacpp_SOURCE_DIR}/ggml)
    endif()
    set(_strata_mmq ${_strata_gg}/src/ggml-cuda)
  endif()
  set(_strata_poly_mmq_instances "")
  foreach(_t q4_0 q4_1 q5_0 q5_1 q8_0 q2_k q3_k q4_k q5_k q6_k iq1_s iq2_xxs iq2_xs iq2_s iq3_xxs iq3_s iq4_nl
             iq4_xs q2_0)
    list(APPEND _strata_poly_mmq_instances ${_strata_mmq}/template-instances/mmq-instance-${_t}.cu)
  endforeach()
  # MXFP4 and NVFP4: on RTX 50 llama.cpp's tiles use the FP4 MMA with block scales, which exists only in the
  # arch-specific target, so on CUDA their instances alone are built with 12x -> 12xa, as Strata's W4A4 unit is.  A
  # Blackwell other than 12.0 has no image of them, and Mmq::fits() leaves MXFP4 out there.  NVFP4 takes this
  # instance only on a 12.0 card with STRATA_PREFILL_NVFP4=w4a4; otherwise its tiles are mmq_nvfp4.cu's int8 ones,
  # built for the architectures as given.  On HIP there is no NVFP4 prompt product.
  set(_strata_poly_mxfp4 ${_strata_mmq}/template-instances/mmq-instance-mxfp4.cu)
    list(APPEND _strata_poly_mxfp4 ${_strata_mmq}/template-instances/mmq-instance-nvfp4.cu)
    list(APPEND _strata_poly_mmq_instances src/poly/mmq_nvfp4.cu)
    set(_strata_poly_fp4_archs ${CMAKE_CUDA_ARCHITECTURES})
    list(TRANSFORM _strata_poly_fp4_archs REPLACE "^(12[0-9])$" "\\1a")
    list(TRANSFORM _strata_poly_fp4_archs REPLACE "^(12[0-9])-(real|virtual)$" "\\1a-\\2")
    add_library(strata_poly_mmq_mxfp4 OBJECT ${_strata_poly_mxfp4})
    set_target_properties(strata_poly_mmq_mxfp4 PROPERTIES CUDA_ARCHITECTURES "${_strata_poly_fp4_archs}")
    target_include_directories(strata_poly_mmq_mxfp4 PRIVATE ${_strata_gg}/include ${_strata_gg}/src ${_strata_mmq})
    target_compile_options(strata_poly_mmq_mxfp4 PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--extended-lambda -use_fast_math>)
    if(MSVC)
      target_compile_options(strata_poly_mmq_mxfp4 PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/bigobj>)
    endif()
    list(APPEND _strata_poly_mmq_instances $<TARGET_OBJECTS:strata_poly_mmq_mxfp4>)
  if(STRATA_ENABLE_HIP)
    # CMake does not infer HIP from the .cu suffixes outside src/ (and drops a .cu source it has no language
    # for), and ggml's MMQ sources take its native HIP path (vendors/hip.h) instead of the hip_compat headers,
    # as strata_mmq's does.
    set_source_files_properties(${_strata_mmq}/quantize.cu ${_strata_poly_mmq_instances} PROPERTIES LANGUAGE HIP)
  endif()
  add_library(strata_poly_mmq STATIC src/poly/mmq.cu src/poly/fattn.cu src/poly/prefill/ggml_cuda_host.cu ${_strata_mmq}/quantize.cu
              ${_strata_poly_mmq_instances})
  target_include_directories(strata_poly_mmq PRIVATE ${_strata_gg}/include ${_strata_gg}/src ${_strata_mmq}
                                                    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(strata_poly_mmq PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--extended-lambda -use_fast_math>)
  if(MSVC)
    target_compile_options(strata_poly_mmq PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/bigobj>)
  endif()
    target_compile_definitions(strata_poly_mmq PRIVATE STRATA_POLY_NVFP4=1)
    target_link_libraries(strata_poly_mmq PUBLIC ggml-base ${_strata_gpu_runtime_target})

  # the routed experts on the CPU (cpu_experts.cpp) and its AVX2 Q2_0 / IQ1_S dot products (cpu_lowbit.cpp, which
  # takes ggml-common.h from ggml's src), for every target that runs them
  add_library(strata_poly_cpu STATIC src/poly/cpu_experts.cpp src/poly/cpu_lowbit.cpp)
  target_include_directories(strata_poly_cpu PUBLIC ${_strata_gg}/include)
  set_source_files_properties(src/poly/cpu_lowbit.cpp PROPERTIES INCLUDE_DIRECTORIES ${_strata_gg}/src)
  target_link_libraries(strata_poly_cpu PUBLIC ggml-cpu ggml-base Threads::Threads)

  if(STRATA_ENABLE_CUDA)
    target_compile_definitions(strata_poly_mmq PUBLIC STRATA_POLY_GDN_MMA=1)
    target_link_libraries(strata_poly_mmq PUBLIC strata_gdn_mma)

    add_executable(poly_gdn_mma_test src/poly/gdn_mma_test.cu src/poly/kernels.cu)
    target_include_directories(poly_gdn_mma_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(poly_gdn_mma_test PRIVATE strata_poly_mmq ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
    add_test(NAME poly_gdn_mma_test COMMAND poly_gdn_mma_test)
    set_tests_properties(poly_gdn_mma_test PROPERTIES SKIP_RETURN_CODE 77)
    # Optional model/state check; run by hand with an MTP GGUF and frozen prompt IDs.
    add_executable(poly_gdn_mma_state_test src/poly/gdn_mma_state_test.cpp src/poly/engine.cpp src/poly/glm5next.cpp
                   src/poly/qwen35.cpp src/poly/gemma4.cpp src/poly/deepseek4.cpp src/poly/lfm2moe.cpp
                   src/poly/flint.cpp src/poly/kolibri1.cpp src/poly/inkling.cpp src/poly/kernels.cu
                   src/poly/kernels_dsv4.cu src/poly/kernels_inkling.cu src/poly/conversation_memory.cpp)
    target_include_directories(poly_gdn_mma_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${_strata_gpu_include_directories})
    target_link_libraries(poly_gdn_mma_state_test PRIVATE strata_poly_mmq strata_poly_cpu strata_kernels strata_spec
                         ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target} CUDA::cuda_driver)
  endif()

  add_executable(strata-poly src/poly/main.cpp src/poly/engine.cpp src/poly/glm5next.cpp src/poly/qwen35.cpp
                            src/poly/gemma4.cpp src/poly/deepseek4.cpp src/poly/lfm2moe.cpp src/poly/flint.cpp
                            src/poly/kolibri1.cpp src/poly/inkling.cpp src/poly/kernels.cu src/poly/kernels_dsv4.cu
                            src/poly/kernels_inkling.cu src/poly/spill.cpp src/poly/conversation_memory.cpp
                            src/core/conversation_file.cpp
                            # host-only: the GBNF grammar module (constrained decoding)
                            src/core/grammar.cpp)
  target_include_directories(strata-poly PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${_strata_gpu_include_directories})
  # the driver's virtual memory calls: the attention caches grow with the conversation; on HIP they are HIP's
  # memory management calls, which the HIP runtime itself provides (hip_compat/cuda.h)
    set(_strata_poly_driver CUDA::cuda_driver)
  target_link_libraries(strata-poly PRIVATE strata_poly_mmq strata_poly_cpu strata_kernels strata_spec
                                           ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target}
                                           ${_strata_poly_driver})
  # needs the model file, so it is run by hand: poly_expert_parity MODEL.gguf
  add_executable(poly_expert_parity src/poly/expert_parity.cpp src/poly/kernels.cu)
  target_include_directories(poly_expert_parity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                       ${_strata_gpu_include_directories})
  target_link_libraries(poly_expert_parity PRIVATE strata_poly_mmq strata_poly_cpu strata_kernels
                                                  ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  # a read with the draft block repeats bit for bit; needs the model file, so it is run by hand:
  # poly_draft_race_test MODEL.gguf [RUNS] [LAYER_SPLIT]
  add_executable(poly_draft_race_test src/poly/draft_race_test.cpp src/poly/engine.cpp src/poly/glm5next.cpp
                                      src/poly/qwen35.cpp src/poly/gemma4.cpp src/poly/deepseek4.cpp
                                      src/poly/lfm2moe.cpp src/poly/flint.cpp src/poly/kolibri1.cpp
                                      src/poly/inkling.cpp src/poly/kernels.cu src/poly/kernels_dsv4.cu
                                      src/poly/kernels_inkling.cu src/poly/conversation_memory.cpp)
  target_include_directories(poly_draft_race_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                         ${_strata_gpu_include_directories})
  target_link_libraries(poly_draft_race_test PRIVATE strata_poly_mmq strata_poly_cpu strata_kernels strata_spec
                                                    ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target}
                                                    ${_strata_poly_driver})
  # the engine's logits for a list of tokens as raw floats, to compare with llama.cpp's; needs the model file, so it
  # is run by hand: poly_logits_dump MODEL.gguf --tokens ID,ID,... (its usage says the rest)
  add_executable(poly_logits_dump src/poly/logits_dump.cpp src/poly/engine.cpp src/poly/glm5next.cpp
                              src/poly/qwen35.cpp src/poly/gemma4.cpp src/poly/deepseek4.cpp src/poly/lfm2moe.cpp
                              src/poly/flint.cpp src/poly/kolibri1.cpp src/poly/inkling.cpp src/poly/kernels.cu
                              src/poly/kernels_dsv4.cu src/poly/kernels_inkling.cu
                              src/poly/conversation_memory.cpp)
  target_include_directories(poly_logits_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                        ${_strata_gpu_include_directories})
  target_link_libraries(poly_logits_dump PRIVATE strata_poly_mmq strata_poly_cpu strata_kernels strata_spec
                                                  ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target}
                                                  ${_strata_poly_driver})
    
  add_executable(poly_route_test src/poly/route_test.cu src/poly/kernels.cu)
  target_include_directories(poly_route_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                    ${_strata_gpu_include_directories})
  target_link_libraries(poly_route_test PRIVATE ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  # the fused residual sum, RMS norm and Q8_1 quantization against the three launches; needs a card, run by hand
  add_executable(poly_norm_q8_test src/poly/norm_q8_test.cu src/poly/kernels.cu)
  target_include_directories(poly_norm_q8_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                      ${_strata_gpu_include_directories})
  target_link_libraries(poly_norm_q8_test PRIVATE strata_kernels ${_strata_gpu_blas_target}
                                                  ${_strata_gpu_runtime_target})
  # a prompt batch's products with few outputs against the same sums on the host; needs a card, run by hand
  add_executable(poly_gemm_small_test src/poly/gemm_small_test.cu src/poly/kernels.cu)
  target_include_directories(poly_gemm_small_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                         ${_strata_gpu_include_directories})
  target_link_libraries(poly_gemm_small_test PRIVATE ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  # attend()'s blocks of rows against its other kernels and itself; needs a card, so it is run by hand
  add_executable(poly_attend_test src/poly/attend_test.cu src/poly/kernels.cu)
  target_include_directories(poly_attend_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                     ${_strata_gpu_include_directories})
  target_link_libraries(poly_attend_test PRIVATE ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  add_executable(poly_hip_value_split_test src/poly/hip_value_split_test.cu src/poly/kernels.cu)
  target_include_directories(poly_hip_value_split_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                            ${_strata_gpu_include_directories})
  target_link_libraries(poly_hip_value_split_test PRIVATE ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  add_test(NAME poly_hip_value_split_test COMMAND poly_hip_value_split_test)
  set_tests_properties(poly_hip_value_split_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
  # STRATA_HIP_SPLIT_ATTEND (heads of 256 over a q8_0 cache): against an FP64 reference and the old kernels, the position on the card, the
  # fallback and a graph replay; needs a card with 32-lane waves (77 = skipped)
  add_executable(poly_hip_split_attend_test src/poly/hip_split_attend_test.cu src/poly/kernels.cu)
  target_include_directories(poly_hip_split_attend_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                            ${_strata_gpu_include_directories})
  target_link_libraries(poly_hip_split_attend_test PRIVATE ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  add_test(NAME poly_hip_split_attend_test COMMAND poly_hip_split_attend_test)
  set_tests_properties(poly_hip_split_attend_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
  # attend_batch() against attend() with Flint's heads, sinks and window ring; needs a card
  add_executable(poly_sink_attend_test src/poly/sink_attend_test.cu src/poly/kernels.cu)
  target_include_directories(poly_sink_attend_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                          ${_strata_gpu_include_directories})
  target_link_libraries(poly_sink_attend_test PRIVATE ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  # STRATA_HIP_PREFILL_FP32 (attend_batch() on FP32 inputs over a q8_0 cache): against an FP64 reference and the default path, and the
  # fallbacks; needs a card (77 = skipped)
  add_executable(poly_hip_prefill_fp32_test src/poly/hip_prefill_fp32_test.cu src/poly/kernels.cu)
  target_include_directories(poly_hip_prefill_fp32_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                            ${_strata_gpu_include_directories})
  target_link_libraries(poly_hip_prefill_fp32_test PRIVATE ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  add_test(NAME poly_hip_prefill_fp32_test COMMAND poly_hip_prefill_fp32_test)
  set_tests_properties(poly_hip_prefill_fp32_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
  # fattn_q8() and fattn_f16() (llama.cpp's flash-attention kernel) against attend_batch(), and with `dec` fdec_q8()
  # against attend(); needs a card, run by hand
  add_executable(poly_fattn_test src/poly/fattn_test.cu src/poly/kernels.cu)
  target_include_directories(poly_fattn_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/poly
                                                    ${_strata_gpu_include_directories})
  target_link_libraries(poly_fattn_test PRIVATE strata_poly_mmq ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  # STRATA_POLY_STAGED_TILES: the staged experts in size classes, each a launch of its own tile width, against one
  # launch, bit for bit; needs a card
  add_executable(poly_mmq_tiles_test src/poly/mmq_tiles_test.cu)
  target_include_directories(poly_mmq_tiles_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/poly
                                                        ${_strata_gpu_include_directories})
  target_link_libraries(poly_mmq_tiles_test PRIVATE strata_poly_mmq ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  add_test(NAME poly_mmq_tiles_test COMMAND poly_mmq_tiles_test)
  set_tests_properties(poly_mmq_tiles_test PROPERTIES SKIP_RETURN_CODE 77)
  # LFM2's short convolution against the same arithmetic on the host; needs a card, so it is run by hand
  add_executable(poly_shortconv_test src/poly/shortconv_test.cu src/poly/kernels.cu)
  target_include_directories(poly_shortconv_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                        ${_strata_gpu_include_directories})
  target_link_libraries(poly_shortconv_test PRIVATE ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target})
  # DeepSeek V4's kernels against the same arithmetic on the host; needs a card, so it is run by hand
  add_executable(poly_dsv4_test src/poly/dsv4_test.cu src/poly/kernels_dsv4.cu)
  target_include_directories(poly_dsv4_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                   ${_strata_gpu_include_directories})
  target_link_libraries(poly_dsv4_test PRIVATE ${_strata_gpu_runtime_target})
  # the pool of the CPU experts, without a model
  add_executable(poly_workers_test src/poly/workers_test.cpp)
  target_link_libraries(poly_workers_test PRIVATE strata_poly_cpu)
  add_test(NAME poly_workers_test COMMAND poly_workers_test)
  # the card's MXFP4 product against ggml's dequantizer on random blocks, without a model; needs a card, run by hand
  add_executable(poly_mxfp4_test src/poly/mxfp4_test.cu)
  target_include_directories(poly_mxfp4_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${_strata_gg}/include
                                                    ${_strata_gpu_include_directories})
  target_link_libraries(poly_mxfp4_test PRIVATE strata_kernels ggml-base ${_strata_gpu_runtime_target})
  # the Q8_0 KV cache kernels (store_q8 / store_q8_at) against a host reference (GPU, synthetic, no model)
  if(STRATA_BUILD_TESTS)
    add_executable(poly_kv_q8_test src/poly/kv_q8_test.cpp src/poly/kernels.cu)
    target_include_directories(poly_kv_q8_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                       ${_strata_gpu_include_directories})
    target_link_libraries(poly_kv_q8_test PRIVATE ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target}
                                                  Threads::Threads)
    add_test(NAME poly_kv_q8_test COMMAND poly_kv_q8_test)
  endif()
  # --kv int8/int8k's kernels (store_int8 / attend_int8) against ggml's own quantizer and against attend() over the
  # same numbers in fp16.  The reference is ggml-cpu's quantize_row_q8_0_ref, hence the ggml links; under HIP the
  # mma tier's probes are behind STRATA_USE_HIP (the dp4a tier is what runs there anyway).
  if(STRATA_BUILD_TESTS)
    add_executable(poly_kv_int8_test src/poly/kv_int8_test.cpp src/poly/kernels.cu)
    target_include_directories(poly_kv_int8_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                      ${_strata_gpu_include_directories} ${_strata_gg}/include)
    target_link_libraries(poly_kv_int8_test PRIVATE ${_strata_gpu_blas_target} ${_strata_gpu_runtime_target}
                                                  ggml-cpu ggml-base Threads::Threads)
    add_test(NAME poly_kv_int8_test COMMAND poly_kv_int8_test)
  endif()
endif()

