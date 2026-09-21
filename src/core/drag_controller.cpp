#include "drag_controller.h"
#include "logging.h"

#include <eui/dsl.h>

namespace core::drag {

void DragController::registerDraggable(const std::string& elementId,
                                        DragStartHandler onStart,
                                        DragMoveHandler onMove,
                                        DragEndHandler onEnd) {
    draggables_[elementId] = {std::move(onStart), std::move(onMove), std::move(onEnd)};
}

void DragController::registerDropTarget(const std::string& targetId,
                                         eui::Rect bounds,
                                         DropPredicate accept,
                                         DropHandler onEnter,
                                         DropHandler onLeave,
                                         DropHandler onDrop) {
    DropTarget t;
    t.bounds = bounds;
    t.accept = std::move(accept);
    t.onEnter = std::move(onEnter);
    t.onLeave = std::move(onLeave);
    t.onDrop = std::move(onDrop);
    dropTargets_[targetId] = std::move(t);
}

std::string DragController::hitTestDraggable(eui::Vec2 pos) const {
    // 简化：返回最近注册的
    if (draggables_.empty()) return "";
    return draggables_.begin()->first;
}

std::string DragController::hitTestDropTarget(eui::Vec2 pos) const {
    for (const auto& [id, target] : dropTargets_) {
        if (pos.x >= target.bounds.x && pos.x <= target.bounds.x + target.bounds.width &&
            pos.y >= target.bounds.y && pos.y <= target.bounds.y + target.bounds.height) {
            return id;
        }
    }
    return "";
}

void DragController::onPointerDown(eui::Vec2 pos) {
    auto id = hitTestDraggable(pos);
    if (id.empty()) return;
    auto it = draggables_.find(id);
    if (it == draggables_.end()) return;

    activeDraggableId_ = id;
    current_.dragId = id;
    current_.startPosition = pos;
    current_.currentPosition = pos;
    current_.payload = nullptr;

    if (it->second.onStart) it->second.onStart(current_);
    dragging_ = true;
    LOG_DEBUG("drag start: " + id);
}

void DragController::onPointerMove(eui::Vec2 pos) {
    if (!dragging_) return;
    current_.currentPosition = pos;

    auto it = draggables_.find(activeDraggableId_);
    if (it != draggables_.end() && it->second.onMove) {
        it->second.onMove(current_);
    }

    auto targetId = hitTestDropTarget(pos);
    for (auto& [id, target] : dropTargets_) {
        bool shouldEnter = (id == targetId);
        if (shouldEnter && !target.entered) {
            target.entered = true;
            if (target.onEnter) target.onEnter(current_);
        } else if (!shouldEnter && target.entered) {
            target.entered = false;
            if (target.onLeave) target.onLeave(current_);
        }
    }
}

void DragController::onPointerUp(eui::Vec2 pos) {
    if (!dragging_) return;
    current_.currentPosition = pos;

    auto targetId = hitTestDropTarget(pos);
    bool dropped = false;
    if (!targetId.empty()) {
        auto& target = dropTargets_[targetId];
        if (target.accept && target.accept(current_)) {
            if (target.onDrop) target.onDrop(current_);
            dropped = true;
        }
    }

    auto it = draggables_.find(activeDraggableId_);
    if (it != draggables_.end() && it->second.onEnd) {
        it->second.onEnd(current_, dropped);
    }

    dragging_ = false;
    activeDraggableId_.clear();
    LOG_DEBUG("drag end, dropped=" + std::to_string(dropped));
}

} // namespace core::drag