#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include "r3dss/core/concurrency.hpp"
#include "r3dss/core/expected.hpp"
#include "r3dss/core/hardware.hpp"
#include "r3dss/core/sha256.hpp"
#include "r3dss/core/types.hpp"

using namespace r3dss;

TEST_CASE("Expected propagates values and errors") {
    Expected<int> ok = 5;
    REQUIRE(ok);
    REQUIRE(*ok == 5);
    Expected<int> bad = makeError(ErrorCode::Io, "boom");
    REQUIRE_FALSE(bad);
    REQUIRE(bad.error().code == ErrorCode::Io);
    auto f = [](bool fail) -> Expected<int> {
        R3DSS_TRY_ASSIGN(v, fail ? Expected<int>(makeError(ErrorCode::Format, "x")) : Expected<int>(3));
        return v * 2;
    };
    REQUIRE(*f(false) == 6);
    REQUIRE(f(true).error().code == ErrorCode::Format);
}

TEST_CASE("CFA helpers") {
    REQUIRE(cfaColorAt(CfaPattern::RGGB, 0, 0) == 0);
    REQUIRE(cfaColorAt(CfaPattern::RGGB, 1, 0) == 1);
    REQUIRE(cfaColorAt(CfaPattern::RGGB, 1, 1) == 2);
    REQUIRE(cfaColorAt(CfaPattern::BGGR, 0, 0) == 2);
    REQUIRE(*cfaFromName("gbrg") == CfaPattern::GBRG);
}

TEST_CASE("Timecode round trips, including drop frame") {
    Rational fps{24, 1};
    for (int64_t n : {0LL, 1LL, 23LL, 24LL, 86399LL, 123456LL}) {
        Timecode tc = Timecode::fromFrames(n, fps, false);
        REQUIRE(tc.toFrames() == n);
    }
    REQUIRE(Timecode::fromFrames(24 * 3600 + 24 * 60 * 2 + 24 * 3 + 4, fps, false).toString() == "01:02:03:04");
    Rational df{30000, 1001};
    for (int64_t n : {0LL, 1LL, 1798LL, 1799LL, 1800LL, 17982LL, 17983LL, 17984LL, 100000LL}) {
        Timecode tc = Timecode::fromFrames(n, df, true);
        INFO(tc.toString());
        REQUIRE(tc.toFrames() == n);
    }
    REQUIRE(Timecode::fromFrames(1800, df, true).toString() == "00:01:00;02");
    auto p = Timecode::parse("10:20:30:12", fps);
    REQUIRE(p);
    REQUIRE(p->toString() == "10:20:30:12");
    REQUIRE(p->plusFrames(12).toString() == "10:20:31:00");
}

TEST_CASE("SHA-256 known answer") {
    REQUIRE(Sha256::hashBytes("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(Sha256::hashBytes("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::string million(1000000, 'a');
    REQUIRE(Sha256::hashBytes(million.data(), million.size()) ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("BoundedQueue blocks, drains and cancels") {
    auto tok = std::make_shared<CancellationToken>();
    BoundedQueue<int> q(2, tok);
    REQUIRE(q.push(1));
    REQUIRE(q.push(2));
    std::thread t([&] { q.push(3); });
    REQUIRE(*q.pop() == 1);
    t.join();
    q.close();
    REQUIRE(*q.pop() == 2);
    REQUIRE(*q.pop() == 3);
    REQUIRE_FALSE(q.pop().has_value());
    BoundedQueue<int> q2(1, tok);
    REQUIRE(q2.push(1));
    std::thread t2([&] { tok->cancel(); });
    REQUIRE_FALSE(q2.push(2)); // blocked until cancel
    t2.join();
}

TEST_CASE("ThreadPool parallelFor covers every index once") {
    ThreadPool pool(4);
    std::vector<int> hits(1000, 0);
    pool.parallelFor(hits.size(), [&](size_t i) { hits[i]++; });
    for (int h : hits) REQUIRE(h == 1);
    auto fut = pool.submit([] { return 42; });
    REQUIRE(fut.get() == 42);
}

TEST_CASE("Thread plan follows hardware policy") {
    HardwareInfo hw;
    hw.logicalCores = 32;
    hw.totalRamBytes = 64ull << 30;
    ThreadPlan p = planThreads(hw, 8256ull * 4644 * 2);
    REQUIRE(p.parallelClips == 4);
    REQUIRE(p.encodeWorkers == 30);
    REQUIRE(p.resampleWorkers == 16);
    hw.cudaAvailable = true;
    REQUIRE(planThreads(hw, 0).resampleWorkers == 2);
    hw.logicalCores = 4;
    hw.totalRamBytes = 1ull << 30; // tiny RAM limits clips to 1
    REQUIRE(planThreads(hw, 8256ull * 4644 * 2).parallelClips == 1);
    REQUIRE(planThreads(hw, 0, 3).parallelClips == 3);
}

#include "r3dss/core/paths.hpp"

TEST_CASE("UTF-8 path helpers round-trip non-ASCII names") {
    const std::string name = "Drehtag \xC3\x84 / \xE6\x92\xAE\xE5\xBD\xB1 \xF0\x9F\x8E\xAC"; // "Drehtag Ä / 撮影 🎬"
    const std::filesystem::path p = fromUtf8("root") / fromUtf8("Drehtag \xC3\x84") / fromUtf8("\xE6\x92\xAE\xE5\xBD\xB1.dng");
    REQUIRE(toUtf8(p.filename()) == "\xE6\x92\xAE\xE5\xBD\xB1.dng");
    REQUIRE(toUtf8(fromUtf8(name)) == name);
    REQUIRE(toUtf8Generic(p) == "root/Drehtag \xC3\x84/\xE6\x92\xAE\xE5\xBD\xB1.dng");
    REQUIRE(fromUtf8(toUtf8(p)) == p);
}

TEST_CASE("Cancel registrations unregister on destruction; cancel is idempotent") {
    auto tok = std::make_shared<CancellationToken>();
    int calls = 0;
    {
        CancelRegistration r = tok->onCancel([&] { ++calls; });
        REQUIRE(tok->callbackCount() == 1);
    }
    REQUIRE(tok->callbackCount() == 0);
    {
        // A queue registers `this` on the token; destroying the queue must remove it so a later
        // cancel never calls into freed memory.
        BoundedQueue<int> q(2, tok);
        REQUIRE(tok->callbackCount() == 1);
    }
    REQUIRE(tok->callbackCount() == 0);
    CancelRegistration live = tok->onCancel([&] { ++calls; });
    tok->cancel();
    tok->cancel();
    REQUIRE(calls == 1);
    CancelRegistration late = tok->onCancel([&] { ++calls; }); // already cancelled: runs immediately
    REQUIRE(calls == 2);
}

TEST_CASE("A paused wait wakes when its token is cancelled, without resume()") {
    PauseGate gate;
    gate.pause();
    auto tok = std::make_shared<CancellationToken>();
    std::atomic<bool> returned{false};
    bool result = true;
    std::thread t([&] {
        result = gate.wait(tok.get());
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(returned.load());
    tok->cancel();
    t.join();
    REQUIRE(returned.load());
    REQUIRE_FALSE(result);
    REQUIRE(tok->callbackCount() == 0);
}
