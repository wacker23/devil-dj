# matcher: the Track Matcher (MATCHER_SPEC; P3-19 layer 1) - features, scoring, the SQL prefilter, the worker service.
# Pure C++ over the library; no JUCE.
add_library(devildj_matcher STATIC
    Camelot.cpp
    ChoiceLog.cpp
    CandidateQuery.cpp
    Features.cpp
    MatcherService.cpp
    Ranker.cpp
    Scorer.cpp)
add_library(devildj::matcher ALIAS devildj_matcher)

target_sources(devildj_matcher PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        Camelot.h
        ChoiceLog.h
        CandidateQuery.h
        Features.h
        MatcherService.h
        Ranker.h
        Scorer.h)

target_include_directories(devildj_matcher PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(devildj_matcher PUBLIC cxx_std_20)
target_link_libraries(devildj_matcher
    PUBLIC devildj::util devildj::library devildj::analysis
    PRIVATE devildj::compile_options)
