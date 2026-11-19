# library: SQLite database + migrations, tags, track import, repositories. No JUCE.
add_library(devildj_library STATIC
    Connection.cpp
    ContentHash.cpp
    Database.cpp
    CueRepository.cpp
    Importer.cpp
    Migrations.cpp
    PlaylistRepository.cpp
    TagReader.cpp
    TagRepository.cpp
    TrackRepository.cpp)
add_library(devildj::library ALIAS devildj_library)

target_sources(devildj_library PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        Connection.h
        ContentHash.h
        Database.h
        CueRepository.h
        Importer.h
        Migrations.h
        PlaylistRepository.h
        RepositoryUtil.h
        TagReader.h
        TagRepository.h
        TrackRepository.h)

target_include_directories(devildj_library PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(devildj_library PUBLIC cxx_std_20)
target_link_libraries(devildj_library
    PUBLIC devildj::util
    PRIVATE devildj::compile_options TagLib::tag unofficial::sqlite3::sqlite3 xxHash::xxhash)
