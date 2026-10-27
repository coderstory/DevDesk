#include "release_view_state.h"

#include <utility>

namespace module::release {

void ReleaseViewState::setSelectedTaskId(std::string id) {
    selectedTaskId_ = std::move(id);
}

const std::string& ReleaseViewState::selectedTaskId() const {
    return selectedTaskId_;
}

void ReleaseViewState::setFilterText(std::string text) {
    filterText_ = std::move(text);
}

const std::string& ReleaseViewState::filterText() const { return filterText_; }

void ReleaseViewState::reset() {
    selectedTaskId_.clear();
    filterText_.clear();
}

} // namespace module::release