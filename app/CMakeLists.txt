add_executable(X3Engine WIN32 main.cpp)

target_link_libraries(X3Engine PRIVATE x3engine glfw)

# WIN32 subsystem but we want a console for logs during dev:
if(MSVC)
    set_target_properties(X3Engine PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()

set_target_properties(X3Engine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
