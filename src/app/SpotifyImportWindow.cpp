#include "app/SpotifyImportWindow.h"

namespace devildj::app
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kAccent(0xffe0483c);

void style(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::textColourOffId, kText);
}

juce::String trackText(const RemoteTrack& t)
{
    juce::StringArray a;
    for (const auto& s : t.artists)
        a.add(juce::String::fromUTF8(s.c_str()));
    return a.joinIntoString(", ") + " - " + juce::String::fromUTF8(t.title.c_str());
}
} // namespace

SpotifyImportWindow::SpotifyImportWindow(Source s)
    : source(std::move(s))
{
    list.setColour(juce::ListBox::backgroundColourId, kPanel);
    list.setRowHeight(26);
    list.setMultipleSelectionEnabled(false);
    addAndMakeVisible(list);
    for (auto* b : {&importButton, &createButton, &copyButton, &saveButton, &backButton})
    {
        style(*b);
        addChildComponent(*b);
    }
    importButton.setVisible(true);
    importButton.setTooltip("Read the ticked playlists and match their tracks to your library");
    importButton.onClick = [this] { importChosen(); };
    createButton.onClick = [this]
    {
        const int n = createPlaylists();
        if (onDone)
            onDone(n);
    };
    copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(allMissing()); setStatus("The missing tracks are on the clipboard."); };
    saveButton.onClick = [this]
    {
        if (source.saveText)
            source.saveText(allMissing());
    };
    backButton.onClick = [this]
    {
        reviewing = false;
        for (auto* c : {static_cast<juce::Component*>(&createButton), static_cast<juce::Component*>(&copyButton),
                        static_cast<juce::Component*>(&saveButton), static_cast<juce::Component*>(&backButton),
                        static_cast<juce::Component*>(&reviewView), static_cast<juce::Component*>(&missingText),
                        static_cast<juce::Component*>(&summary)})
            c->setVisible(false);
        list.setVisible(true);
        importButton.setVisible(true);
        resized();
    };
    summary.setColour(juce::Label::textColourId, kText);
    addChildComponent(summary);
    reviewView.setViewedComponent(&reviewContent, false);
    reviewView.setScrollBarsShown(true, false);
    addChildComponent(reviewView);
    missingText.setMultiLine(true);
    missingText.setReadOnly(true);
    missingText.setColour(juce::TextEditor::backgroundColourId, kPanel);
    missingText.setColour(juce::TextEditor::textColourId, kDim);
    addChildComponent(missingText);
    setSize(820, 560);
}

SpotifyImportWindow::~SpotifyImportWindow()
{
    *alive = false; // a worker finishing later finds nobody here
}

void SpotifyImportWindow::setStatus(const juce::String& s)
{
    statusText = s;
    repaint();
}

void SpotifyImportWindow::start()
{
    if (!source.playlists)
        return;
    working = true;
    setStatus("Reading your Spotify playlists...");
    auto fetch = source.playlists;
    const auto weak = alive;
    juce::Thread::launch([this, fetch, weak]
                         {
                             auto r = fetch();
                             juce::MessageManager::callAsync(
                                 [this, weak, r = std::move(r)]() mutable
                                 {
                                     if (!*weak)
                                         return;
                                     working = false;
                                     if (!r)
                                     {
                                         setStatus("Couldn't read your playlists: " + juce::String(r.error()));
                                         return;
                                     }
                                     lists = std::move(r).value();
                                     chosen.assign(lists.size(), false);
                                     list.updateContent();
                                     setStatus(juce::String(static_cast<int>(lists.size())) +
                                               " playlists. Tick the ones to import (Space), then IMPORT.");
                                 });
                         });
}

