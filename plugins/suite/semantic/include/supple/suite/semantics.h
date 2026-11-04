// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What a plugin means, beyond the framework's tables: the documents of the
// com.suppleaudio.semantic-params/1 extension (Steppe's
// docs/specs/semantic-params-extension.md, core-params and core-patch).
//
//   - Meaning: each parameter's key ("filter.cutoff"), role tags and choice
//     keys, each property's key, the patch type and a paragraph of docs. A
//     SemanticDefinition carries its own (the player's scripts do); any
//     other Definition gets one derived from its names.
//   - PatchCodec: a state snapshot (core/StateFormat.h) as a patch document
//     and back -- values by key in physical units, choices by key, toggles
//     as booleans, properties as text, modulation routes by source and
//     destination -- with merge (RFC 7396), validation
//     whose errors carry a JSON Pointer and a hint, the semantic schema
//     document, the patch's JSON Schema, and a description in prose.
//
// Physical units are core-params': a percentage in percent (the framework's
// plain value is a fraction), seconds as milliseconds. A continuous
// parameter's CLAP value is its 0..1 position (normalizedHostValue()).
// Main thread only.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Definition.h"
#include "core/StateFormat.h"
#include "supple/suite/json.h"

namespace supple::suite {

// A condition on whether a parameter applies, on parameter `param` (earlier
// in the table): a toggle or a choice holds one of `values` (plain: a
// toggle's 0 or 1, a choice's index), or a number -- continuous or integer,
// `values` empty -- lies strictly between `above` and `below` (plain).
struct ActiveCondition {
    size_t param = 0;
    std::vector<int> values;
    double above = -std::numeric_limits<double>::infinity();
    double below = std::numeric_limits<double>::infinity();
    bool operator==(const ActiveCondition&) const = default;
};

// Whether the patch's routes may move a parameter: from the global sources
// only, or from any (an instrument's, per voice).
enum class Routable : uint8_t { No, Global, Voice };

struct ParamMeaning {
    std::string key;                      // [a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*
    std::vector<std::string> tags;        // "role:level", "stage:filter", "macro"
    std::vector<std::string> choiceKeys;  // a choice's options, lower snake case
    // It applies while each holds and the parameter each names applies;
    // always, without any.
    std::vector<ActiveCondition> activeWhen;
    // A continuous parameter's: routes may move it, as far as routeMin to
    // routeMax (physical; its own range when routeMin > routeMax).
    Routable routable = Routable::No;
    double routeMin = 1.0, routeMax = 0.0;
    bool operator==(const ParamMeaning&) const = default;
};

// A modulation source the patch's routes name: a DSP script's Source
// member, or one an instrument has of its own (velocity, modwheel ...).
struct SourceMeaning {
    std::string key;  // [a-z][a-z0-9_]*
    std::string name, description;
    bool perVoice = false;  // a value per voice (an instrument's)
    bool bipolar = true;    // -1..1, or 0..1
    bool operator==(const SourceMeaning&) const = default;
};

struct PropertyMeaning {
    std::string key;
    bool operator==(const PropertyMeaning&) const = default;
};

struct Meaning {
    std::string patchType;                    // "supple.<slug>-patch/<major>.<minor>"
    std::string docs;                         // what the plugin is and how its patch is organised
    std::vector<ParamMeaning> params;         // by parameter table index
    std::vector<PropertyMeaning> properties;  // by property index
    // The sources routes name, and the property that keeps the routes (as
    // routeText describes); -1 for a plugin without routes. The patch says
    // them as its "routes" member, never among its properties.
    std::vector<SourceMeaning> sources;
    int routes = -1;
    bool operator==(const Meaning&) const = default;
};

// --- routes ---------------------------------------------------------------------------------
//
// A route moves a parameter by a source's value times its amount: in the
// parameter's physical unit (a percent in percent, a level in dB), except a
// frequency's, in octaves; through a second source (`via`) it is scaled by
// that one's value too. The sum of a parameter's routes is kept to its
// route range. The routes property keeps them one to a line, sorted, as
//
//   source>destination amount
//   source>destination amount via
//
// with the amount as JSON writes a number. At most kMaxRoutes.
inline constexpr size_t kMaxRoutes = 64;

struct RouteLine {
    std::string_view source, destination, via;  // via empty: none
    double amount = 0.0;
};

// One line of a routes property; false when it is malformed. Allocates
// nothing.
bool readRouteLine(std::string_view line, RouteLine& route);

// Each line of a routes property in turn; false, stopping there, at one
// that is malformed. Allocates nothing: the audio thread reads the
// property with it.
template <class Fn>
bool forEachRoute(std::string_view text, Fn&& fn) {
    while (!text.empty()) {
        const size_t end = std::min(text.find('\n'), text.size());
        const std::string_view line = text.substr(0, end);
        text.remove_prefix(std::min(end + 1, text.size()));
        if (line.empty()) continue;
        RouteLine r;
        if (!readRouteLine(line, r)) return false;
        fn(r);
    }
    return true;
}

// How a route's amount moves a parameter's plain value: by octaves (a
// frequency's), or added after multiplying by routeScale() (a percent's
// plain value is a fraction: 0.01).
bool routeInOctaves(const ParamDef& p);
double routeScale(const ParamDef& p);
// The unit a route's amount for this parameter is in: a frequency's "oct",
// any other's own (nullptr: a plain number). How far routes may move it,
// in that unit: the width of its route range.
const char* routeAmountUnit(const ParamDef& p);
double routeReach(const ParamDef& p, const ParamMeaning& meaning);

// A Definition that says what it means.
class SemanticDefinition : public Definition {
  public:
    SemanticDefinition(PluginInfo info, ParamTable params, PropertyList properties, Meaning meaning,
                       std::vector<FactoryPreset> presets = {})
        : Definition(std::move(info), std::move(params), std::move(properties), std::move(presets)),
          meaning_(std::move(meaning)) {}
    const Meaning& meaning() const { return meaning_; }
    // By its meaning's conditions (activity()).
    bool applies(size_t index, const std::function<double(size_t)>& host) const override;

