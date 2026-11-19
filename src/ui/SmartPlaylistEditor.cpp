#include "SmartPlaylistEditor.h"

#include "ui/TrackTableModel.h"

#include <algorithm>
#include <cmath>

namespace devildj::ui
{

namespace
{
using library::Rule;
using library::RuleField;
using library::RuleOp;

const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffd8d8e0);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kAccent(0xffe0483c);
constexpr int kRowHeight = 30;

const RuleField kAllFields[] = {RuleField::Title,     RuleField::Artist,     RuleField::Album,     RuleField::Genre,
                                RuleField::Comment,   RuleField::Tag,        RuleField::Bpm,       RuleField::Key,
                                RuleField::Rating,    RuleField::Colour,     RuleField::Year,      RuleField::Time,
                                RuleField::DateAdded, RuleField::LastPlayed, RuleField::PlayCount};

bool usesDays(RuleOp op)
{
    return op == RuleOp::InLast || op == RuleOp::NotInLast;
}

bool needsValue(RuleOp op)
{
    return op != RuleOp::IsEmpty && op != RuleOp::IsNotEmpty;
}

void style(juce::TextEditor& e)
{
    e.setColour(juce::TextEditor::backgroundColourId, kPanel);
    e.setColour(juce::TextEditor::textColourId, kText);
    e.setColour(juce::TextEditor::outlineColourId, kPanel.brighter(0.2f));
    e.setColour(juce::TextEditor::focusedOutlineColourId, kAccent);
    e.setFont(juce::FontOptions(14.0f));
}

void style(juce::ComboBox& c)
{
    c.setColour(juce::ComboBox::backgroundColourId, kPanel);
    c.setColour(juce::ComboBox::textColourId, kText);
    c.setColour(juce::ComboBox::outlineColourId, kPanel.brighter(0.2f));
}

void style(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::textColourOffId, kText);
}
} // namespace

// --- labels and values ------------------------------------------------------------------------------------------

juce::String SmartPlaylistEditor::fieldLabel(RuleField f)
{
    switch (f)
    {
        case RuleField::Title: return "Title";
        case RuleField::Artist: return "Artist";
        case RuleField::Album: return "Album";
        case RuleField::Genre: return "Genre";
        case RuleField::Comment: return "Comment";
        case RuleField::Tag: return "Tag";
        case RuleField::Bpm: return "BPM";
        case RuleField::Key: return "Key";
        case RuleField::Rating: return "Rating (stars)";
        case RuleField::Colour: return "Colour";
        case RuleField::Year: return "Year";
        case RuleField::Time: return "Length";
        case RuleField::DateAdded: return "Date added";
        case RuleField::LastPlayed: return "Last played";
        case RuleField::PlayCount: return "Play count";
    }
    return {};
}

juce::String SmartPlaylistEditor::opLabel(RuleOp op)
{
    switch (op)
    {
        case RuleOp::Contains: return "contains";
        case RuleOp::NotContains: return "does not contain";
        case RuleOp::Is: return "is";
        case RuleOp::IsNot: return "is not";
        case RuleOp::StartsWith: return "starts with";
        case RuleOp::GreaterThan: return "is more than";
        case RuleOp::LessThan: return "is less than";
        case RuleOp::Between: return "is between";
        case RuleOp::InLast: return "is in the last";
        case RuleOp::NotInLast: return "is not in the last";
        case RuleOp::IsEmpty: return "is empty";
        case RuleOp::IsNotEmpty: return "is not empty";
    }
    return {};
}

std::optional<double> SmartPlaylistEditor::parseValue(RuleField f, const juce::String& raw)
{
    const auto text = raw.trim().replaceCharacter(',', '.');
    if (text.isEmpty())
        return std::nullopt;
    if (f == RuleField::Colour)
    {
        const auto hex = text.trimCharactersAtStart("#");
        if (hex.length() != 6 || !hex.containsOnly("0123456789abcdefABCDEF"))
            return std::nullopt;
        return static_cast<double>(hex.getHexValue32());
    }
    if (f == RuleField::Time && text.containsChar(':'))
    {
        const auto minutes = text.upToFirstOccurrenceOf(":", false, false);
        const auto seconds = text.fromFirstOccurrenceOf(":", false, false);
        if (!minutes.containsOnly("0123456789") || !seconds.containsOnly("0123456789.") || seconds.isEmpty())
            return std::nullopt;
        return minutes.getIntValue() * 60.0 + seconds.getDoubleValue();
    }
    if (!text.containsOnly("0123456789.-"))
        return std::nullopt;
    return text.getDoubleValue();
}

