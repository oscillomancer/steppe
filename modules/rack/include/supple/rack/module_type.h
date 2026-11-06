// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Module types (ADR-0062, docs/specs/rack.md §4): what a rack can hold, by id.
// A type is compiled module code (a script, by the JIT or as C++) or a host
// module's description (the host implements its audio, rack::HostModule).
// Either way it describes itself with the script runtime's models, so the
// rack, the commands and the window treat every module alike.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "supple/script_module.h"

namespace supple::rack {

namespace sd = script::detail;

// Where a module type comes from. Project types win over library types, and
// those over stock ones, for the same id.
enum class Origin : std::uint8_t { Host, Stock, Library, Project };
std::string_view originName(Origin origin);

// A module's face: its size and where each control sits, in design units
// from its top-left corner.
struct PanelItem {
    enum class Kind : std::uint8_t {
        Knob,
        Slider,
        Toggle,
        Choice,
        Readout,
        Input,
        Output,
        Display
    };
    Kind kind = Kind::Knob;
    int index = -1;  // a parameter's (Knob..Readout) or a port's among its direction; -1 Display
    double x = 0, y = 0;  // a knob's or a jack's centre; a rectangle's top-left
    double w = 0, h = 0;  // a knob's diameter (w = h); a rectangle's size; 0 for a jack
};
struct PanelLayout {
    double width = 0, height = 0;
    std::vector<PanelItem> items;
    bool custom = false;  // from the module's panel(), not the automatic layout
};

// The automatic layout's measures (rack.md §4.5).
inline constexpr double kTitleHeight = 24;
inline constexpr double kKnobSize = 36;
inline constexpr double kColumn = 44;   // a knob and its label
inline constexpr double kKnobRow = 58;  // a knob, its label and a gap
inline constexpr double kJackSize = 22;
inline constexpr double kJackPitch = 36;
inline constexpr double kJackRow = 46;  // a jack, its label and a gap
inline constexpr double kMargin = 8;

class ModuleType {
  public:
    // A script's (stock, library or project), described by its code.
    static std::shared_ptr<const ModuleType> fromCode(std::shared_ptr<const sd::ModuleCode> code,
                                                      Origin origin, std::string source = {},
                                                      std::string path = {});
    // A host module's: its description; the host makes its instances.
    static std::shared_ptr<const ModuleType> host(sd::ModuleInfoModel info,
                                                  std::vector<sd::ParamModel> params,
                                                  std::vector<sd::PortModel> ports);

    const std::string& id() const { return info_.id; }
    Origin origin() const { return origin_; }
    const sd::ModuleInfoModel& info() const { return info_; }
    const std::vector<sd::ParamModel>& params() const { return params_; }
    // Every port in declaration order; inputs() and outputs() index them by
    // direction (a PortModel's `index`).
    const std::vector<sd::PortModel>& ports() const { return ports_; }
    const std::vector<int>& inputs() const { return inputs_; }
    const std::vector<int>& outputs() const { return outputs_; }
    const sd::PortModel& input(int k) const {
        return ports_[static_cast<std::size_t>(inputs_[static_cast<std::size_t>(k)])];
    }
    const sd::PortModel& output(int k) const {
        return ports_[static_cast<std::size_t>(outputs_[static_cast<std::size_t>(k)])];
    }
    // Null for a host module.
    const std::shared_ptr<const sd::ModuleCode>& code() const { return code_; }
    const std::string& source() const { return source_; }
    const std::string& path() const { return path_; }
    const PanelLayout& layout() const { return layout_; }

    // By key; -1 when it has none.
    int param(std::string_view key) const;
    int input(std::string_view key) const;
    int output(std::string_view key) const;

    // What is wrong with the description (keys, limits, ids), in words; empty
    // when nothing is. A type with problems is not added to a catalog.
    std::string problems() const;

  private:
    ModuleType() = default;
    void index();
    sd::ModuleInfoModel info_;
    Origin origin_ = Origin::Host;
    std::vector<sd::ParamModel> params_;
    std::vector<sd::PortModel> ports_;
    std::vector<int> inputs_, outputs_;
    std::shared_ptr<const sd::ModuleCode> code_;
    std::string source_, path_;
    PanelLayout layout_;
};

// The automatic layout for a description (rack.md §4.5).
PanelLayout automaticLayout(const std::vector<sd::ParamModel>& params,
                            const std::vector<sd::PortModel>& ports, int width = 0, int height = 0);
// The layout a module's panel() makes, run on a probe instance; the
// automatic one when it has none. Main thread.
PanelLayout layoutOf(const sd::ModuleCode& code);

// Module types by id: the host's, the stock ones, the user's library and the
// project's. Main thread. Each change bumps the generation of what it
// touched, so racks see code that changed under the same id.
class Catalog {
  public:
    struct Entry {
        std::shared_ptr<const ModuleType> type;
        std::uint64_t generation = 0;
    };
    // Adds or replaces the type of that id and origin. Refuses a type with
    // problems (returns them).
    std::string add(std::shared_ptr<const ModuleType> type);
    void remove(std::string_view id, Origin origin);
    void clear(Origin origin);
    // The type racks use for an id: the project's, else the library's, else
    // the stock or host one.
    const Entry* find(std::string_view id) const;
    const Entry* find(std::string_view id, Origin origin) const;
    // Every id once (the one find() gives), by origin then name.
    std::vector<const Entry*> list() const;
    std::uint64_t generation() const { return generation_; }

  private:
    std::map<std::pair<std::string, Origin>, Entry, std::less<>> types_;
    std::uint64_t generation_ = 0;
};

}  // namespace supple::rack
