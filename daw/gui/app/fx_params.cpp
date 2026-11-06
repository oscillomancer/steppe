// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "fx_params.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "supple/engine/midi_control.h"
#include "supple/json/parse.h"
#include "supple/model/schemas.h"
#include "supple/params/params.h"
#include "supple/ui/controls.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kUiOrigin{cmd::Origin::Type::Ui, "gui", {}};

std::string idText(model::Id id) { return id.str(); }

// One described semantic parameter, with the strings its spec points into kept
// alive for as long as the rows that use it.
struct Described {
    std::string key, name, group;
    std::shared_ptr<params::DescribedParam> spec;
};

}  // namespace

ParamPanel::ParamPanel(cmd::Dispatcher& dispatcher, ui::Root& root)
    : dispatcher_(dispatcher), root_(root) {}

Result<std::size_t> ParamPanel::build(ui::Node& parent, model::Id fx) {
    fx_ = fx;
    gesture_.clear();
    auto query = [&](std::string command, json::Value args) {
        return dispatcher_.execute({std::move(command), std::move(args), kUiOrigin});
    };
    json::Value describeArgs = json::Value::object();
    describeArgs["fx"] = idText(fx);
    describeArgs["full"] = true;
    auto described = query("fx.describe", std::move(describeArgs));
    if (!described) return std::move(described).error();
    const std::string name = (*described)["name"].isString() ? (*described)["name"].asString() : "";
    parent.emplace<ui::Label>("fx.params.title", name.empty() ? "Parameters" : name,
                              ui::Font::UiBold, "body");
    const json::Value* paramsJson = described->find("params");
    // The parameters' MIDI learn (fx.learn.list).
    learned_.clear();
    json::Value learnArgs = json::Value::object();
    learnArgs["fx"] = idText(fx);
    if (auto listed = query("fx.learn.list", std::move(learnArgs)); listed)
        for (const json::Value& b : (*listed)["learn"].asArray()) {
            schema::DecodeOptions options;  // the list adds fx and control
            options.unknownMembers = schema::UnknownMembers::Ignore;
            auto learn = model::paramLearnSchema().decode(b, options);
            if (!learn) continue;
            LearnChoice choice(*learn);
            choice.whenSelected = learn->whenSelected;
            choice.whenFocused = learn->whenFocused;
            choice.whenVisible = learn->whenVisible;
            learned_[learn->param] = choice;
        }

    if (!paramsJson || !paramsJson->isObject()) {
        parent.emplace<ui::Label>("fx.params.note",
                                  "This plugin has no semantic parameters (use its own editor).",
                                  ui::Font::Ui, "small", "ink-quiet");
        // Still list the plain CLAP parameters read-only, so the panel is not empty.
        std::size_t shown = 0;
        if (paramsJson && paramsJson->isArray())
            for (const json::Value& p : paramsJson->asArray()) {
                const std::string pname =
                    p.find("name") && p.find("name")->isString() ? p.find("name")->asString() : "";
                std::string value;
                if (const json::Value* v = p.find("value"); v && v->isNumber()) {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%.4g", v->asDouble());
                    value = buf;
                }
                auto& row = parent.emplace<ui::Node>("fx.plain." + std::to_string(shown));
                row.layout.dir = ui::Layout::Dir::Row;
                row.layout.height = ui::SizeRule::fixed(22);
                row.layout.gap = 6;
                row.emplace<ui::Label>("fx.plain.name." + std::to_string(shown), pname,
                                       ui::Font::Ui, "small", "ink-quiet");
                row.emplace<ui::Label>("fx.plain.value." + std::to_string(shown), value,
                                       ui::Font::Mono, "caption", "ink");
                const json::Value* id = p.find("id");
                const json::Value* readOnly = p.find("read_only");
                if (id && id->isNumber() && !(readOnly && readOnly->isBool() && readOnly->asBool()))
                    learnButton(row, "fx.plain.learn." + std::to_string(shown),
                                std::to_string(static_cast<std::uint32_t>(id->asDouble())), pname);
                ++shown;
            }
        return shown;
    }

    // Current values: the effective patch's flat "params" member.
    json::Value patchArgs = json::Value::object();
    patchArgs["fx"] = idText(fx);
    const json::Value* values = nullptr;
    json::Value patch;
    if (auto got = query("fx.patch.get", std::move(patchArgs)); got) {
        if (const json::Value* p = got->find("patch"); p && p->isObject()) {
            patch = *p;
            values = patch.find("params");
        }
    }

    std::vector<Described> rows;
    for (const auto& member : paramsJson->asObject()) {
        auto spec = params::fromDescription(member.key, member.value);
        if (!spec) continue;
        Described d;
        d.key = member.key;
        d.name = (*spec)->name.empty() ? member.key : (*spec)->name;
        d.group = (*spec)->group;
        d.spec = std::shared_ptr<params::DescribedParam>(std::move(*spec));
        rows.push_back(std::move(d));
    }
    if (rows.empty()) {
        parent.emplace<ui::Label>("fx.params.note", "No parameters.", ui::Font::Ui, "small",
                                  "ink-quiet");
        return std::size_t{0};
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Described& a, const Described& b) {
        if (a.group != b.group) return a.group < b.group;
        return a.name < b.name;
    });

    auto current = [&](const Described& d) -> double {
        const params::ParamSpec& spec = d.spec->spec;
        const json::Value* v = values && values->isObject() ? values->find(d.key) : nullptr;
        if (!v && paramsJson->isObject()) {
            if (const json::Value* e = paramsJson->find(d.key); e && e->find("default"))
                v = e->find("default");
        }
        if (!v) return spec.def;
        auto parsed = params::fromJson(spec, *v);
        return parsed ? *parsed : spec.def;
    };

    std::string lastGroup;
    std::size_t shown = 0;
    for (const Described& d : rows) {
        const params::ParamSpec& spec = d.spec->spec;
        if (spec.flags & params::kHidden) continue;
        if (d.group != lastGroup) {
            lastGroup = d.group;
            parent.emplace<ui::Label>("fx.param.group." + d.key, d.group, ui::Font::UiBold,
                                      "caption", "ink-quiet");
        }
        const double physical = current(d);
        const bool writable = !(spec.flags & params::kReadonly);
        auto& row = parent.emplace<ui::Node>("fx.param.row." + d.key);
        row.layout.dir = ui::Layout::Dir::Row;
        row.layout.height = ui::SizeRule::fixed(24);
        row.layout.gap = 6;
        row.layout.align = ui::Layout::Align::Center;
        auto& label =
            row.emplace<ui::Label>("fx.param.name." + d.key, d.name, ui::Font::Ui, "small");
        label.layout.width = ui::SizeRule::fixed(150);
        auto& control = row.emplace<ui::Node>("fx.param.control." + d.key);
        control.layout.width = ui::SizeRule::fraction();
        control.layout.height = ui::SizeRule::fraction();
        control.layout.dir = ui::Layout::Dir::Row;
        control.layout.align = ui::Layout::Align::Center;

        const std::shared_ptr<params::DescribedParam> spec_keep = d.spec;
        const std::string key = d.key;
        auto setPhysical = [this, key](double value, bool gesture) {
            setValue(key, json::Value(value), gesture);
        };
        if (spec.type == params::Type::Bool || spec.type == params::Type::Enum) {
            std::string text;
            if (spec.type == params::Type::Bool)
                text = physical >= 0.5 ? "on" : "off";
            else {
                const int index = static_cast<int>(physical + 0.5);
                text =
                    spec.choices.empty()
                        ? std::to_string(index)
                        : std::string(spec
                                          .choices[static_cast<std::size_t>(std::clamp(
                                              index, 0, static_cast<int>(spec.choices.size()) - 1))]
                                          .label);
            }
            auto* button = &control.emplace<ui::Button>(
                "fx.param." + d.key, text, [this, spec_keep, key, physical] {
                    auto& s = spec_keep->spec;
                    double next = physical;
                    if (s.type == params::Type::Bool)
                        next = physical >= 0.5 ? 0.0 : 1.0;
                    else if (!s.choices.empty())
                        next = static_cast<double>((static_cast<int>(physical + 0.5) + 1) %
                                                   static_cast<int>(s.choices.size()));
                    json::Value v = json::Value(next);
                    setValue(key, std::move(v), false);
                });
            button->enabled = writable;
            button->layout.width = ui::SizeRule::fixed(180);
        } else {
            auto* slider = &control.emplace<ui::Slider>("fx.param." + d.key);
            slider->onChange = [this, spec_keep, key, setPhysical](double normalized,
                                                                   ui::Gesture g) {
                if (g == ui::Gesture::End) {
                    gesture_.clear();
                    return;
                }
                if (g == ui::Gesture::Begin) {
                    gesture_ = "fx.param:" + idText(fx_) + ":" + key + ":" +
                               std::to_string(++gestureSerial_);
                    return;
                }
                const double value = params::clampPhysical(
                    spec_keep->spec, params::fromNormalized(spec_keep->spec, normalized));
                setPhysical(value, g == ui::Gesture::Move);
            };
            slider->setValue(params::toNormalized(spec, physical));
            slider->setDefault(params::toNormalized(spec, spec.def));
            slider->enabled = writable;
        }
        auto& field =
            row.emplace<ui::TextField>("fx.param.text." + d.key, params::format(spec, physical));
        field.layout.width = ui::SizeRule::fixed(120);
        field.enabled = writable;
        field.onSubmit = [this, key, spec_keep](const std::string& text) {
            setValue(key, json::Value(text), false);
        };
        if (writable) learnButton(row, "fx.param.learn." + d.key, d.key, d.name);
        if (writable && spec.type != params::Type::Enum) {
            auto& lane =
                row.emplace<ui::Button>("fx.param.automate." + d.key, "env",
                                        [this, key, physical] { (void)automate(key, physical); });
            lane.layout.width = ui::SizeRule::fixed(34);
        } else if (writable && openLearn) {  // keep the MIDI buttons in one column
            row.emplace<ui::Node>("fx.param.nolane." + d.key).layout.width =
                ui::SizeRule::fixed(34);
        }
        ++shown;
    }
    (void)root_;
    return shown;
}