juce::String SmartPlaylistEditor::valueText(RuleField f, double v)
{
    if (f == RuleField::Colour)
        return "#" + juce::String::toHexString(static_cast<int>(v)).paddedLeft('0', 6).toUpperCase();
    if (f == RuleField::Time)
        return TrackTableModel::timeText(v);
    return std::abs(v - std::round(v)) < 1e-9 ? juce::String(static_cast<juce::int64>(std::llround(v)))
                                               : juce::String(v);
}

// --- one rule row ------------------------------------------------------------------------------------------------

class SmartPlaylistEditor::Row final : public juce::Component
{
public:
    Row(SmartPlaylistEditor& e, const Rule& rule)
        : editor(e)
    {
        for (int i = 0; i < static_cast<int>(std::size(kAllFields)); ++i)
            field.addItem(fieldLabel(kAllFields[i]), i + 1);
        for (int k = 0; k < 24; ++k)
            key.addItem(TrackTableModel::keyChoiceName((k % 2) * 12 + k / 2), (k % 2) * 12 + k / 2 + 1); // C, Cm, Db, ...
        for (auto* c : {&field, &op, &key})
        {
            style(*c);
            addAndMakeVisible(*c);
        }
        for (auto* t : {&value, &to})
        {
            style(*t);
            t->onTextChange = [this] { editor.changed(); };
            addAndMakeVisible(*t);
        }
        for (auto* l : {&andLabel, &unitLabel})
        {
            l->setColour(juce::Label::textColourId, kDim);
            l->setFont(juce::FontOptions(13.0f));
            addAndMakeVisible(*l);
        }
        andLabel.setText("and", juce::dontSendNotification);
        andLabel.setJustificationType(juce::Justification::centred);
        style(remove);
        remove.setButtonText("-");
        remove.setTooltip("Remove this rule");
        addAndMakeVisible(remove);
        remove.onClick = [this]
        {
            if (auto* parent = &editor)
                for (int i = 0; i < parent->numRules(); ++i)
                    if (parent->rows[static_cast<std::size_t>(i)].get() == this)
                    {
                        parent->removeRule(i);
                        return;
                    }
        };

        set(rule);
        field.onChange = [this]
        {
            fillOperators(operatorsList().front());
            updateValueWidgets();
            editor.changed();
        };
        op.onChange = [this]
        {
            updateValueWidgets();
            editor.changed();
        };
        key.onChange = [this] { editor.changed(); };
    }

    void set(const Rule& r)
    {
        for (int i = 0; i < static_cast<int>(std::size(kAllFields)); ++i)
            if (kAllFields[i] == r.field)
                field.setSelectedId(i + 1, juce::dontSendNotification);
        fillOperators(r.op);
        if (library::isTextField(r.field))
            value.setText(juce::String::fromUTF8(r.text.c_str()), false);
        else if (r.field == RuleField::Key)
            key.setSelectedId(static_cast<int>(r.value) + 1, juce::dontSendNotification);
        else
        {
            value.setText(valueText(r.field, r.value), false);
            to.setText(valueText(r.field, r.to), false);
        }
        updateValueWidgets();
    }

    RuleField currentField() const
    {
        const int i = std::clamp(field.getSelectedId() - 1, 0, static_cast<int>(std::size(kAllFields)) - 1);
        return kAllFields[i];
    }

    std::vector<RuleOp> operatorsList() const { return library::operatorsFor(currentField()); }

    RuleOp currentOp() const
    {
        const auto ops = operatorsList();
        const int i = std::clamp(op.getSelectedId() - 1, 0, static_cast<int>(ops.size()) - 1);
        return ops[static_cast<std::size_t>(i)];
    }

