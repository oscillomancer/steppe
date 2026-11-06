// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The retained node tree (docs/specs/gui.md §2): semantic roles, stack
// layout, input, damage. Custom-drawn views (arrange, piano roll) are one
// node each with their own internal hit testing and virtualization.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "supple/ui/canvas.h"
#include "supple/ui/geometry.h"

namespace supple::ui {

class Root;
struct GlFrame;

enum class Role {
    Group,
    Label,
    Button,
    Toggle,
    Slider,
    TextField,
    List,
    ListItem,
    Tab,
    Menu,
    Custom
};

enum Mod : unsigned { kShift = 1, kCtrl = 2, kAlt = 4, kSuper = 8 };

struct PointerEvent {
    enum class Type { Down, Up, Move, Wheel, Enter, Leave, Cancel } type = Type::Move;
    Point pos;          // window coordinates, design units
    int button = 0;     // 1 left, 2 middle, 3 right
    int clicks = 1;     // 2 for a double click
    unsigned mods = 0;  // Mod bits
    float wheelX = 0, wheelY = 0;
};

struct KeyEvent {
    int key = 0;  // SDL keycode
    unsigned mods = 0;
    bool repeat = false;
};

// Sizing along an axis inside a stack: a fixed size, a share of the space
// left over (weights), or the node's own content size.
struct SizeRule {
    enum class Kind { Fixed, Fraction, Content } kind = Kind::Fraction;
    float value = 1;
    static SizeRule fixed(float v) { return {Kind::Fixed, v}; }
    static SizeRule fraction(float weight = 1) { return {Kind::Fraction, weight}; }
    static SizeRule content() { return {Kind::Content, 0}; }
};

struct Insets {
    float left = 0, top = 0, right = 0, bottom = 0;
    static Insets all(float v) { return {v, v, v, v}; }
    static Insets xy(float x, float y) { return {x, y, x, y}; }
};

struct Layout {
    enum class Dir { None, Row, Column } dir = Dir::None;  // None: children keep their bounds
    enum class Align { Start, Center, End, Stretch } align = Align::Stretch;  // cross axis
    SizeRule width = SizeRule::fraction(), height = SizeRule::fraction();
    float minWidth = 0, minHeight = 0, maxWidth = 1e9f, maxHeight = 1e9f;
    float gap = 0;
    Insets padding;
};

class Node {
  public:
    explicit Node(std::string id = {}, Role role = Role::Group) : id_(std::move(id)), role_(role) {}
    virtual ~Node();
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // --- tree
    template <class T, class... Args>
    T& emplace(Args&&... args) {
        return static_cast<T&>(add(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    Node& add(std::unique_ptr<Node> child);
    // Removed nodes stay alive until the current input dispatch completes.
    std::shared_ptr<Node> remove(Node* child);
    void clear();
    Node* parent() const { return parent_; }
    const std::vector<std::shared_ptr<Node>>& children() const { return children_; }
    // Depth-first search by id (this node included).
    Node* find(std::string_view id);
    Root* root() const;

    const std::string& id() const { return id_; }
    Role role() const { return role_; }

    // --- layout
    Layout layout;
    const Rect& bounds() const { return bounds_; }
    // Content size for SizeRule::content(); default: the stack of children.
    virtual Size measure(const Canvas& canvas, Size available);
    // Sets bounds and, for stacks, positions the children.
    virtual void arrange(const Canvas& canvas, const Rect& bounds);
    // Asks the root to lay out again (size or content changed).
    void relayout();

    // --- painting (window coordinates); children paint after their parent
    virtual void paint(Canvas& canvas) { (void)canvas; }
    void invalidate();                  // own bounds
    void invalidate(const Rect& area);  // part of them
    bool clipsChildren = false;         // scroll views, lists
    // Bulk GL content (gl.h): paintGl runs after paint() with NanoVG flushed
    // and the GL scissor set to the region being painted.
    bool drawsGl = false;
    virtual void paintGl(const GlFrame& frame) { (void)frame; }

    // --- state and input
    bool visible = true;
    bool enabled = true;
    bool focusable = false;
    bool interactive = false;  // receives pointer events (else they pass through)
    virtual Node* hitTest(Point p);
    virtual bool onPointer(const PointerEvent& e) {
        (void)e;
        return false;
    }
    virtual bool onKey(const KeyEvent& e) {
        (void)e;
        return false;
    }
    // A key released, to the focused node and its parents (a held key that
    // is a modifier of a gesture, such as Space for panning).
    virtual bool onKeyUp(const KeyEvent& e) {
        (void)e;
        return false;
    }
    // IME composition (preedit): the text being composed and the cursor in it
    // (bytes); empty text ends composition.
    virtual bool onComposition(std::string_view text, int cursor) {
        (void)text;
        (void)cursor;
        return false;
    }
    virtual bool onText(std::string_view text) {
        (void)text;
        return false;
    }
    virtual void onFocus(bool focused) { (void)focused; }
    virtual void onHover(bool hovered) { (void)hovered; }
    // A continuous control whose drag only needs the latest pointer position:
    // while it holds the capture, window motion reaches it once per frame
    // (Root::handle) rather than once per motion event.
    virtual bool coalescesDrag() const { return false; }
    bool hovered() const;
    bool focused() const;
    bool visibleInTree() const;

    // For tests, the command log and (later) accessibility.
    virtual std::string valueText() const { return {}; }

  protected:
    void setBounds(const Rect& r) { bounds_ = r; }

  private:
    friend class Root;
    void attach(Root* root);
    std::string id_;
    Role role_;
    Rect bounds_;
    Node* parent_ = nullptr;
    Root* root_ = nullptr;  // set on the tree's top node only
    std::vector<std::shared_ptr<Node>> children_;
};

}  // namespace supple::ui
