// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "../../../tests/file_platform.h"
#include "../../../tests/wav_stream_test_support.h"
#include "../src/media_files.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/engine/app.h"
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace supple::engine;

TEST_CASE("media copies checkpoint payload and publication and propagate sync failures") {
    const auto root = fs::temp_directory_path() /
                      ("supple-copy-sync-" + std::to_string(supple::test::processId()));
    REQUIRE(fs::create_directory(root));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::permissions(path / "source" / "take.wav", fs::perms::owner_write,
                            fs::perm_options::add, ec);
            fs::remove_all(path, ec);
        }
    } cleanup{root};
    fs::create_directory(root / "source");
    const auto source = root / "source" / "take.wav";
    supple::audio::AudioData audio;
    audio.sampleRate = 48000;
    audio.channels = {{.25f, -.25f}};
    REQUIRE(supple::audio::writeWav(source, audio));
    const auto before = supple::sha256File(source).value();
    const auto target = root / "dest";
    const auto realTarget = fs::weakly_canonical(target);
    bool folder = false, failData = false, failDirectory = false, readonly = false;
    SUBCASE("import") { folder = false; }
    SUBCASE("folder") { folder = true; }
    SUBCASE("import: data barrier failure") { failData = true; }
    SUBCASE("folder: data barrier failure") { folder = failData = true; }
    SUBCASE("import: directory barrier failure") { failDirectory = true; }
    SUBCASE("folder: directory barrier failure") { folder = failDirectory = true; }
    SUBCASE("read-only source") { readonly = true; }
    if (readonly)
        fs::permissions(source,
                        fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove);
    std::vector<fs::path> synced;
    struct ResetHook {
        ~ResetHook() { supple::setDurabilityHookForTesting({}); }
    } reset;
    supple::setDurabilityHookForTesting([&](const fs::path& path, bool) {
        synced.push_back(path);
        if (path == realTarget) return !failDirectory;
        return !(failData && path.parent_path() == target && fs::is_regular_file(path));
    });
    bool created = false;
    std::vector<fs::path> owned;
    supple::Status status = supple::okStatus();
    if (folder) {
        status = detail::copyFolder(source.parent_path(), target, &owned);
        created = !owned.empty();
    } else {
        const auto copied = detail::copyIntoFolder(source, target, &created);
        if (!copied) status = copied.error();
    }
    CHECK_FALSE(synced.empty());
    CHECK(supple::sha256File(source).value() == before);
    if (readonly)
        CHECK((fs::status(source).permissions() & fs::perms::owner_write) == fs::perms::none);
    if (failData || failDirectory)
        CHECK_FALSE(status);
    else
        CHECK(status);
    if (failData) {
        CHECK_FALSE(fs::exists(target / "take.wav"));
        CHECK_FALSE(created);
    } else {
        REQUIRE(fs::exists(target / "take.wav"));
        CHECK(supple::sha256File(target / "take.wav").value() == before);
        CHECK(created);
        CHECK(std::find(synced.begin(), synced.end(), realTarget) != synced.end());
    }
}

namespace {
// These cases run in separate CTest processes: the first staging serial is
// deterministically zero, regardless of earlier imports in the ordinary suite.
void stagingCollision(bool folder, bool upper) {
    const auto root = fs::temp_directory_path() /
                      ("supple-copy-name-" + std::to_string(supple::test::processId()));
    REQUIRE(fs::create_directory(root));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    fs::create_directory(root / "source");
    const auto name = std::string(upper ? ".COPY-" : ".copy-") +
                      std::to_string(supple::test::processId()) + "-0.copy-tmp";
    const auto source = root / "source" / name;
    supple::audio::AudioData audio;
    audio.sampleRate = 48000;
    audio.channels = {{.25f, -.25f, .5f, -.5f}};
    REQUIRE(supple::audio::writeWav(source, audio));
    const auto before = supple::sha256File(source);
    REQUIRE(before);
    const auto target = root / "dest";
    if (folder) {
        REQUIRE(detail::copyFolder(source.parent_path(), target));
    } else {
        const auto copied = detail::copyIntoFolder(source, target);
        REQUIRE_MESSAGE(copied, (copied ? "" : copied.error().toString()));
        CHECK(*copied == name);
    }
    const auto decoded = supple::audio::readAudio(target / name);
    REQUIRE(decoded);
    CHECK(decoded->channels == audio.channels);
    CHECK(supple::sha256File(target / name).value() == *before);
    CHECK(supple::sha256File(source).value() == *before);
    CHECK(std::distance(fs::directory_iterator(target), fs::directory_iterator{}) == 1);
}
}  // namespace

