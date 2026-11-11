add_library(mbp STATIC
    src/mbp.c
    src/mbp_clock.c
    src/mbp_config.c
    src/mbp_config_field.c
    src/mbp_decode.c
    src/mbp_health.c
    src/mbp_ipc.c
    src/mbp_ipc_path.c
    src/mbp_link.c
    src/mbp_poller.c
    src/mbp_proto.c
    src/mbp_sched.c
    src/mbp_serve.c
    src/mbp_store.c
    src/mbp_thread.c
)
target_include_directories(mbp PUBLIC include PRIVATE src)
target_compile_definitions(mbp PRIVATE MBP_VERBOSE_ERRORS=$<BOOL:${MBP_VERBOSE_ERRORS}>)
target_link_libraries(mbp PUBLIC PkgConfig::MODBUS PkgConfig::CJSON Threads::Threads m)

function(mbp_add_test name)
    add_executable(mbp_test_${name} test/mbp_test_${name}.c)
    target_include_directories(mbp_test_${name} PRIVATE src)
    target_compile_definitions(mbp_test_${name} PRIVATE MBP_VERBOSE_ERRORS=$<BOOL:${MBP_VERBOSE_ERRORS}>)
    target_link_libraries(mbp_test_${name} PRIVATE mbp)
    add_test(NAME ${name} COMMAND mbp_test_${name} ${PROJECT_SOURCE_DIR})
endfunction()

mbp_add_test(decode)
mbp_add_test(config)
mbp_add_test(sched)
mbp_add_test(health)
mbp_add_test(proto)
