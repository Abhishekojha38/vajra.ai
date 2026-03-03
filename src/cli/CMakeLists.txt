add_executable(aham main.c)

target_link_libraries(aham
    core
    agent
    providers
    ollama
    provider_openai
    memory
    history
    security
    shell
    file_ops
    scheduler
    skills
    serial
    heartbeat
    ux
    gateway
    CURL::libcurl
    ${DL_LIBRARY}
    pthread
    m
)

if(READLINE_LIBRARY)
    target_link_libraries(aham ${READLINE_LIBRARY})
    target_compile_definitions(aham PRIVATE HAS_READLINE)
endif()
