// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/files.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace supple;

namespace {
std::vector<std::uint8_t> bytesOf(std::string_view s) { return {s.begin(), s.end()}; }
}  // namespace

TEST_CASE("files: durable checkpoints flush streams and report path barrier failures") {
    const auto dir = fs::temp_directory_path() / ("supple-sync-" + std::to_string(getpid()));
    REQUIRE(fs::create_directory(dir));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            setDurabilityHookForTesting({});
            fs::remove_all(path);
        }
    } cleanup{dir};
    const auto file = dir / "data.bin";
#ifdef _WIN32
    auto* stream = _wfopen(file.c_str(), L"w+b");
#else
    auto* stream = std::fopen(file.c_str(), "w+b");
#endif
    REQUIRE(stream);
    REQUIRE(std::fwrite("payload", 1, 7, stream) == 7);
    CHECK(syncFileStream(stream));
    REQUIRE(std::fclose(stream) == 0);
    CHECK(*readTextFile(file) == "payload");
    CHECK_FALSE(syncFileStream(nullptr));
    const auto missing = syncFile(dir / "missing");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "file.sync");
    CHECK_FALSE(syncFile(dir));
    std::vector<fs::path> observed;
    setDurabilityHookForTesting([&](const fs::path& path, bool) {
        observed.push_back(path);
        return true;
    });
    REQUIRE(syncFile(file));
    REQUIRE(syncParentDirectories(file));
    REQUIRE(observed.size() >= 2);
    CHECK(observed[0] == file);
    CHECK(observed[1] == fs::canonical(dir));
    setDurabilityHookForTesting([&](const fs::path& path, bool) { return path != file; });
    const auto failed = syncFile(file);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "file.sync");
    setDurabilityHookForTesting([&](const fs::path& path, bool) { return path != dir; });
    const auto failedDirectory = syncParentDirectories(file);
    REQUIRE_FALSE(failedDirectory);
    CHECK(failedDirectory.error().code == "file.sync_directory");
    CHECK_FALSE(failedDirectory.error().hint.empty());
    CHECK(*readTextFile(file) == "payload");
}

TEST_CASE("files: parent checkpoints resolve symlinks before dot segments") {
    const auto root = fs::temp_directory_path() / ("supple-sync-alias-" + std::to_string(getpid()));
    REQUIRE(fs::create_directory(root));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            setDurabilityHookForTesting({});
            fs::remove_all(path);
        }
    } cleanup{root};
    fs::create_directories(root / "real" / "child");
    std::error_code ec;
    fs::create_directory_symlink(root / "real" / "child", root / "alias", ec);
    if (ec) {
        MESSAGE("directory symlink unavailable: " << ec.message());
        return;
    }
    const auto file = root / "alias" / ".." / "data.bin";
    std::ofstream(file) << "payload";
    REQUIRE(fs::exists(root / "real" / "data.bin"));
    std::vector<fs::path> directories;
    setDurabilityHookForTesting([&](const fs::path& path, bool directory) {
        if (directory) directories.push_back(path);
        return true;
    });
    REQUIRE(syncParentDirectories(file));
    REQUIRE_FALSE(directories.empty());
    CHECK(directories.front() == fs::canonical(root / "real"));
}

