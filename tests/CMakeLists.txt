add_executable(DevilDJTests
    DependencyLinkTests.cpp)

target_link_libraries(DevilDJTests PRIVATE
    Catch2::Catch2WithMain
    unofficial::sqlite3::sqlite3
    xxHash::xxhash
    devildj::compile_options)
