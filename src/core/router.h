// src/core/router.h
#pragma once

#include <vector>

#include "module_interface.h"

namespace core {

class Router {
public:
    Router() = default;

    void registerModule(Module* module);

    void navigateTo(PageId page);
    PageId currentPage() const;

    void compose(eui::Ui& ui, const eui::Screen& screen);

    void startAll();
    void stopAll();

private:
    std::vector<Module*> modules_;
    PageId current_ = PageId::Home;
};

} // namespace core