void SpotifyImportWindow::importChosen()
{
    std::vector<RemotePlaylist> picked;
    for (std::size_t i = 0; i < lists.size(); ++i)
        if (chosen[i])
            picked.push_back(lists[i]);
    if (picked.empty() || working || !source.tracks || !source.library)
    {
        setStatus(picked.empty() ? "Tick at least one playlist first." : statusText);
        return;
    }
    working = true;
    setStatus("Reading " + juce::String(static_cast<int>(picked.size())) + " playlists and matching them to your library...");
    auto src = source;
    const auto weak = alive;
    juce::Thread::launch(
        [this, src, picked, weak]
        {
            const auto library = src.library();
            const PlaylistMatcher matcher(library);
            std::vector<ImportPlan> out;
            juce::String problem;
            for (const auto& p : picked)
            {
                auto t = src.tracks(p.id);
                if (!t)
                {
                    problem = juce::String(p.name) + ": " + juce::String(t.error());
                    continue;
                }
                out.push_back(planImport(p.name, std::move(t).value(), matcher));
            }
            std::map<std::int64_t, juce::String> n;
            for (const auto& l : library)
                n[l.id] = juce::String::fromUTF8(l.artist.c_str()) + " - " + juce::String::fromUTF8(l.title.c_str());
            juce::MessageManager::callAsync(
                [this, weak, out = std::move(out), n = std::move(n), problem]() mutable
                {
                    if (!*weak)
                        return;
                    working = false;
                    results = std::move(out);
                    names = std::move(n);
                    showReview();
                    if (problem.isNotEmpty())
                        setStatus("Some couldn't be read - " + problem);
                });
        });
}

void SpotifyImportWindow::showReview()
{
    reviewing = true;
    list.setVisible(false);
    importButton.setVisible(false);
    for (auto* c : {static_cast<juce::Component*>(&createButton), static_cast<juce::Component*>(&copyButton),
                    static_cast<juce::Component*>(&saveButton), static_cast<juce::Component*>(&backButton),
                    static_cast<juce::Component*>(&reviewView), static_cast<juce::Component*>(&missingText),
                    static_cast<juce::Component*>(&summary)})
        c->setVisible(true);
    juce::String counts;
    int ambiguous = 0;
    for (const auto& p : results)
    {
        counts << juce::String::fromUTF8(p.name.c_str()) << ": " << p.matched() << " found, " << p.ambiguous() << " to choose, "
               << p.missing() << " not in your library.   ";
        ambiguous += p.ambiguous();
    }
    summary.setText(counts, juce::dontSendNotification);
    // A row per ambiguous track: its candidates (best first) or skip.
    choices.clear();
    choiceLabels.clear();
    choiceAt.clear();
    reviewContent.removeAllChildren();
    for (int pi = 0; pi < static_cast<int>(results.size()); ++pi)
    {
        auto& p = results[static_cast<std::size_t>(pi)];
        for (int ti = 0; ti < static_cast<int>(p.matches.size()); ++ti)
        {
            const auto& m = p.matches[static_cast<std::size_t>(ti)];
            if (m.kind != TrackMatch::Kind::Ambiguous)
                continue;
            auto label = std::make_unique<juce::Label>();
            label->setText(trackText(p.tracks[static_cast<std::size_t>(ti)]), juce::dontSendNotification);
            label->setColour(juce::Label::textColourId, kText);
            auto box = std::make_unique<juce::ComboBox>();
            for (std::size_t c = 0; c < m.candidates.size(); ++c)
                box->addItem(names[m.candidates[c].first] + "   (" + juce::String(juce::roundToInt(m.candidates[c].second * 100.0)) + " %)",
                             static_cast<int>(c) + 1);
            box->addItem("(leave it out)", 1000);
            box->setSelectedId(1, juce::dontSendNotification);
            box->onChange = [this, pi, ti, b = box.get()]
            {
                auto& plan = results[static_cast<std::size_t>(pi)];
                const auto& cands = plan.matches[static_cast<std::size_t>(ti)].candidates;
                const int id = b->getSelectedId();
                plan.chosen[static_cast<std::size_t>(ti)] = id == 1000 || id < 1 || id > static_cast<int>(cands.size())
                                                                ? 0
                                                                : cands[static_cast<std::size_t>(id - 1)].first;
            };
            reviewContent.addAndMakeVisible(*label);
            reviewContent.addAndMakeVisible(*box);
            choiceLabels.push_back(std::move(label));
            choices.push_back(std::move(box));
            choiceAt.emplace_back(pi, ti);
        }
    }
    missingText.setText(allMissing().isEmpty() ? juce::String("Every track was found in your library.") : allMissing(), false);
    setStatus(ambiguous > 0 ? "Choose the ones that are unclear, then CREATE PLAYLISTS." : "Ready: CREATE PLAYLISTS.");
    resized();
}

