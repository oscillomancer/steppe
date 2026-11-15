// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "../../../tests/file_platform.h"
#include "../src/media_files.h"
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace supple::engine;

TEST_CASE("media folder copy excludes its destination subtree and resolved aliases") {
    const auto root = fs::temp_directory_path() /
                      ("engine-media-copy-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
