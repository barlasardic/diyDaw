#include "MainComponent.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <iostream>

namespace daw
{

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
        if (const auto i = args.indexOf ("--snapshot"); i >= 0 && i + 1 < args.size())
        {
            const auto size = juce::StringArray::fromTokens (args[i + 2], "x", {});
            MainComponent ui;
            if (size.size() == 2)
                ui.setSize (size[0].getIntValue(), size[1].getIntValue());

            juce::Array<juce::File> audioFiles;
            for (int a = i + 3; a < args.size(); ++a)
            {
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

            const auto image = ui.createComponentSnapshot (ui.getLocalBounds());
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
