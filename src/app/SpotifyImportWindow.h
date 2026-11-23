#pragma once

// The import window (P4-21): the owner's Spotify playlists (tick the ones to import; Space ticks the selected row),
// IMPORT -> for each, its tracks matched to the library -> a review: the counts, every ambiguous track with its
// candidates to pick from (or skip), the missing ones (copy / save as text) -> CREATE PLAYLISTS. Everything works
// from the keyboard (Tab between the controls, arrows / Space in the lists, Enter on the buttons).
// The work (network, matching) runs on a worker thread; what it needs comes in as functions, so it is testable.

#include "app/SpotifyApi.h"
#include "app/SpotifyImport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace devildj::app
{

class SpotifyImportWindow final : public juce::Component, private juce::ListBoxModel
{
public:
    struct Source
    {
        std::function<util::Result<std::vector<RemotePlaylist>>()> playlists;
        std::function<util::Result<std::vector<RemoteTrack>>(const std::string& id)> tracks;
        std::function<std::vector<LibraryTrack>()> library;
        std::function<util::Result<std::int64_t>(const ImportPlan& plan)> create; // the playlist made
        std::function<void(const juce::String& text)> saveText;                  // the missing list to a file
    };

    explicit SpotifyImportWindow(Source source);
    ~SpotifyImportWindow() override;

    void start();                       // fetches the playlists
    std::function<void(int created)> onDone;

    // State, for tests.
    const std::vector<RemotePlaylist>& playlists() const noexcept { return lists; }
    std::vector<bool>& ticked() noexcept { return chosen; }
    const std::vector<ImportPlan>& plans() const noexcept { return results; }
    std::vector<ImportPlan>& plansToEdit() noexcept { return results; }
    juce::String status() const { return statusText; }
    bool busy() const noexcept { return working; }
    void importChosen();   // IMPORT
    int createPlaylists(); // CREATE PLAYLISTS: how many were made
    juce::String allMissing() const;
    juce::ComboBox* choiceBox(int index); // the review's ambiguous rows

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override;
    void returnKeyPressed(int row) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void showReview();
    void setStatus(const juce::String& s);

    Source source;
    std::vector<RemotePlaylist> lists;
    std::vector<bool> chosen;
    std::vector<ImportPlan> results;
    std::map<std::int64_t, juce::String> names; // library track -> "Artist - Title"
    juce::String statusText = "Reading your Spotify playlists...";
    bool working = false;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    juce::ListBox list{"Spotify playlists", this};
    juce::TextButton importButton{"IMPORT"}, createButton{"CREATE PLAYLISTS"}, copyButton{"COPY MISSING"},
        saveButton{"SAVE MISSING..."}, backButton{"BACK"};
    juce::Label summary;
    juce::Viewport reviewView;
    juce::Component reviewContent;
    std::vector<std::unique_ptr<juce::ComboBox>> choices;
    std::vector<std::pair<int, int>> choiceAt; // plan, track
    std::vector<std::unique_ptr<juce::Label>> choiceLabels;
    juce::TextEditor missingText;
    bool reviewing = false;
};

} // namespace devildj::app