Status ParamPanel::automate(const std::string& key, double physical) {
    json::Value listArgs = json::Value::object();
    listArgs["object"] = idText(fx_);
    auto listed = dispatcher_.execute({"env.list", std::move(listArgs), kUiOrigin});
    if (!listed) return std::move(listed).error();
    for (const json::Value& e : (*listed)["envelopes"].asArray()) {
        if (e.find("param")->asString() != key) continue;
        json::Value args = json::Value::object();
        args["env"] = e.find("env")->asString();
        args["visible"] = true;
        auto shown = dispatcher_.execute({"env.set", std::move(args), kUiOrigin});
        if (!shown) return std::move(shown).error();
        return {};
    }
    json::Value args = json::Value::object(), points = json::Value::array(),
                first = json::Value::object();
    args["fx"] = idText(fx_);
    args["param"] = key;
    first["beat"] = 0.0;
    first["value"] = physical;
    points.push_back(std::move(first));
    args["points"] = std::move(points);
    auto added = dispatcher_.execute({"env.add", std::move(args), kUiOrigin});
    if (!added) return std::move(added).error();
    return {};
}

void ParamPanel::learnButton(ui::Node& row, const std::string& id, const std::string& param,
                             const std::string& title) {
    if (!openLearn) return;
    std::string text = "MIDI";
    if (auto it = learned_.find(param); it != learned_.end()) {
        text = engine::describe(it->second.control);
        text = text.substr(0, text.find(" · "));  // "CC 21": the channel is in the dialog
    }
    auto& button = row.emplace<ui::Button>(id, text, [this, param, title] { learn(param, title); });
    button.layout.width = ui::SizeRule::fixed(76);
}

