#include "MainComponent.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <iostream>

namespace daw
{

namespace
{
    // Restrict command-line discovery to explicitly supplied directories. LV2 can
    // return previously cached bundles, so filter its search results as well.
    template <typename Format>
    class SnapshotPluginFormat : public Format
    {
        juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }

        juce::StringArray searchPathsForPlugins (const juce::FileSearchPath& paths, bool recursive, bool allowAsync) override
        {
            juce::StringArray result;
            for (const auto& candidate : Format::searchPathsForPlugins (paths, recursive, allowAsync))
                for (int i = 0; i < paths.getNumPaths(); ++i)
                    if (const juce::File file (candidate); file == paths[i] || file.isAChildOf (paths[i]))
                    {
                        result.addIfNotAlreadyThere (candidate);
                        break;
                    }
            return result;
        }
    };

    void addSnapshotFormats (juce::AudioPluginFormatManager& formats)
    {
       #if JUCE_PLUGINHOST_VST3
        formats.addFormat (std::make_unique<SnapshotPluginFormat<juce::VST3PluginFormat>>());
       #endif
       #if JUCE_PLUGINHOST_LV2
        formats.addFormat (std::make_unique<SnapshotPluginFormat<juce::LV2PluginFormat>>());
       #endif
    }

    void scanSnapshotPlugins (MainComponent& ui, const juce::StringArray& paths)
    {
        if (paths.isEmpty())
            return;
        juce::FileSearchPath searchPaths;
        for (const auto& path : paths)
            searchPaths.add (juce::File::getCurrentWorkingDirectory().getChildFile (path.unquoted()));
        // Keep the scanning format instances: LV2 resolves plugin URIs through their
        // loaded bundles. A copied catalogue alone cannot resolve custom LV2 folders.
        auto& host = ui.getPluginHost();
        const auto savedCallback = std::move (host.onScanFinished);
        host.getKnownPlugins().clear();
        host.setExtraSearchPaths (searchPaths);
        host.scanNow();
        host.onScanFinished = savedCallback; // snapshots must not overwrite the user's catalogue
    }
}

class DiyDawApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // `diyDaw --snapshot out.png [WxH] [audio files... | project.diydaw]` renders the UI
        // offscreen and exits (for visual checks). Audio file i is placed on track i, staggered
        // by one bar; a project file is opened instead (its samples decoded synchronously).
        auto args = juce::StringArray::fromTokens (commandLine, true);
        // `--dialog export|pianoroll|synth|devices|master|plugins` snapshots that panel instead of the main window:
        // the Export Audio dialog, or the detail view showing the first MIDI clip / the first MIDI
        // track's device chain (synth and devices are the same view).
        juce::StringArray pluginPaths;
        for (int p = args.indexOf ("--plugin-path"); p >= 0; p = args.indexOf ("--plugin-path"))
        {
            if (p + 1 < args.size()) pluginPaths.add (args[p + 1]);
            args.removeRange (p, juce::jmin (2, args.size() - p));
        }
        juce::String dialogName;
        if (const auto d = args.indexOf ("--dialog"); d >= 0)
        {
            dialogName = args[d + 1];
            args.removeRange (d, 2);
        }

        // `diyDaw --export out.wav project.diydaw` bounces the whole project (24 bit, 44.1 kHz).
        if (const auto i = args.indexOf ("--export"); i >= 0 && i + 2 < args.size())
        {
            const auto cwd = juce::File::getCurrentWorkingDirectory();
            MainComponent ui (addSnapshotFormats);
            scanSnapshotPlugins (ui, pluginPaths);
            auto result = ui.openProjectSynchronously (cwd.getChildFile (args[i + 2].unquoted()));

            if (result.wasOk())
            {
                ExportSettings settings;
                settings.endBeat = ProjectRenderer::getProjectEndBeat (ui.getProject());
                settings.autoTail = true;
                ProjectRenderer renderer (ui.getProject(), settings);
                result = renderer.renderToFile (cwd.getChildFile (args[i + 1].unquoted()));
            }

            if (result.failed())
                std::cerr << result.getErrorMessage() << std::endl;

            setApplicationReturnValue (result.wasOk() ? 0 : 1);
            quit();
            return;
        }

        if (const auto i = args.indexOf ("--snapshot"); i >= 0 && i + 1 < args.size())
        {
            const auto size = juce::StringArray::fromTokens (args[i + 2], "x", {});
            MainComponent ui (addSnapshotFormats);
            scanSnapshotPlugins (ui, pluginPaths);
            if (size.size() == 2)
                ui.setSize (size[0].getIntValue(), size[1].getIntValue());

            juce::Array<juce::File> audioFiles;
            for (int a = i + 3; a < args.size(); ++a)
            {
                if (args[a] == "--show-automation")
                    continue;
                if (args[a] == "--piano-rows" || args[a] == "--piano-keys")
                {
                    ++a; // skip the value too
                    continue;
                }
                const auto f = juce::File::getCurrentWorkingDirectory().getChildFile (args[a].unquoted());
                if (f.hasFileExtension (Project::fileExtension))
                {
                    if (const auto r = ui.openProjectSynchronously (f); r.failed())
                        std::cerr << r.getErrorMessage() << std::endl;
                }
                else
                    audioFiles.add (f);
            }
            ui.loadFilesSynchronously (audioFiles, 4.0);
            if (args.contains ("--show-automation"))
            {
                // The arrangement may be nested in the main view; keep this flag local
                // to the offscreen snapshot path.
                const auto expandAutomation = [] (auto&& visit, juce::Component& component) -> void
                {
                    if (auto* arrangement = dynamic_cast<ArrangementView*> (&component))
                        arrangement->showAutomationForTracksWithAutomation();
                    for (auto* child : component.getChildren())
                        visit (visit, *child);
                };
                expandAutomation (expandAutomation, ui);
            }

            std::unique_ptr<juce::Component> dialog;
            if (dialogName == "plugins")
                dialog = ui.createPluginManager();
            else if (dialogName == "export")
                dialog = std::make_unique<ExportDialog> (ui.getProject(), 44100.0);
            else if (dialogName == "master")
            {
                auto detail = std::make_unique<DetailView> (ui.getProject());
                detail->setBounds (ui.getLocalBounds());
                detail->showMaster();
                detail->refresh();
                dialog = std::move (detail);
            }
            else if (dialogName == "pianoroll" || dialogName == "synth" || dialogName == "devices")
            {
                auto detail = std::make_unique<DetailView> (ui.getProject());
                detail->setBounds (ui.getLocalBounds());

                for (const auto& track : ui.getProject().getState())
                {
                    if (! Project::isMidiTrack (track))
                        continue;

                    detail->showTrack (track);
                    for (const auto& clip : track)
                        if (Project::isMidiClip (clip))
                        {
                            detail->showClip (clip);
                            break;
                        }
                    break;
                }

                detail->setTab (dialogName == "pianoroll" ? DetailView::Tab::clip : DetailView::Tab::device);

                // `--piano-rows <px>` / `--piano-keys <px>`: piano roll row height / keyboard width.
                if (const auto r = args.indexOf ("--piano-rows"); r >= 0)
                    detail->getPianoRoll().setRowHeight (args[r + 1].getFloatValue());
                if (const auto k = args.indexOf ("--piano-keys"); k >= 0)
                    detail->getPianoRoll().setKeyboardWidth (args[k + 1].getIntValue());

                detail->refresh();
                dialog = std::move (detail);
            }

            auto& target = dialog != nullptr ? *dialog : static_cast<juce::Component&> (ui);
            const auto image = target.createComponentSnapshot (target.getLocalBounds());
            juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 1].unquoted()));
            file.deleteFile();
            juce::FileOutputStream stream (file);
            const auto ok = stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow> (getApplicationName());

        // `diyDaw song.diydaw` opens a project.
        for (const auto& arg : args)
            if (const auto f = juce::File::getCurrentWorkingDirectory().getChildFile (arg.unquoted());
                f.hasFileExtension (Project::fileExtension) && f.existsAsFile())
                if (auto* ui = mainWindow->getMainComponent())
                    ui->openProject (f);
    }

    void shutdown() override { mainWindow = nullptr; }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            if (auto* ui = mainWindow->getMainComponent())
            {
                ui->requestQuit ([] { JUCEApplication::getInstance()->quit(); });
                return;
            }

        quit();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, colours::background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (700, 420, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }

        MainComponent* getMainComponent() const { return dynamic_cast<MainComponent*> (getContentComponent()); }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace daw

START_JUCE_APPLICATION (daw::DiyDawApplication)
