#pragma once

#include "library/SmartRules.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace devildj::ui
{

// The smart playlist rule builder (P2-07): a name, "match all / any of these rules", and rule rows of
// field - operator - value (two values for "between"; a key list for Key; days for date fields). + adds a row,
// - removes one. The line at the bottom says how many tracks match, updated as you edit.
class SmartPlaylistEditor final : public juce::Component
{
public:
    // counter: how many tracks the rules match (-1 = invalid); called after every edit.
    SmartPlaylistEditor(const juce::String& name, const library::SmartRules& rules,
                        std::function<int(const library::SmartRules&)> counter);
    ~SmartPlaylistEditor() override;

    std::function<void(const juce::String& name, const library::SmartRules& rules)> onSave;
    std::function<void()> onCancel;

    juce::String name() const;
    library::SmartRules rules() const;
    int numRules() const noexcept { return static_cast<int>(rows.size()); }
    void addRule(const library::Rule& rule = {});
    void removeRule(int index);
    void setMatchAll(bool all);
    juce::String countText() const { return count.getText(); }
    bool canSave() const;
    void save(); // the Save button

    // Text for a rule's value as typed: "5:30" -> 330 seconds, "#E03030" -> colour, numbers.
    static std::optional<double> parseValue(library::RuleField field, const juce::String& text);
    static juce::String valueText(library::RuleField field, double value);
    static juce::String fieldLabel(library::RuleField field);
    static juce::String opLabel(library::RuleOp op);

    void resized() override;
    void paint(juce::Graphics& g) override;

    class Row;

private:
    void changed();
    void layoutRows();

    std::function<int(const library::SmartRules&)> countTracks;
    juce::Label nameLabel, matchLabel, count;
    juce::TextEditor nameEditor;
    juce::ComboBox match;
    juce::TextButton addButton{"+ Add rule"}, saveButton{"Save"}, cancelButton{"Cancel"};
    juce::Viewport viewport;
    juce::Component rowHolder;
    std::vector<std::unique_ptr<Row>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SmartPlaylistEditor)
};

// Opens the editor in a dialog window over `parent`; onSave gets the result.
void showSmartPlaylistEditor(juce::Component* parent, const juce::String& title, const juce::String& name,
                             const library::SmartRules& rules, std::function<int(const library::SmartRules&)> counter,
                             std::function<void(const juce::String&, const library::SmartRules&)> onSave);

} // namespace devildj::ui
