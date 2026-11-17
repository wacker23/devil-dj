# Third-party dependencies fetched at configure time.
# Every entry here must also be listed in docs/LICENSES.md (checked by tools/check_licenses.py).

include(FetchContent)

# JUCE 8: app framework, audio I/O, UI. Licence: JUCE Starter EULA (docs/LICENSES.md, decision D6).
set(DEVILDJ_JUCE_TAG 8.0.15)
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG ${DEVILDJ_JUCE_TAG}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(juce)
