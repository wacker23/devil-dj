# library: SQLite database + migrations, tags, track import, repositories. No JUCE.
add_library(devildj_library STATIC
    BeatgridRepository.cpp
    PhraseRepository.cpp
    CueFeedbackRepository.cpp
    Connection.cpp
    ContentHash.cpp
    Database.cpp
    ForeignLibrary.cpp
    HistoryRepository.cpp
    CueRepository.cpp
    Importer.cpp
    Migrations.cpp
    MissingFiles.cpp
    PlaylistFiles.cpp
    PlaylistRepository.cpp
    SmartRules.cpp
    TagReader.cpp
    TagRepository.cpp
    TrackRepository.cpp
    XmlReader.cpp)
add_library(devildj::library ALIAS devildj_library)

target_sources(devildj_library PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        BeatgridRepository.h
        PhraseRepository.h
        CueFeedbackRepository.h
        Connection.h
        ContentHash.h
        Database.h
        ForeignLibrary.h
        HistoryRepository.h
        CueRepository.h
        Importer.h
        Migrations.h
        MissingFiles.h
        PlaylistFiles.h
        PlaylistRepository.h
        SmartRules.h
        RepositoryUtil.h
        TagReader.h
        TagRepository.h
        TrackRepository.h
        XmlReader.h)

target_include_directories(devildj_library PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(devildj_library PUBLIC cxx_std_20)
target_link_libraries(devildj_library
    PUBLIC devildj::util
    PRIVATE devildj::compile_options TagLib::tag unofficial::sqlite3::sqlite3 xxHash::xxhash)