TEST_CASE("base64: RFC 4648 test vectors round-trip") {
    const std::pair<const char*, const char*> vectors[] = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (const auto& [plain, encoded] : vectors) {
        const auto b = bytesOf(plain);
        CHECK(base64Encode(b) == encoded);
        auto d = base64Decode(encoded);
        REQUIRE(d);
        CHECK(*d == b);
    }
    std::vector<std::uint8_t> all(256);
    for (int i = 0; i < 256; ++i) all[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    auto d = base64Decode(base64Encode(all));
    REQUIRE(d);
    CHECK(*d == all);
}

TEST_CASE("base64: rejects malformed input") {
    for (const char* bad : {"Zg=", "Zg=a", "Z===", "Zm9v\n", "Zm-v", "Zh==", "Zm9=Zm9v"}) {
        INFO(bad);
        auto d = base64Decode(bad);
        REQUIRE(!d.ok());
        CHECK(d.error().code == "base64.invalid");
    }
}

TEST_CASE("files: atomic write, read back, errors") {
    const fs::path dir =
        fs::temp_directory_path() / ("supple-files-test-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const fs::path p = dir / "sub" / "a.txt";
    REQUIRE(writeFileAtomic(p, std::string_view("hello")));
    CHECK(!fs::exists(p.string() + ".tmp"));
    auto t = readTextFile(p);
    REQUIRE(t);
    CHECK(*t == "hello");
    REQUIRE(writeFileAtomic(p, std::string_view("")));
    CHECK(readTextFile(p).value() == "");
    CHECK(readFile(p, 0));
    REQUIRE(writeFileAtomic(p, std::string_view("12345")));
    CHECK(readFile(p, 4).error().code == "file.too_large");
    CHECK(readFile(dir / "missing").error().code == "file.open");
    // Concurrent writers of one target: every write succeeds, the result is
    // one writer's complete content, and no temporary files are left.
    {
        std::vector<std::thread> ts;
        std::atomic<int> failures{0};
        for (int w = 0; w < 4; ++w)
            ts.emplace_back([&, w] {
                const std::string body(1000, static_cast<char>('a' + w));
                for (int i = 0; i < 50; ++i)
                    if (!writeFileAtomic(p, std::string_view(body)).ok()) ++failures;
            });
        for (auto& th : ts) th.join();
        CHECK(failures == 0);
        const std::string got = readTextFile(p).value();
        REQUIRE(got.size() == 1000);
        CHECK(got == std::string(1000, got[0]));
        int files = 0;
        for (const auto& e : fs::directory_iterator(p.parent_path())) {
            (void)e;
            ++files;
        }
        CHECK(files == 1);
    }
    CHECK(
        writeFileAtomic(dir / "sub" / "a.txt" / "not-a-dir", std::string_view("x")).error().code ==
        "file.write");
    fs::remove_all(dir);
}

TEST_CASE("files: native Unicode paths and binary bytes survive replacement") {
    const fs::path dir =
        fs::temp_directory_path() / ("supple-native-" + std::to_string(::getpid()));
    const fs::path path = dir / fs::path(std::u8string(u8"音声.wav"));
    const std::vector<std::uint8_t> first{0, 10, 13, 26, 255};
    const std::vector<std::uint8_t> second{26, 13, 10, 0};
    REQUIRE(writeFileAtomic(path, first));
    REQUIRE(readFile(path));
    CHECK(*readFile(path) == first);
    REQUIRE(writeFileAtomic(path, second));
    REQUIRE(readFile(path));
    CHECK(*readFile(path) == second);
    fs::remove_all(dir);
}

TEST_CASE("files: atomic replacement preserves an existing reader") {
    const fs::path dir =
        fs::temp_directory_path() / ("supple-reader-" + std::to_string(::getpid()));
    const fs::path path = dir / "data";
    REQUIRE(writeFileAtomic(path, std::string_view("old")));
    {
#ifdef _WIN32
        struct Reader {
            HANDLE handle;
            ~Reader() { ::CloseHandle(handle); }
        } reader{::CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        REQUIRE(reader.handle != INVALID_HANDLE_VALUE);
        const auto replaced = writeFileAtomic(path, std::string_view("new"));
        INFO((replaced ? "ok" : replaced.error().message));
        REQUIRE(replaced);
        char old[3];
        DWORD count = 0;
        REQUIRE(::ReadFile(reader.handle, old, sizeof(old), &count, nullptr));
        const std::string previous(old, count);
#else
        std::ifstream reader(path, std::ios::binary);
        REQUIRE(reader.good());
        REQUIRE(writeFileAtomic(path, std::string_view("new")));
        const std::string previous((std::istreambuf_iterator<char>(reader)),
                                   std::istreambuf_iterator<char>());
#endif
        CHECK(previous == "old");
        CHECK(readTextFile(path).value() == "new");
    }
    fs::remove_all(dir);
}

TEST_CASE("files: Unicode failure paths stay structured") {
    const fs::path dir =
        fs::temp_directory_path() / ("supple-unicode-errors-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{dir};
    const auto name = std::u8string(u8"missing-音");
    const fs::path missing = dir / fs::path(name);
    auto read = readFile(missing);
    REQUIRE_FALSE(read);
    CHECK(read.error().code == "file.open");
    CHECK(read.error().message.find(std::string(name.begin(), name.end())) != std::string::npos);
    fs::create_directory(missing);
    auto written = writeFileAtomic(missing, "cannot replace a directory");
    REQUIRE_FALSE(written);
    CHECK(written.error().code == "file.write");
    CHECK(written.error().message.find(std::string(name.begin(), name.end())) != std::string::npos);
}

#ifdef _WIN32
namespace {
// A handle that denies delete sharing, as a viewer, indexer or scanner might
// hold the file.
struct DenyDeleteHolder {
    HANDLE handle;
    explicit DenyDeleteHolder(const fs::path& path)
        : handle(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    void release() {
        if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
    ~DenyDeleteHolder() { release(); }
};
}  // namespace

TEST_CASE("files: a brief hold without delete sharing delays the replace, then it succeeds") {
    const fs::path dir = fs::temp_directory_path() / ("supple-busy-" + std::to_string(::getpid()));
    const fs::path path = dir / "data";
    REQUIRE(writeFileAtomic(path, std::string_view("old")));
    DenyDeleteHolder holder(path);
    REQUIRE(holder.handle != INVALID_HANDLE_VALUE);
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        holder.release();
    });
    const auto replaced = writeFileAtomic(path, std::string_view("new"));
    releaser.join();
    INFO((replaced ? "ok" : replaced.error().message));
    REQUIRE(replaced);
    CHECK(readTextFile(path).value() == "new");
    std::error_code ec;
    int files = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        (void)e;
        ++files;
    }
    CHECK(files == 1);
    fs::remove_all(dir, ec);
}

TEST_CASE("files: a lasting hold without delete sharing is a structured error") {
    const fs::path dir = fs::temp_directory_path() / ("supple-held-" + std::to_string(::getpid()));
    const fs::path path = dir / "data";
    REQUIRE(writeFileAtomic(path, std::string_view("old")));
    {
        DenyDeleteHolder holder(path);
        REQUIRE(holder.handle != INVALID_HANDLE_VALUE);
        const auto replaced = writeFileAtomic(path, std::string_view("new"));
        REQUIRE_FALSE(replaced);
        const Error& e = replaced.error();
        CHECK(e.code == "file.write");
        CHECK(e.message.find("another program has the file open") != std::string::npos);
        CHECK(e.path.find("data") != std::string::npos);
        CHECK(e.hint.find("close the program") != std::string::npos);
        // The previous content stays and no temporary file is left behind.
        holder.release();
        CHECK(readTextFile(path).value() == "old");
        int files = 0;
        for (const auto& entry : fs::directory_iterator(dir)) {
            (void)entry;
            ++files;
        }
        CHECK(files == 1);
    }
    // Once released the same save works.
    CHECK(writeFileAtomic(path, std::string_view("new")));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("files: retryWhileFileBusy gives up at once on other errors") {
    unsigned long error = 0;
    int attempts = 0;
    const bool ok = retryWhileFileBusy(
        fs::temp_directory_path(),
        [&] {
            ++attempts;
            ::SetLastError(ERROR_DISK_FULL);
            return false;
        },
        error);
    CHECK_FALSE(ok);
    CHECK(attempts == 1);
    CHECK(error == ERROR_DISK_FULL);
    // Access denied on a directory is not a sharing problem either.
    attempts = 0;
    CHECK_FALSE(retryWhileFileBusy(
        fs::temp_directory_path(),
        [&] {
            ++attempts;
            ::SetLastError(ERROR_ACCESS_DENIED);
            return false;
        },
        error));
    CHECK(attempts == 1);
}
#endif

TEST_CASE("writeFileNew publishes only under a free name, synced, never replacing") {
    const fs::path dir = fs::temp_directory_path() / ("supple-new-" + std::to_string(getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "copy.bak";
    REQUIRE(writeFileNew(file, bytesOf("first")));
    CHECK(*readTextFile(file) == "first");
    auto again = writeFileNew(file, bytesOf("second"));
    REQUIRE_FALSE(again);
    CHECK(again.error().code == "file.exists");
    CHECK(*readTextFile(file) == "first");

    // Taken between the sync and the publication (another writer chose the
    // same free name): still refused, the other writer's file kept.
    const fs::path raced = dir / "raced.bak";
    std::vector<fs::path> synced;
    setFileSyncHookForTesting([&](const fs::path& p) {
        synced.push_back(p);
        if (p == raced) std::ofstream(raced) << "other";
        return true;
    });
    auto lost = writeFileNew(raced, bytesOf("mine"));
    setFileSyncHookForTesting({});
    REQUIRE_FALSE(lost);
    CHECK(lost.error().code == "file.exists");
    CHECK(*readTextFile(raced) == "other");
    CHECK(synced == std::vector<fs::path>{raced});

    // A failed sync publishes nothing.
    setFileSyncHookForTesting([](const fs::path&) { return false; });
    auto unsynced = writeFileNew(dir / "never.bak", bytesOf("x"));
    setFileSyncHookForTesting({});
    REQUIRE_FALSE(unsynced);
    CHECK_FALSE(fs::exists(dir / "never.bak"));
    std::size_t files = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir)) ++files;
    CHECK(files == 2);  // no temporary file left behind
    fs::remove_all(dir);
}
