// src/core/drag_controller.h
// 自实现拖拽框架（EUI-NEO 无 drop-target API）
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace eui { struct Vec2; struct Rect; }

namespace core::drag {

struct DragContext {
    std::string dragId;
    void* payload = nullptr;
    eui::Vec2 startPosition;
    eui::Vec2 currentPosition;
};

using DragStartHandler = std::function<void(const DragContext&)>;
using DragMoveHandler = std::function<void(const DragContext&)>;
using DragEndHandler = std::function<void(const DragContext&, bool droppedOnValidTarget)>;
using DropPredicate = std::function<bool(const DragContext&)>;
using DropHandler = std::function<void(const DragContext&)>;

class DragController {
public:
    DragController() = default;

    void registerDraggable(const std::string& elementId,
                            DragStartHandler onStart,
                            DragMoveHandler onMove,
                            DragEndHandler onEnd);

    void registerDropTarget(const std::string& targetId,
                             eui::Rect bounds,
                             DropPredicate acceptPredicate,
                             DropHandler onEnter,
                             DropHandler onLeave,
                             DropHandler onDrop);

    void onPointerDown(eui::Vec2 pos);
    void onPointerMove(eui::Vec2 pos);
    void onPointerUp(eui::Vec2 pos);

    bool isDragging() const { return dragging_; }
    const DragContext& current() const { return current_; }

private:
    struct Draggable {
        DragStartHandler onStart;
        DragMoveHandler onMove;
        DragEndHandler onEnd;
    };
    struct DropTarget {
        eui::Rect bounds;
        DropPredicate accept;
        DropHandler onEnter;
        DropHandler onLeave;
        DropHandler onDrop;
        bool entered = false;
    };

    std::unordered_map<std::string, Draggable> draggables_;
    std::unordered_map<std::string, DropTarget> dropTargets_;
    DragContext current_;
    bool dragging_ = false;
    std::string activeDraggableId_;

    std::string hitTestDraggable(eui::Vec2 pos) const;
    std::string hitTestDropTarget(eui::Vec2 pos) const;
};

} // namespace core::drag