    // The rule as typed; nullopt when a needed value is missing or doesn't parse.
    std::optional<Rule> rule() const
    {
        Rule r;
        r.field = currentField();
        r.op = currentOp();
        if (!needsValue(r.op))
            return r;
        if (library::isTextField(r.field))
        {
            r.text = value.getText().trim().toStdString();
            if (r.text.empty())
                return std::nullopt;
            return r;
        }
        if (r.field == RuleField::Key)
        {
            if (key.getSelectedId() < 1)
                return std::nullopt;
            r.value = key.getSelectedId() - 1;
            return r;
        }
        const auto v = parseValue(usesDays(r.op) ? RuleField::PlayCount : r.field, value.getText());
        if (!v)
            return std::nullopt;
        r.value = *v;
        if (r.op == RuleOp::Between)
        {
            const auto t = parseValue(r.field, to.getText());
            if (!t)
                return std::nullopt;
            r.to = *t;
        }
        return r;
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(0, 3);
        remove.setBounds(area.removeFromRight(30));
        area.removeFromRight(8);
        field.setBounds(area.removeFromLeft(150));
        area.removeFromLeft(6);
        op.setBounds(area.removeFromLeft(160));
        area.removeFromLeft(6);
        const auto f = currentField();
        const auto o = currentOp();
        if (f == RuleField::Key)
            key.setBounds(area.removeFromLeft(100));
        else if (o == RuleOp::Between)
        {
            const int w = std::max(60, (area.getWidth() - 40) / 2);
            value.setBounds(area.removeFromLeft(w));
            andLabel.setBounds(area.removeFromLeft(40));
            to.setBounds(area.removeFromLeft(w));
        }
        else
        {
            const bool unit = unitLabel.getText().isNotEmpty();
            value.setBounds(area.removeFromLeft(unit ? std::min(120, area.getWidth() - 60) : area.getWidth()));
            area.removeFromLeft(6);
            unitLabel.setBounds(area);
        }
    }

private:
    void fillOperators(RuleOp selected)
    {
        op.clear(juce::dontSendNotification);
        const auto ops = operatorsList();
        for (int i = 0; i < static_cast<int>(ops.size()); ++i)
        {
            op.addItem(opLabel(ops[static_cast<std::size_t>(i)]), i + 1);
            if (ops[static_cast<std::size_t>(i)] == selected)
                op.setSelectedId(i + 1, juce::dontSendNotification);
        }
        if (op.getSelectedId() == 0)
            op.setSelectedId(1, juce::dontSendNotification);
    }

    void updateValueWidgets()
    {
        const auto f = currentField();
        const auto o = currentOp();
        const bool needs = needsValue(o);
        key.setVisible(needs && f == RuleField::Key);
        value.setVisible(needs && f != RuleField::Key);
        to.setVisible(needs && o == RuleOp::Between);
        andLabel.setVisible(needs && o == RuleOp::Between);
        juce::String unit;
        juce::String hint;
        if (usesDays(o))
            unit = "days";
        else if (f == RuleField::Time)
            hint = "m:ss";
        else if (f == RuleField::Colour)
            hint = "#RRGGBB";
        else if (f == RuleField::Rating)
            unit = "stars";
        unitLabel.setText(o == RuleOp::Between ? juce::String() : unit, juce::dontSendNotification);
        unitLabel.setVisible(needs);
        value.setTextToShowWhenEmpty(hint, kDim);
        to.setTextToShowWhenEmpty(hint, kDim);
        resized();
    }

    SmartPlaylistEditor& editor;
    juce::ComboBox field, op, key;
    juce::TextEditor value, to;
    juce::Label andLabel, unitLabel;
    juce::TextButton remove;
};

// --- the editor --------------------------------------------------------------------------------------------------

