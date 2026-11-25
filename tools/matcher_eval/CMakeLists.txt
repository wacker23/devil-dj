# matcher_eval (P6-13): the Track Matcher evaluated on the owner's history - a report in docs/reports.
add_executable(matcher_eval main.cpp)
target_link_libraries(matcher_eval PRIVATE devildj::matcher devildj::library devildj::compile_options shell32 ole32)
set_target_properties(matcher_eval PROPERTIES OUTPUT_NAME "matcher_eval")