  private:
    Meaning meaning_;
};

// Lower snake case of any text: "High Cut" -> "high_cut", "Mix %" -> "mix".
std::string snakeCase(std::string_view text);
// A Definition's meaning: its own, or one derived from its names (a key from
// the group and the name, choice keys from the labels, the patch type from
// the plugin's name).
Meaning meaningOf(const Definition& definition);
// What is wrong with a meaning for this definition (keys malformed or not
// unique, counts that do not match, conditions on a later parameter or a
// value it does not have); empty when nothing is.
std::string meaningProblems(const Definition& definition, const Meaning& meaning);
// Which parameters apply (ParamMeaning::activeWhen), from each one's host
// value by table index. The meaning has no problems.
std::vector<bool> activity(const Definition& definition, const Meaning& meaning,
                           const std::function<double(size_t)>& host);

// The patch envelope (core-patch §1): what a patch says about itself.
struct Envelope {
    std::optional<std::string> name, author, notes;
    std::vector<std::string> tags;
};

class PatchCodec {
  public:
    // The definition outlives the codec.
    explicit PatchCodec(const Definition& definition);

    const Meaning& meaning() const { return meaning_; }

    // The schema document (supple.semantic-schema/1.0), the "params" member
    // alone (core-params' describeTable), and the patch's JSON Schema. With
    // the current values, a parameter that does not apply now says
    // "active": false.
    json::Value schemaDocument(const state::Snapshot* current = nullptr) const;
    json::Value paramsDescription(const state::Snapshot* current = nullptr) const;
    json::Value jsonSchema() const;
    // Which parameters apply with these values (activity()).
    std::vector<bool> activity(const state::Snapshot& snapshot) const;

    // A "routes" member (an object of routes by "source>destination") as
    // the routes property's text; false with errors (paths under `path`).
    bool routeText(const json::Value& routes, const std::string& path, std::string& text,
                   std::vector<json::Error>& errors) const;

    // The snapshot as a patch document; with `changedOnly`, only values that
    // differ from their defaults (still a complete patch: missing values
    // read as defaults).
    json::Value patch(const state::Snapshot& snapshot, const Envelope& envelope,
                      bool changedOnly) const;

    // A document onto `snapshot` and `envelope`: replacing everything (what it
    // leaves out takes its default) or merged into what they hold (RFC 7396).
    // Transactional: false with errors, nothing changed, otherwise both
    // updated. Error paths point into the document as merged. A file
    // property the document changes must name an existing file of a type it
    // accepts, by absolute path, that the file check (when set) can read;
    // "" means none.
    bool apply(const json::Value& document, bool merge, state::Snapshot& snapshot,
               Envelope& envelope, std::vector<json::Error>& errors) const;

    // A saved patch onto a snapshot being loaded, leniently: values of keys
    // the plugin has replace the snapshot's, anything else is skipped, so a
    // session saved before a parameter went still loads. A snapshot value
    // that writes the patch's own value stays as it is, exactly: a state
    // saved and loaded saves the same bytes again.
    void restore(const json::Value& document, state::Snapshot& snapshot, Envelope& envelope) const;

    // Whether property `index` can use the file at `path` (which exists), and
    // if not, why: for instance, whether its decoder reads it.
    using FileCheck = std::function<bool(size_t index, const std::string& path, std::string& why)>;
    void setFileCheck(FileCheck check) { fileCheck_ = std::move(check); }
    // The plugin's own audio files, "factory:<name>" (the framework's
    // core/Files.h): a file property may take one, and the schema lists them.
    // The file check reads them as it reads a path.
    void setFactoryFiles(std::vector<std::string> references) {
        factoryFiles_ = std::move(references);
    }

    // Compact prose: "Tilt: Tilt +3.0 dB; Pivot 1.20 kHz", or "default
    // settings", of the parameters that apply. With `changedOnly`, values
    // that differ from their defaults.
    std::string describe(const state::Snapshot& snapshot, bool changedOnly) const;

    // A parameter's physical value and back (percent in percent, seconds in
    // ms), and its JSON form.
    double physical(size_t index, double plain) const;
    double plain(size_t index, double physical) const;
    json::Value valueJson(size_t index, double host) const;

  private:
    // Whether it has properties a patch shows (any but the routes').
    bool hasProperties() const {
        return definition_.properties().count() > (meaning_.routes >= 0 ? 1u : 0u);
    }
    std::string exampleRoute() const;
    // The routes property's text as the patch's "routes" member, and the
    // text a snapshot holds.
    json::Value routesJson(const std::string& text) const;
    const std::string& routesOf(const state::Snapshot& snapshot) const;
    bool readValue(size_t index, const json::Value& v, const std::string& path, double& host,
                   std::vector<json::Error>& errors) const;
    bool fileUsable(size_t index, const std::string& path, const state::Snapshot& current,
                    std::string& why) const;

    const Definition& definition_;
    Meaning meaning_;
    FileCheck fileCheck_;
    std::vector<std::string> factoryFiles_;
};

// The patch framed as a state chunk (core-patch §4): the magic, version 2,
// the byte length and the canonical JSON.
std::vector<uint8_t> frame(const std::string& canonicalJson, uint32_t magic);
bool unframe(const uint8_t* data, size_t size, uint32_t magic, std::string& json);

}  // namespace supple::suite