void ParamPanel::learn(const std::string& param, const std::string& title) {
    LearnRequest request;
    request.title = title;
    request.takeover = request.conditions = true;
    if (auto it = learned_.find(param); it != learned_.end()) request.current = it->second;
    const std::string fx = idText(fx_);
    request.onOk = [this, fx, param](const LearnChoice& choice) {
        json::Value args = learnArgs(choice, true);
        args["fx"] = fx;
        args["param"] = param;
        auto set = dispatcher_.execute({"fx.learn.set", std::move(args), kUiOrigin});
        if (!set && onError) onError(set.error().message);
    };
    request.onRemove = [this, fx, param] {
        json::Value args = json::Value::object();
        args["fx"] = fx;
        args["param"] = param;
        auto removed = dispatcher_.execute({"fx.learn.remove", std::move(args), kUiOrigin});
        if (!removed && onError) onError(removed.error().message);
    };
    if (openLearn) openLearn(std::move(request));
}

void ParamPanel::setValue(const std::string& key, json::Value value, bool gesture) {
    json::Value values = json::Value::object();
    values[key] = std::move(value);
    json::Value args = json::Value::object();
    args["fx"] = idText(fx_);
    args["values"] = std::move(values);
    const cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}, gesture ? gesture_ : std::string{}};
    auto result = dispatcher_.execute({"fx.param.set", std::move(args), origin});
    if (!result && onError) onError(result.error().message);
}

}  // namespace supple::gui