TEST_CASE("media staging collision: import" * doctest::skip()) { stagingCollision(false, false); }
TEST_CASE("media staging collision: folder" * doctest::skip()) { stagingCollision(true, false); }
TEST_CASE("media staging collision: import-case" * doctest::skip()) {
    stagingCollision(false, true);
}
TEST_CASE("media staging collision: folder-case" * doctest::skip()) {
    stagingCollision(true, true);
}

TEST_CASE("media copy accepts a valid filename without room for a staging suffix") {
    const auto root = fs::temp_directory_path() /
                      ("supple-media-long-" + std::to_string(supple::test::processId()) + "-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(root));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    fs::create_directory(root / "source");
    const std::string name = std::string(251, 'a') + ".wav";
    const auto source = root / "source" / name;
    {
        std::ofstream file(source, std::ios::binary);
        if (!file) {
            MESSAGE("this filesystem/path cannot hold a 255-byte filename");
            return;
        }
        file << "complete media bytes";
        REQUIRE(file.good());
    }
    const auto target = root / "target";
    SUBCASE("import") {
        bool created = false;
        const auto copy = detail::copyIntoFolder(source, target, &created);
        REQUIRE_MESSAGE(copy, (copy ? "" : copy.error().toString()));
        CHECK(*copy == name);
        CHECK(created);
        REQUIRE(detail::copyIntoFolder(source, target, &created));
        CHECK_FALSE(created);
    }
    SUBCASE("save as") { REQUIRE(detail::copyFolder(source.parent_path(), target)); }
    CHECK(fs::file_size(target / name) == fs::file_size(source));
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(target)) {
        CHECK(entry.path().filename() == name);
        ++entries;
    }
    CHECK(entries == 1);
}

TEST_CASE("media folder copy excludes its destination subtree and resolved aliases") {
    const auto root = supple::test::uniqueTempDir("engine-media-copy-");
    struct Cleanup {
        fs::path path;
        fs::path cwd = fs::current_path();
        ~Cleanup() {
            std::error_code ec;
            fs::current_path(cwd, ec);
            fs::remove_all(path, ec);
        }
    } cleanup{root};
    const auto source = root / "media";
    const auto destination = source / "new" / "media";
    fs::create_directories(source / "rec");
    std::ofstream(source / "tone.wav") << "tone";
    std::ofstream(source / "rec" / "take.wav") << "take";
    auto target = destination;
    SUBCASE("existing destination") {
        fs::create_directories(destination);
        std::ofstream(destination / "existing.wav") << "keep";
    }
    SUBCASE("destination created during copying") {}
    SUBCASE("relative destination with no existing path components") {
        fs::current_path(source);
        target = fs::path("new") / "media";
    }
    SUBCASE("destination through a directory symlink") {
        std::error_code ec;
        fs::create_directory_symlink(source, root / "alias", ec);
        if (ec) {
            MESSAGE("directory symlink unavailable: " << ec.message());
            return;
        }
        target = root / "alias" / "new" / "media";
        fs::create_directories(target);
    }
    const auto copied = detail::copyFolder(source, target);
    REQUIRE(copied);
    CHECK(fs::exists(destination / "tone.wav"));
    CHECK(fs::exists(destination / "rec" / "take.wav"));
    CHECK_FALSE(fs::exists(destination / "new"));
    // A repeated Save As keeps identical files and still excludes the destination.
    REQUIRE(detail::copyFolder(source, target));
    CHECK_FALSE(fs::exists(destination / "new"));
    CHECK(fs::file_size(destination / "tone.wav") == 4);
    CHECK(fs::file_size(destination / "rec" / "take.wav") == 4);
    // Record in the nested project, then Save As back to the original folder.
    // An ancestor destination must receive new media, not exclude the source.
    std::ofstream(destination / "rec" / "new-take.wav") << "new take";
    REQUIRE(detail::copyFolder(target, source));
    REQUIRE(fs::exists(source / "rec" / "new-take.wav"));
    CHECK(fs::file_size(source / "rec" / "new-take.wav") == 8);
}

