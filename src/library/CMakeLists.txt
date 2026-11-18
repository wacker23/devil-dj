# library: SQLite database + migrations, tags; importers and repositories follow in Phase 2. No JUCE.
add_library(devildj_library STATIC
    Connection.cpp
    Database.cpp
    Migrations.cpp
    TagReader.cpp)
add_library(devildj::library ALIAS devildj_library)

target_sources(devildj_library PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        Connection.h
        Database.h
        Migrations.h
        TagReader.h)

target_include_directories(devildj_library PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(devildj_library PUBLIC cxx_std_20)
target_link_libraries(devildj_library
    PUBLIC devildj::util
    PRIVATE devildj::compile_options TagLib::tag unofficial::sqlite3::sqlite3)
