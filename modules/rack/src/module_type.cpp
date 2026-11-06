// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/rack/module_type.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "supple/script_runtime.h"

namespace supple::rack {

std::string_view originName(Origin origin) {
    switch (origin) {
        case Origin::Host:
            return "host";
        case Origin::Stock:
            return "stock";
        case Origin::Library:
            return "library";
        case Origin::Project:
            return "project";
    }
    return "host";
}

std::shared_ptr<const ModuleType> ModuleType::fromCode(std::shared_ptr<const sd::ModuleCode> code,
                                                       Origin origin, std::string source,
                                                       std::string path) {
    if (!code) return nullptr;
    auto t = std::shared_ptr<ModuleType>(new ModuleType());
    t->info_ = code->info();
    t->origin_ = origin;
    t->params_ = code->params();
    t->ports_ = code->ports();
    t->code_ = std::move(code);
    t->source_ = std::move(source);
    t->path_ = std::move(path);
    t->index();
    t->layout_ = layoutOf(*t->code_);
    return t;
}

std::shared_ptr<const ModuleType> ModuleType::host(sd::ModuleInfoModel info,
                                                   std::vector<sd::ParamModel> params,
                                                   std::vector<sd::PortModel> ports) {
    auto t = std::shared_ptr<ModuleType>(new ModuleType());
    t->info_ = std::move(info);
    t->origin_ = Origin::Host;
    t->params_ = std::move(params);
    t->ports_ = std::move(ports);
    t->index();
    t->layout_ = automaticLayout(t->params_, t->ports_, t->info_.width, t->info_.height);
    return t;
}

void ModuleType::index() {
    inputs_.clear();
    outputs_.clear();
    for (std::size_t i = 0; i < ports_.size(); ++i) {
        std::vector<int>& list = ports_[i].output ? outputs_ : inputs_;
        ports_[i].index = static_cast<int>(list.size());
        list.push_back(static_cast<int>(i));
    }
}

int ModuleType::param(std::string_view key) const {
    for (std::size_t i = 0; i < params_.size(); ++i)
        if (params_[i].key == key) return static_cast<int>(i);
    return -1;
}
int ModuleType::input(std::string_view key) const {
    for (std::size_t k = 0; k < inputs_.size(); ++k)
        if (ports_[static_cast<std::size_t>(inputs_[k])].key == key) return static_cast<int>(k);
    return -1;
}
int ModuleType::output(std::string_view key) const {
    for (std::size_t k = 0; k < outputs_.size(); ++k)
        if (ports_[static_cast<std::size_t>(outputs_[k])].key == key) return static_cast<int>(k);
    return -1;
}

namespace {

bool keyWord(const std::string& key) {
    if (key.empty() || !std::islower(static_cast<unsigned char>(key[0]))) return false;
    for (char c : key)
        if (!(std::islower(static_cast<unsigned char>(c)) ||
              std::isdigit(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    return true;
}

// Reverse DNS: words of letters, digits and '-' joined by '.', two or more.
bool reverseDns(const std::string& id) {
    if (id.empty() || id.front() == '.' || id.back() == '.') return false;
    int words = 1;
    char last = 0;
    for (char c : id) {
        if (c == '.') {
            if (last == '.' || last == 0) return false;
            ++words;
        } else if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-')) {
            return false;
        }
        last = c;
    }
    return words >= 2;
}

}  // namespace

std::string ModuleType::problems() const {
    std::string out;
    auto add = [&](const std::string& p) { out += (out.empty() ? "" : "; ") + p; };
    if (!reverseDns(info_.id))
        add("id '" + info_.id +
            "' is not reverse-DNS (words of letters, digits and '-' joined by '.')");
    if (info_.name.empty()) add("it has no name");
    std::set<std::string> keys;
    for (const sd::ParamModel& p : params_) {
        if (!keyWord(p.key))
            add("parameter key '" + p.key + "' is not lower case letters, digits and _");
        if (!keys.insert(p.key).second) add("two members have the key '" + p.key + "'");
    }
    for (const sd::PortModel& p : ports_) {
        if (!keyWord(p.key))
            add("port key '" + p.key + "' is not lower case letters, digits and _");
        if (!keys.insert(p.key).second) add("two members have the key '" + p.key + "'");
    }
    if (inputs_.size() > 32) add("more than 32 inputs");
    if (outputs_.size() > 32) add("more than 32 outputs");
    if (params_.size() > 256) add("more than 256 parameters");
    if (info_.tags.size() > 4) add("more than four tags");
    return out;
}

// --- layout -------------------------------------------------------------------------

PanelLayout automaticLayout(const std::vector<sd::ParamModel>& params,
                            const std::vector<sd::PortModel>& ports, int width, int height) {
    PanelLayout l;
    // Knobs: every parameter a user sets (meters are readouts), grouped in
    // declaration order of their groups.
    std::vector<int> controls, readouts;
    std::vector<std::string> groups;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (params[i].hidden) continue;
        (params[i].output ? readouts : controls).push_back(static_cast<int>(i));
        if (std::find(groups.begin(), groups.end(), params[i].group) == groups.end())
            groups.push_back(params[i].group);
    }
    std::stable_sort(controls.begin(), controls.end(), [&](int a, int b) {
        const auto ga =
            std::find(groups.begin(), groups.end(), params[static_cast<std::size_t>(a)].group);
        const auto gb =
            std::find(groups.begin(), groups.end(), params[static_cast<std::size_t>(b)].group);
        return ga < gb;
    });
    int inputs = 0, outputs = 0;
    for (const sd::PortModel& p : ports) (p.output ? outputs : inputs)++;
    const int knobs = static_cast<int>(controls.size());
    const int jacks = std::max(inputs, outputs);
    // Columns: two to four, enough that the face is not too tall.
    int columns = knobs <= 2 ? std::max(1, knobs) : knobs <= 6 ? 2 : knobs <= 12 ? 3 : 4;
    const int jackColumns =
        std::clamp(jacks, 1, std::max(2, static_cast<int>((columns * kColumn) / kJackPitch)));
    double w = std::max(columns * kColumn, jackColumns * kJackPitch) + 2 * kMargin;
    w = std::max(w, 2 * kJackPitch + 2 * kMargin);
    if (width > 0) w = width;
    l.width = w;
    const double inner = w - 2 * kMargin;
    double y = kTitleHeight + kMargin;
    // Knobs row by row, centred in their columns.
    const int perRow = std::max(1, std::min(columns, static_cast<int>(inner / kColumn)));
    for (int k = 0; k < knobs; ++k) {
        const int row = k / perRow, col = k % perRow;
        const int inRow = std::min(perRow, knobs - row * perRow);
        const double rowWidth = inRow * kColumn;
        const double x0 = kMargin + (inner - rowWidth) / 2;
        PanelItem item;
        const auto& p = params[static_cast<std::size_t>(controls[static_cast<std::size_t>(k)])];
        item.index = controls[static_cast<std::size_t>(k)];
        // A choice is a stepped knob; a toggle a button in the knob's place
        // (a rectangle, as a panel() gives one).
        const double cx = x0 + col * kColumn + kColumn / 2;
        const double cy = y + row * kKnobRow + kKnobSize / 2;
        if (p.kind == sd::ParamModel::Kind::Toggle) {
            item.kind = PanelItem::Kind::Toggle;
            item.w = kColumn - 6;
            item.h = 20;
            item.x = cx - item.w / 2;
            item.y = cy - item.h / 2;
        } else {
            item.kind = PanelItem::Kind::Knob;
            item.x = cx;
            item.y = cy;
            item.w = item.h = kKnobSize;
        }
        l.items.push_back(item);
    }
    if (knobs) y += ((knobs + perRow - 1) / perRow) * kKnobRow;
    for (int r : readouts) {
        PanelItem item;
        item.kind = PanelItem::Kind::Readout;
        item.index = r;
        item.x = kMargin;
        item.y = y;
        item.w = inner;
        item.h = 18;
        l.items.push_back(item);
        y += 22;
    }
    // Jacks: inputs, then outputs, in rows.
    auto jackRows = [&](bool output, int count) {
        if (count == 0) return;
        const int per = std::max(1, static_cast<int>(inner / kJackPitch));
        int k = 0;
        for (const sd::PortModel& p : ports) {
            if (p.output != output) continue;
            const int row = k / per, col = k % per;
            const int inRow = std::min(per, count - row * per);
            const double x0 = kMargin + (inner - inRow * kJackPitch) / 2;
            PanelItem item;
            item.kind = output ? PanelItem::Kind::Output : PanelItem::Kind::Input;
            item.index = p.index;
            item.x = x0 + col * kJackPitch + kJackPitch / 2;
            item.y = y + row * kJackRow + kJackSize / 2 + 4;
            l.items.push_back(item);
            ++k;
        }
        y += ((count + per - 1) / per) * kJackRow;
    };
    jackRows(false, inputs);
    jackRows(true, outputs);
    l.height = height > 0 ? height : y + kMargin;
    return l;
}

namespace {

// Collects what a module's panel() lays out.
class Collector final : public sd::PanelBuilder {
  public:
    explicit Collector(PanelLayout& l) : l_(l) {}
    void size(double width, double height) override {
        l_.width = width;
        l_.height = height;
    }
    void control(Control kind, int slot, double x, double y, double w, double h) override {
        PanelItem item;
        item.kind = kind == Control::Knob     ? PanelItem::Kind::Knob
                    : kind == Control::Slider ? PanelItem::Kind::Slider
                    : kind == Control::Toggle ? PanelItem::Kind::Toggle
                    : kind == Control::Choice ? PanelItem::Kind::Choice
                                              : PanelItem::Kind::Readout;
        item.index = slot;
        item.x = x;
        item.y = y;
        item.w = w;
        item.h = h;
        if (slot >= 0) l_.items.push_back(item);
    }
    void jack(bool output, int index, double x, double y) override {
        PanelItem item;
        item.kind = output ? PanelItem::Kind::Output : PanelItem::Kind::Input;
        item.index = index;
        item.x = x;
        item.y = y;
        if (index >= 0) l_.items.push_back(item);
    }
    void display(double x, double y, double w, double h) override {
        l_.items.push_back({PanelItem::Kind::Display, -1, x, y, w, h});
    }

