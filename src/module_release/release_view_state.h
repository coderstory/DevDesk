// 上线域视图状态（骨架）：跨帧保留选择/筛选，不落库。
#pragma once

#include <string>

namespace module::release {

class ReleaseViewState {
public:
    void setSelectedTaskId(std::string id);
    const std::string& selectedTaskId() const;

    void setFilterText(std::string text);
    const std::string& filterText() const;

    void reset();

private:
    std::string selectedTaskId_;
    std::string filterText_;
};

} // namespace module::release