TEST_CASE("crash recovery reads regular files only, and never waits on a named pipe") {
    // Round-9 review: a sidecar that was a named pipe parked recovery in
    // open() until something wrote to it. Every recovery read (autosaves,
    // kept copies, sidecars, MIDI journals) goes through these helpers.
    const fs::path root = fs::temp_directory_path() /
                          ("supple-recovery-files-" + std::to_string(supple::test::processId()));
    fs::remove_all(root);
    fs::create_directories(root / "folder");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    std::ofstream(root / "autosave-1.json") << "{}";
    CHECK(detail::readRecoveryText(root, root / "autosave-1.json", 1 << 20).value() == "{}");
    CHECK(detail::readRecoveryFile(root, root / "autosave-1.json", 1 << 20)->size() == 2);
    // The codes the two readers gave; read on a thread when a pipe could hold it.
    auto codes = [&](const fs::path& file, const fs::path& confine) {
        auto text = detail::readRecoveryText(confine, file, 1 << 20);
        auto bytes = detail::readRecoveryFile(confine, file, 1 << 20);
        return std::pair(text ? std::string("read") : text.error().code + ":" + text.error().hint,
                         bytes ? std::string("read") : bytes.error().code);
    };
    auto refused = [&](const std::pair<std::string, std::string>& got) {
        CHECK(got.first.starts_with("recover.not_file:"));
        CHECK(got.first.size() > std::string("recover.not_file:").size());  // a hint
        CHECK(got.second == "recover.not_file");
    };
    refused(codes(root / "folder", root));
#if !defined(_WIN32)
    const fs::path pipe = root / "take.rec.json";
    REQUIRE(::mkfifo(pipe.c_str(), 0600) == 0);
    fs::create_symlink(pipe, root / "link.rec.json");
    for (const fs::path& file : {pipe, root / "link.rec.json"})
        for (const fs::path& confine : {root, fs::path()}) {
            INFO(file.string(), " confined to ", confine.string());
            std::pair<std::string, std::string> got;
            CHECK_FALSE(supple::test::waitsOnFifo(pipe, [&] { got = codes(file, confine); }));
            refused(got);
        }
    CHECK(fs::is_fifo(pipe));  // left as it was
#endif
}

TEST_CASE("import and save reopen accept a recording promoted to RF64") {
    using namespace supple;
    const auto root =
        fs::temp_directory_path() / ("supple-rf64-import-" + std::to_string(test::processId()));
    fs::remove_all(root);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    fs::create_directories(root / "source");
    const auto wave = root / "source" / "take.wav";
    audio::WavStreamWriter writer;
    REQUIRE(writer.open(wave, 48000, 2));
    audio::WavStreamWriterTestAccess::riffLimit(writer, 256);
    std::vector<float> data(128);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<float>(i) / 128;
    REQUIRE(writer.append(data.data(), 64));
    REQUIRE(writer.close());
    const auto expected = audio::readAudio(wave);
    REQUIRE(expected);
    auto created = EngineApp::create({.workdir = root / "project"});
    REQUIRE(created);
    auto& app = **created;
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, app);
    REQUIRE(dispatcher.execute(
        {"track.add", *json::parse(R"({"name":"Audio"})"), {cmd::Origin::Type::Test, "rf64", ""}}));
    auto args = *json::parse(R"({"track":"tr_1","position":0})");
    args["audio_file"] = wave.string();
    auto added = dispatcher.execute({"item.add", args, {cmd::Origin::Type::Test, "rf64", ""}});
    REQUIRE_MESSAGE(added, (added ? "" : added.error().toString()));
    REQUIRE(app.head().media.size() == 1);
    CHECK(app.head().media.begin()->value->frames == 64);
    CHECK(app.head().media.begin()->value->channels == 2);
    const auto saved = root / "project" / "song.json";
    REQUIRE(app.save(saved));
    REQUIRE(app.open(saved));
    CHECK(app.head().media.begin()->value->frames == 64);
    const auto copy = app.mediaDir() / app.head().media.begin()->value->path;
    const auto decoded = audio::readAudio(copy);
    REQUIRE(decoded);
    CHECK(decoded->channels == expected->channels);
    auto bytes = readFile(copy);
    REQUIRE(bytes);
    CHECK(std::string(reinterpret_cast<const char*>(bytes->data()), 4) == "RF64");
    CHECK(sha256File(wave).value() == sha256File(copy).value());
}
