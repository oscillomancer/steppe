// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Form controls for dialogs (gui.md §2, §4.8): a dropdown, an editable
// dropdown (a text field with suggestions), a checkbox, a foldable section
// and a checklist. Values set by the owner (set…) never call back; user
// edits do. Each exposes its value for tests (valueText).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "supple/ui/node.h"

namespace supple::ui {

class TextField;
class Label;
class Button;

struct Choice {
    std::string value, label;
    bool enabled = true;
};

// A button showing the chosen label and "▾"; a click opens a menu of the
// choices below it, with the current one checked.
class Dropdown : public Node {
  public:
    Dropdown(std::string id, std::vector<Choice> choices, std::string value = {});
    std::function<void(const std::string& value)> onChange;
    void setChoices(std::vector<Choice> choices);
    void setValue(const std::string& value);  // no callback
    const std::string& value() const { return value_; }
    const std::vector<Choice>& choices() const { return choices_; }
    std::string label() const;  // the chosen choice's label ("" when none matches)
    // A quieter label (a value that comes from a default, not the project).
    bool quiet = false;
    // Chooses a value as the user would (tests, scripts): calls onChange.
    void choose(const std::string& value);

    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override { return label(); }

  private:
    void open();
    std::vector<Choice> choices_;
    std::string value_;
    bool pressed_ = false;
};

// A text field with a "▾" that lists suggestions (each fills the field).
// Typed text is free: the owner validates it.
class EditableDropdown : public Node {
  public:
    EditableDropdown(std::string id, std::vector<Choice> suggestions, std::string text = {},
                     std::string placeholder = {});
    std::function<void(const std::string& text)> onChange;  // edits and picks
    void setSuggestions(std::vector<Choice> suggestions);
    void setText(const std::string& text);  // no callback
    void setPlaceholder(std::string placeholder);
    const std::string& text() const;
    TextField& field() { return *field_; }
    Button& toggle() { return *button_; }
    const std::vector<Choice>& suggestions() const { return suggestions_; }
    void pick(const std::string& value);  // as the user would: fills and calls onChange
    std::string valueText() const override;

  private:
    std::vector<Choice> suggestions_;
    TextField* field_ = nullptr;
    Button* button_ = nullptr;
};

// A box and a label; a click (or Enter/Space on focus) toggles it.
class Checkbox : public Node {
  public:
    Checkbox(std::string id, std::string text, bool checked = false);
    std::function<void(bool)> onToggle;
    void setChecked(bool checked);  // no callback
    bool checked() const { return checked_; }
    void setText(std::string text);
    const std::string& text() const { return text_; }
    void toggle();  // as the user would

    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override { return checked_ ? "on" : "off"; }

  private:
    std::string text_;
    bool checked_ = false, pressed_ = false;
};

// A titled section whose body folds away; the header shows a summary while
// folded ("−14 LUFS, −1 dBTP").
class FoldSection : public Node {
  public:
    FoldSection(std::string id, std::string title, bool open = false);
    Node& body() { return *body_; }
    void setOpen(bool open);
    bool isOpen() const { return open_; }
    void setSummary(std::string summary);
    std::function<void(bool open)> onToggle;
    std::string valueText() const override { return open_ ? "open" : "folded"; }

  private:
    std::string title_;
    bool open_ = false;
    Button* header_ = nullptr;
    Label* summary_ = nullptr;
    Node* body_ = nullptr;
};

// A list of checkboxes with All / None; onChange gets the checked values in
// list order.
class Checklist : public Node {
  public:
    explicit Checklist(std::string id);
    std::function<void(const std::vector<std::string>& checked)> onChange;
    void setItems(const std::vector<Choice>& items, const std::vector<std::string>& checked);
    std::vector<std::string> checked() const;
    void setChecked(const std::vector<std::string>& checked);  // no callback
    void setAll(bool on);  // as the All / None buttons do (calls onChange)
    std::string valueText() const override;

  private:
    void changed();
    Node* rows_ = nullptr;
    std::vector<std::pair<std::string, Checkbox*>> boxes_;
};

}  // namespace supple::ui
