#pragma once

#include "Project.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace daw
{

/** Owns native plugin editor windows. Message thread only; prune before engine garbage
    collection, and closeAll before replacing the document or destroying the project. */
class PluginWindows
{
public:
    explicit PluginWindows (Project&);
    ~PluginWindows();
    void open (juce::ValueTree device);
    void prune();
    void closeAll();

private:
    struct Window;
    Project& project;
    std::vector<std::unique_ptr<Window>> windows;
};

} // namespace daw