juce::ComboBox* SpotifyImportWindow::choiceBox(int index)
{
    return index >= 0 && index < static_cast<int>(choices.size()) ? choices[static_cast<std::size_t>(index)].get() : nullptr;
}

juce::String SpotifyImportWindow::allMissing() const
{
    juce::String out;
    for (const auto& p : results)
        if (const auto m = missingReport(p); m.isNotEmpty())
            out << "# " << juce::String::fromUTF8(p.name.c_str()) << "\n" << m;
    return out;
}

int SpotifyImportWindow::createPlaylists()
{
    if (!source.create)
        return 0;
    int made = 0;
    juce::String problems;
    for (const auto& p : results)
    {
        const auto r = source.create(p);
        if (r)
            ++made;
        else
            problems << juce::String::fromUTF8(p.name.c_str()) << ": " << r.error() << "  ";
    }
    setStatus(juce::String(made) + " playlists made in your library." + (problems.isNotEmpty() ? " " + problems : juce::String()));
    return made;
}

int SpotifyImportWindow::getNumRows()
{
    return static_cast<int>(lists.size());
}

void SpotifyImportWindow::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(lists.size()))
        return;
    g.fillAll(selected ? kAccent.withAlpha(0.3f) : kPanel);
    const auto& p = lists[static_cast<std::size_t>(row)];
    g.setColour(kText);
    g.setFont(juce::FontOptions(14.0f));
    g.drawText(juce::String::fromUTF8(chosen[static_cast<std::size_t>(row)] ? "\xe2\x98\x91  " : "\xe2\x98\x90  ") +
                   juce::String::fromUTF8(p.name.c_str()) + "   (" + juce::String(p.tracks) + " tracks" +
                   (p.owner.empty() ? juce::String() : ", " + juce::String::fromUTF8(p.owner.c_str())) + ")",
               8, 0, width - 16, height, juce::Justification::centredLeft);
}

void SpotifyImportWindow::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < static_cast<int>(chosen.size()))
    {
        chosen[static_cast<std::size_t>(row)] = !chosen[static_cast<std::size_t>(row)];
        list.repaintRow(row);
    }
}

void SpotifyImportWindow::returnKeyPressed(int)
{
    importChosen();
}

bool SpotifyImportWindow::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey && !reviewing)
    {
        const int row = list.getSelectedRow();
        if (row >= 0 && row < static_cast<int>(chosen.size()))
        {
            chosen[static_cast<std::size_t>(row)] = !chosen[static_cast<std::size_t>(row)];
            list.repaintRow(row);
        }
        return true;
    }
    return false;
}

void SpotifyImportWindow::resized()
{
    auto area = getLocalBounds().reduced(10);
    area.removeFromTop(26); // the status line
    auto buttons = area.removeFromBottom(34);
    if (!reviewing)
    {
        importButton.setBounds(buttons.removeFromRight(120).reduced(2));
        list.setBounds(area);
        return;
    }
    createButton.setBounds(buttons.removeFromRight(160).reduced(2));
    saveButton.setBounds(buttons.removeFromRight(130).reduced(2));
    copyButton.setBounds(buttons.removeFromRight(130).reduced(2));
    backButton.setBounds(buttons.removeFromLeft(80).reduced(2));
    summary.setBounds(area.removeFromTop(40));
    auto missing = area.removeFromBottom(area.getHeight() / 3);
    missingText.setBounds(missing.reduced(0, 4));
    reviewView.setBounds(area);
    const int w = reviewView.getMaximumVisibleWidth();
    reviewContent.setSize(w, std::max(1, static_cast<int>(choices.size())) * 30);
    for (std::size_t i = 0; i < choices.size(); ++i)
    {
        juce::Rectangle<int> row(0, static_cast<int>(i) * 30, w, 28);
        choiceLabels[i]->setBounds(row.removeFromLeft(w / 2));
        choices[i]->setBounds(row.reduced(2));
    }
}

void SpotifyImportWindow::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(working ? kDim : kText);
    g.setFont(juce::FontOptions(14.0f));
    g.drawText(statusText, getLocalBounds().reduced(10).removeFromTop(24), juce::Justification::centredLeft);
}

} // namespace devildj::app
