#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace devildj::ui
{

// The deck's memory cues and loops as a list (P3-02), opened from its MEM button: a click (or tap) calls one;
// "Add here" stores the playhead; Rename / Delete act on the selected row. The app owns the cues and refills the
// list when they change (setRows).
class MemoryCuePanel final : public juce::Component, private juce::ListBoxModel
{
public:
    struct Row
    {
        juce::String time;  // "1:23.4"
        bool loop = false;
        juce::String label;
        std::optional<std::uint32_t> colour;
    };

    MemoryCuePanel();
    void setRows(std::vector<Row> rows, const juce::String& title);

    std::function<void(int row)> onCall;
    std::function<void()> onAdd;
    std::function<void(int row)> onRename;
    std::function<void(int row)> onDelete;

    void resized() override;
    void paint(juce::Graphics& g) override;

    // For tests.
    juce::ListBox& list() noexcept { return listBox; }
    juce::TextButton& addButton() noexcept { return add; }
    int numRows() const noexcept { return static_cast<int>(rows.size()); }
    void callRow(int row); // what a click on a row does

private:
    int getNumRows() override { return static_cast<int>(rows.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void selectedRowsChanged(int) override;

    std::vector<Row> rows;
    juce::String heading;
    juce::ListBox listBox;
    juce::TextButton add{"Add here"}, rename{"Rename"}, remove{"Delete"};
};

} // namespace devildj::ui