SmartPlaylistEditor::SmartPlaylistEditor(const juce::String& n, const library::SmartRules& initial,
                                         std::function<int(const library::SmartRules&)> counter)
    : countTracks(std::move(counter))
{
    nameLabel.setText("Name", juce::dontSendNotification);
    matchLabel.setText("Tracks that match", juce::dontSendNotification);
    for (auto* l : {&nameLabel, &matchLabel, &count})
    {
        l->setColour(juce::Label::textColourId, kText);
        l->setFont(juce::FontOptions(14.0f));
        addAndMakeVisible(*l);
    }
    count.setColour(juce::Label::textColourId, kDim);
    style(nameEditor);
    nameEditor.setText(n, false);
    nameEditor.onTextChange = [this] { changed(); };
    addAndMakeVisible(nameEditor);
    match.addItem("all of these rules", 1);
    match.addItem("any of these rules", 2);
    match.setSelectedId(initial.matchAll ? 1 : 2, juce::dontSendNotification);
    match.onChange = [this] { changed(); };
    style(match);
    addAndMakeVisible(match);
    for (auto* b : {&addButton, &saveButton, &cancelButton})
    {
        style(*b);
        addAndMakeVisible(*b);
    }
    saveButton.setColour(juce::TextButton::buttonColourId, kAccent.withAlpha(0.7f));
    addButton.onClick = [this] { addRule(); };
    saveButton.onClick = [this] { save(); };
    cancelButton.onClick = [this]
    {
        if (onCancel)
            onCancel();
    };
    viewport.setViewedComponent(&rowHolder, false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    for (const auto& r : initial.rules)
        addRule(r);
    if (initial.rules.empty())
        addRule();
    setSize(720, 420);
    changed();
}

SmartPlaylistEditor::~SmartPlaylistEditor() = default;

juce::String SmartPlaylistEditor::name() const
{
    return nameEditor.getText().trim();
}

library::SmartRules SmartPlaylistEditor::rules() const
{
    library::SmartRules out;
    out.matchAll = match.getSelectedId() != 2;
    for (const auto& row : rows)
        if (const auto r = row->rule())
            out.rules.push_back(*r);
    return out;
}

bool SmartPlaylistEditor::canSave() const
{
    if (name().isEmpty())
        return false;
    for (const auto& row : rows)
        if (!row->rule())
            return false;
    return true;
}

void SmartPlaylistEditor::addRule(const library::Rule& rule)
{
    rows.push_back(std::make_unique<Row>(*this, rule));
    rowHolder.addAndMakeVisible(*rows.back());
    layoutRows();
    changed();
}

void SmartPlaylistEditor::removeRule(int index)
{
    if (index < 0 || index >= numRules())
        return;
    // Deleting the row from inside its own button's click: let the click finish first.
    auto row = std::move(rows[static_cast<std::size_t>(index)]);
    rows.erase(rows.begin() + index);
    rowHolder.removeChildComponent(row.get());
    juce::MessageManager::callAsync([gone = std::shared_ptr<Row>(row.release())] {});
    layoutRows();
    changed();
}

void SmartPlaylistEditor::setMatchAll(bool all)
{
    match.setSelectedId(all ? 1 : 2, juce::sendNotificationSync);
}

void SmartPlaylistEditor::changed()
{
    const auto r = rules();
    bool complete = true;
    for (const auto& row : rows)
        complete &= row->rule().has_value();
    const int n = countTracks ? countTracks(r) : -1;
    juce::String text;
    if (n < 0)
        text = "These rules can't be used";
    else
        text << n << (n == 1 ? " track matches" : " tracks match");
    if (!complete)
        text << " (fill in or remove the empty rule)";
    count.setText(text, juce::dontSendNotification);
    saveButton.setEnabled(canSave());
}

void SmartPlaylistEditor::save()
{
    if (canSave() && onSave)
        onSave(name(), rules());
}

void SmartPlaylistEditor::layoutRows()
{
    const int width = std::max(100, viewport.getWidth() - viewport.getScrollBarThickness());
    rowHolder.setSize(width, numRules() * kRowHeight);
    for (int i = 0; i < numRules(); ++i)
        rows[static_cast<std::size_t>(i)]->setBounds(0, i * kRowHeight, width, kRowHeight);
}

void SmartPlaylistEditor::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto top = area.removeFromTop(28);
    nameLabel.setBounds(top.removeFromLeft(60));
    nameEditor.setBounds(top);
    area.removeFromTop(10);
    auto matchRow = area.removeFromTop(28);
    matchLabel.setBounds(matchRow.removeFromLeft(140));
    match.setBounds(matchRow.removeFromLeft(200));
    addButton.setBounds(matchRow.removeFromRight(110));
    area.removeFromTop(10);
    auto bottom = area.removeFromBottom(30);
    cancelButton.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(8);
    saveButton.setBounds(bottom.removeFromRight(90));
    count.setBounds(bottom);
    area.removeFromBottom(10);
    viewport.setBounds(area);
    layoutRows();
}

void SmartPlaylistEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
}

// --- dialog ------------------------------------------------------------------------------------------------------

void showSmartPlaylistEditor(juce::Component* parent, const juce::String& title, const juce::String& name,
                             const library::SmartRules& rules, std::function<int(const library::SmartRules&)> counter,
                             std::function<void(const juce::String&, const library::SmartRules&)> onSave)
{
    auto editor = std::make_unique<SmartPlaylistEditor>(name, rules, std::move(counter));
    auto* raw = editor.get();
    const auto close = [raw]
    {
        if (auto* window = raw->findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    };
    raw->onCancel = close;
    raw->onSave = [close, onSave = std::move(onSave)](const juce::String& n, const library::SmartRules& r)
    {
        onSave(n, r);
        close();
    };
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(editor.release());
    options.dialogTitle = title;
    options.dialogBackgroundColour = kBackground;
    options.componentToCentreAround = parent;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

} // namespace devildj::ui
