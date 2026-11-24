# codebeat: the Code Beat live-coding deck (CODEBEAT_SPEC). P5-07: the quickjs-ng sandbox with the pattern language
# (js/codebeat/*.js) embedded as binary data. A plain static library: no JUCE code, so it builds once.

# The JS library, embedded: every js/codebeat/*.js except the tests, looked up by file name at import time.
file(GLOB devildj_codebeat_js_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/js/codebeat/*.js")
juce_add_binary_data(devildj_codebeat_js
    HEADER_NAME CodeBeatJs.h
    NAMESPACE CodeBeatJs
    SOURCES ${devildj_codebeat_js_files})

# The editor's help (P5-16): the reference table and the starter examples, embedded for the ui library.
file(GLOB devildj_codebeat_examples CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/help/examples/*.js")
juce_add_binary_data(devildj_codebeat_help
    HEADER_NAME CodeBeatHelp.h
    NAMESPACE CodeBeatHelp
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/help/reference.json" ${devildj_codebeat_examples})

add_library(devildj_codebeat STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/CodeEvaluator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/CodeEvaluator.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/JsRuntime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/PreTokenizer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/PreTokenizer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/JsRuntime.h")
add_library(devildj::codebeat ALIAS devildj_codebeat)

target_include_directories(devildj_codebeat PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(devildj_codebeat
    PUBLIC devildj::compile_options
    PRIVATE qjs devildj_codebeat_js)
