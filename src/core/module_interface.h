// src/core/module_interface.h
#pragma once

#include <string>

namespace eui { struct Screen; class Ui; }

namespace core {

enum class PageId {
    Home,
    Markdown,
    Todo,
    ClaudeProxy,
    Settings,
};

inline const char* pageIdToString(PageId id) {
    switch (id) {
        case PageId::Home: return "Home";
        case PageId::Markdown: return "Markdown";
        case PageId::Todo: return "Todo";
        case PageId::ClaudeProxy: return "ClaudeProxy";
        case PageId::Settings: return "Settings";
    }
    return "?";
}

class Module {
public:
    virtual ~Module() = default;

    virtual PageId pageId() const = 0;
    virtual std::string displayName() const = 0;
    virtual std::string iconGlyph() const = 0;

    virtual void compose(eui::Ui& ui, const eui::Screen& screen) = 0;

    virtual void onStart() {}
    virtual void onStop() {}
};

} // namespace core