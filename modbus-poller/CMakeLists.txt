add_library(mbp STATIC
    src/mbp.c
    src/mbp_decode.c
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