  private:
    PanelLayout& l_;
};

}  // namespace

PanelLayout layoutOf(const sd::ModuleCode& code) {
    const auto& info = code.info();
    if (!code.panel()) return automaticLayout(code.params(), code.ports(), info.width, info.height);
    PanelLayout l;
    l.custom = true;
    void* self = code.create();
    // The panel names members by slot: number them as an instance does.
    unsigned char* base = static_cast<unsigned char*>(self);
    for (std::size_t i = 0; i < code.params().size(); ++i)
        sd::ParamBinding::setSlot(*reinterpret_cast<script::Param*>(base + code.params()[i].offset),
                                  static_cast<int>(i));
    for (const sd::PortModel& p : code.ports()) {
        if (p.output)
            sd::PortBinding::setSlot(*reinterpret_cast<script::Output*>(base + p.offset), p.index);
        else
            sd::PortBinding::setSlot(*reinterpret_cast<script::Input*>(base + p.offset), p.index);
    }
    sd::Instance record;
    sd::Instance::bind(*reinterpret_cast<script::Plugin*>(self), &record);
    Collector collector(l);
    script::Panel panel;
    sd::Layout::bind(panel, &collector);
    {
        sd::CurrentScope scope(&record);
        record.budget = std::int64_t{1} << 22;
        code.panel()(&record, self, &panel);
    }
    code.destroy(self);
    if (l.width <= 0 || l.height <= 0) {
        const PanelLayout a = automaticLayout(code.params(), code.ports(), info.width, info.height);
        if (l.width <= 0) l.width = a.width;
        if (l.height <= 0) l.height = a.height;
    }
    return l;
}

// --- the catalog ---------------------------------------------------------------------

std::string Catalog::add(std::shared_ptr<const ModuleType> type) {
    if (!type) return "no module type";
    if (std::string p = type->problems(); !p.empty()) return p;
    Entry& e = types_[{type->id(), type->origin()}];
    e.type = std::move(type);
    e.generation = ++generation_;
    return {};
}

void Catalog::remove(std::string_view id, Origin origin) {
    if (types_.erase(std::pair<std::string, Origin>{std::string(id), origin})) ++generation_;
}

void Catalog::clear(Origin origin) {
    for (auto it = types_.begin(); it != types_.end();)
        it = it->first.second == origin ? types_.erase(it) : std::next(it);
    ++generation_;
}

const Catalog::Entry* Catalog::find(std::string_view id, Origin origin) const {
    auto it = types_.find(std::pair<std::string, Origin>{std::string(id), origin});
    return it == types_.end() ? nullptr : &it->second;
}

const Catalog::Entry* Catalog::find(std::string_view id) const {
    for (Origin o : {Origin::Project, Origin::Library, Origin::Stock, Origin::Host})
        if (const Entry* e = find(id, o)) return e;
    return nullptr;
}

std::vector<const Catalog::Entry*> Catalog::list() const {
    std::vector<const Entry*> out;
    for (const auto& [key, entry] : types_)
        if (find(key.first) == &entry) out.push_back(&entry);
    std::stable_sort(out.begin(), out.end(), [](const Entry* a, const Entry* b) {
        if (a->type->origin() != b->type->origin()) return a->type->origin() < b->type->origin();
        return a->type->info().name < b->type->info().name;
    });
    return out;
}

}  // namespace supple::rack
