// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <thread>
#include "landscape/LandscapeQuery.h"

using namespace cc::landscape;
using Status = LandscapeQueryStatus;

void require(bool ok, const char *message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void close(float a, float b, const char *message) {
    require(std::abs(a - b) < 1e-3F, message);
}

LandscapeData data() {
    LandscapeData d;
    d.sectorsX = 2;
    d.sectorsZ = 1;
    d.maxLevel = 1;
    d.minTileLevel = 0;
    d.tileResolution = 3;
    d.sectorSize = 4;
    d.heightScale = 65535;
    d.heightBias = -100;
    return d;
}
LandscapeQueryTile tile(uint32_t tx, uint32_t tz) {
    LandscapeQueryTile t;
    t.height.resize(18);
    t.splat.resize(18);
    t.normal.resize(27);
    for (uint32_t z = 0; z < 3; ++z) {
        for (uint32_t x = 0; x < 3; ++x) {
            const uint16_t h = static_cast<uint16_t>(100 + (tx * 2 + x) * 100 + (tz * 2 + z) * 50);
            const size_t i = z * 3 + x;
            t.height[i * 2] = h >> 8;
            t.height[i * 2 + 1] = h & 255;
            const uint16_t s = 3U | (7U << 5U) | (10U << 10U);
            std::memcpy(t.splat.data() + i * 2, &s, 2);
            // Deliberately different from the height gradient: queries must use RGB.
            t.normal[i * 3] = 64;
            t.normal[i * 3 + 1] = 220;
            t.normal[i * 3 + 2] = 160;
        }
    }
    return t;
}
struct Loading {
    struct Job {
        uint32_t x;
        uint32_t z;
        LandscapeQuery::Completion complete;
    };
    std::deque<Job> jobs;
    size_t requests{0};
    LandscapeQuery::Loader loader() {
        return [this](uint32_t x, uint32_t z, LandscapeQuery::Completion done) {
            ++requests;
            jobs.push_back({x, z, std::move(done)});
        };
    }
    void finish(bool ok = true) {
        auto job = std::move(jobs.front());
        jobs.pop_front();
        job.complete(tile(job.x, job.z), ok);
    }
    void drain(LandscapeQuery &query) {
        query.update();
        for (int i = 0; i < 20 && !jobs.empty(); ++i) {
            while (!jobs.empty()) {
                finish();
            }
            query.update();
        }
    }
};

void sampling() {
    Loading loading;
    LandscapeQuery q(data(), loading.loader(), 8);
    require(q.sample({4, 2}).status == Status::NOT_READY, "cold query must not load");
    require(loading.requests == 0, "query triggered I/O");
    require(q.setSource(1, {4, 2}, 4) == Status::NOT_READY, "preload should be pending");
    q.update();
    require(loading.jobs.size() == 2, "decode concurrency must be bounded");
    loading.drain(q);
    require(q.sourceStatus(1) == Status::HIT, "region ready");
    auto hit = q.sample({0.75F, 0.25F});
    require(hit.status == Status::HIT, "sample hit");
    close(hit.position.y, 87.5F, "bilinear height and height bias");
    const float length = std::sqrt(127.0F * 127.0F + 185.0F * 185.0F + 65.0F * 65.0F);
    close(hit.normal.x, -127.0F / length, "source normal X");
    close(hit.normal.y, 185.0F / length, "source normal Y");
    close(hit.normal.z, 65.0F / length, "source normal Z");
    require(hit.surfaceType == 3, "dominant material");
    close(hit.surfaceWeight, 21.0F / 31.0F, "material weight");
    close(q.sample({8, 4}).position.y, 1000, "inclusive far corner");
    close(q.sample({0, 0}).position.y, 0, "inclusive near corner");
    close(q.sample({2, 2}).position.y, 300, "tile/sector boundary");
    close(q.sample({1.999999F, 2}).position.y, q.sample({2.000001F, 2}).position.y, "tile seam");
    require(q.sample({8.01F, 4}).status == Status::MISS, "outside should miss");
    const auto loaded = loading.requests;
    q.removeSource(1);
    require(q.sample({4, 2}).status == Status::HIT, "unpinned data retained until evicted");
    q.update();
    require(loading.requests == loaded, "queries must not schedule new work");
}

void budgetAndSharing() {
    Loading loading;
    LandscapeQuery q(data(), loading.loader(), 2);
    q.setSource(1, {1, 1}, 0);
    q.setSource(2, {1, 1}, 0);
    q.setSource(3, {3, 1}, 0);
    loading.drain(q);
    require(loading.requests == 2, "overlapping sources share decoding");
    require(q.setSource(4, {5, 1}, 0) == Status::OUT_OF_CAPACITY, "pinned capacity must reject admission");
    q.update();
    require(loading.requests == 2, "rejected source must not trigger I/O");
    q.removeSource(1);
    require(q.setSource(4, {5, 1}, 0) == Status::OUT_OF_CAPACITY, "second owner must still protect tile");
    q.removeSource(2);
    require(q.setSource(4, {5, 1}, 0) == Status::NOT_READY, "released capacity admits pending source");
    loading.drain(q);
    require(q.sample({1, 1}).status == Status::NOT_READY, "unpinned old tile evicted");
    require(q.sample({3, 1}).status == Status::HIT, "other source preserved");
    require(q.sample({5, 1}).status == Status::HIT, "new source admitted");
    require(q.setSource(9, {4, 2}, 100000) == Status::OUT_OF_CAPACITY, "huge region bounded");
    q.update();
    require(loading.requests == 3, "oversized region must not trigger I/O");
    require(q.setSource(9, {5, 1}, 0) == Status::HIT, "shrunk region shares resident tiles at capacity");
    require(q.setSource(9, {4, 2}, -1) == Status::ERROR, "negative radius rejected");
    require(q.setSource(9, {103, 101}, 1) == Status::MISS, "non-overlapping region");
}

void completionLifetime() {
    Loading loading;
    LandscapeQuery q(data(), loading.loader(), 1);
    q.setSource(1, {1, 1}, 0);
    q.update();
    q.setSource(1, {5, 1}, 0);
    q.update();
    require(loading.jobs.size() == 2, "two outstanding generations");
    loading.finish();
    q.update();
    require(q.sample({5, 1}).status == Status::NOT_READY, "stale tile must not publish");
    std::thread worker([&loading] {
        loading.finish();
    });
    worker.join();
    q.update();
    require(q.sample({5, 1}).status == Status::HIT, "worker result published on update");
    {
        LandscapeQuery temporary(data(), loading.loader(), 1);
        temporary.setSource(1, {1, 1}, 0);
        temporary.update();
    }
    loading.finish(); // Must safely drop results after destruction.
}

void failureAndSplat() {
    Loading loading;
    LandscapeQuery q(data(), loading.loader(), 1);
    q.setSource(1, {1, 1}, 0);
    q.update();
    loading.finish(false);
    q.update();
    require(q.sourceStatus(1) == Status::ERROR && q.sample({1, 1}).status == Status::ERROR, "failed load distinguished from miss");
    q.removeSource(1);
    q.setSource(1, {1, 1}, 0);
    loading.drain(q);
    require(q.sourceStatus(1) == Status::HIT, "new registration retries failed data");
    Loading splatLoader;
    LandscapeQuery splat(data(), splatLoader.loader(), 1);
    splat.setSource(1, {1, 1}, 0);
    splat.update();
    auto t = tile(0, 0);
    for (const auto &pair : {std::pair<size_t, uint16_t>{0, 3}, {1, 7}, {3, 11}, {4, 13}}) {
        std::memcpy(t.splat.data() + pair.first * 2, &pair.second, 2);
    }
    splatLoader.jobs.front().complete(std::move(t), true);
    splatLoader.jobs.pop_front();
    splat.update();
    const auto hit = splat.sample({0.75F, 0.25F});
    require(hit.surfaceType == 7, "splat triangle interpolation");
    close(hit.surfaceWeight, 0.5F, "splat spatial weights");
}

void triangleHeight() {
    Loading loading;
    LandscapeQuery q(data(), loading.loader(), 1);
    q.setSource(1, {1, 1}, 0);
    q.update();
    auto t = tile(0, 0);
    for (const auto &pair : {std::pair<size_t, uint16_t>{0, 100}, {1, 102}, {3, 104}, {4, 110}}) {
        t.height[pair.first * 2] = pair.second >> 8;
        t.height[pair.first * 2 + 1] = pair.second & 255;
    }
    loading.jobs.front().complete(std::move(t), true);
    loading.jobs.pop_front();
    q.update();
    close(q.sample({0.5F, 0.5F}).position.y, 3, "B-C diagonal, not bilinear or A-D");
    close(q.sample({0.2F, 0.3F}).position.y, 1.6F, "first triangle");
    close(q.sample({0.8F, 0.7F}).position.y, 6.4F, "second triangle");
}

void holes() {
    Loading loading;
    LandscapeQuery q(data(), loading.loader(), 1);
    q.setSource(1, {1, 1}, 0);
    q.update();
    auto t = tile(0, 0);
    uint16_t hole = 0x8000U | 3U | (7U << 5U) | (10U << 10U);
    std::memcpy(t.splat.data(), &hole, 2);
    loading.jobs.front().complete(std::move(t), true);
    loading.jobs.pop_front();
    q.update();
    require(q.sourceStatus(1) == Status::HIT, "hole does not make residency unready");
    require(q.sample({0.25F, 0.25F}).status == Status::MISS, "hole cell misses");
    require(q.sample({0.99F, 0.99F}).status == Status::MISS, "both triangles in hole miss");
    require(q.sample({1.25F, 0.25F}).status == Status::HIT, "adjacent solid cell survives");
    close(q.sample({1.25F, 0.25F}).surfaceWeight, 21.0F / 31.0F, "hole bit is not material weight");
}

void malformedAndRapidMovement() {
    for (uint32_t channel = 0; channel < 3; ++channel) {
        for (int delta : {-1, 1}) {
            Loading loading;
            LandscapeQuery q(data(), loading.loader(), 1);
            q.setSource(1, {1, 1}, 0);
            q.update();
            auto damaged = tile(0, 0);
            auto &bytes = channel == 0 ? damaged.height : channel == 1 ? damaged.splat
                                                                       : damaged.normal;
            bytes.resize(bytes.size() + delta);
            loading.jobs.front().complete(std::move(damaged), true);
            loading.jobs.pop_front();
            q.update();
            require(q.sourceStatus(1) == Status::ERROR, "wrong decoded channel size must fail");
            require(q.sample({1, 1}).status == Status::ERROR, "malformed bytes must not be sampled");
            q.removeSource(1);
            q.setSource(1, {1, 1}, 0);
            loading.drain(q);
            require(q.sourceStatus(1) == Status::HIT, "valid retry after malformed decode");
        }
    }
    Loading loading;
    LandscapeQuery q(data(), loading.loader(), 1);
    for (uint32_t i = 0; i < 1000; ++i) {
        q.setSource(1, {float((i % 4) * 2 + 1), 1}, 0);
        q.update();
        require(loading.jobs.size() <= 2, "rapid movement exceeded decode concurrency");
        if (i % 7 == 0 && !loading.jobs.empty()) {
            // Finish newest first to exercise out-of-order delivery.
            auto job = std::move(loading.jobs.back());
            loading.jobs.pop_back();
            job.complete(tile(job.x, job.z), true);
        }
    }
    q.setSource(1, {7, 1}, 0);
    loading.drain(q);
    require(q.sourceStatus(1) == Status::HIT, "latest source must recover after rapid cancellation");
    close(q.sample({7, 1}).position.y, 750, "latest generation height");
    require(q.sample({-0.001F, 0}).status == Status::MISS, "near negative X edge");
    require(q.sample({0, -0.001F}).status == Status::MISS, "near negative Z edge");
    require(q.sample({8.001F, 4}).status == Status::MISS, "near positive X edge");
    require(q.sample({8, 4.001F}).status == Status::MISS, "near positive Z edge");
}

void benchmark() {
    auto d = data();
    d.sectorsX = d.sectorsZ = 8;
    d.maxLevel = 2;
    d.sectorSize = 8;
    Loading loading;
    LandscapeQuery q(d, loading.loader(), 64);
    q.setSource(1, {8, 8}, 4);
    loading.drain(q);
    q.setSource(2, {24, 24}, 2);
    loading.drain(q);
    double checksum = 0;
    const auto measure = [&](const char *name, uint32_t count, auto operation) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < count; ++i)
            operation(i);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "{\"case\":\"" << name << "\",\"iterations\":" << count << ",\"ms\":" << ms << "}\n";
    };
    measure("stationary_source", 100000, [&](uint32_t) {
        checksum += uint32_t(q.setSource(1, {8, 8}, 4));
    });
    measure("resident_sample", 500000, [&](uint32_t i) {
        checksum += q.sample({6.1F + (i % 30) * .1F, 6.3F}).position.y;
    });
    measure("moving_source", 10000, [&](uint32_t i) {
        checksum += uint32_t(q.setSource(1, {float((i * 17) % 56 + 4), float((i * 29) % 56 + 4)}, 4));
        loading.drain(q);
    });
    std::cout << "checksum=" << checksum << " requests=" << loading.requests << '\n';
}

void regionOwnership() {
    Loading loading;
    LandscapeQuery q(data(), loading.loader(), 3);
    q.setSource(1, {2, 1}, .9F); // X tiles 0, 1.
    q.setSource(2, {1, 1}, 0);   // Another owner of tile 0.
    loading.drain(q);
    q.setSource(1, {4, 1}, .9F); // X tiles 1, 2; releases only its own old pin.
    loading.drain(q);
    require(q.sourceStatus(1) == Status::HIT && q.sourceStatus(2) == Status::HIT, "overlapping region move must fit");
    const auto requests = loading.requests;
    q.setSource(1, {4.05F, 1}, .9F);
    loading.drain(q);
    require(loading.requests == requests, "unchanged tile rectangle must not reload");
    require(q.setSource(1, {6, 2}, .9F) == Status::OUT_OF_CAPACITY, "other owner's tile prevents oversized union");
    require(q.sourceStatus(2) == Status::HIT, "capacity refusal must preserve other source");
    q.setSource(3, {7, 1}, 0);
    loading.drain(q);
    require(q.sourceStatus(3) == Status::HIT, "rejected moving source must release its old pins");

    Loading retained;
    LandscapeQuery cache(data(), retained.loader(), 2);
    cache.setSource(1, {3, 1}, 0);
    retained.drain(cache);
    cache.setSource(2, {5, 1}, 0);
    retained.drain(cache);
    cache.removeSource(1);
    cache.removeSource(2);
    const auto before = retained.requests;
    cache.setSource(3, {2, 1}, .9F);
    retained.drain(cache);
    require(retained.requests == before + 1, "inserting first tile must not evict the later requested cached tile");
    require(cache.sourceStatus(3) == Status::HIT, "full incoming region retained");
}

void reusedKeyAndConcurrentDestruction() {
    Loading loading;
    LandscapeQuery q(data(), loading.loader(), 1);
    q.setSource(1, {1, 1}, 0);
    q.update();
    q.setSource(1, {3, 1}, 0);
    q.update();
    q.setSource(1, {1, 1}, 0);
    q.update();
    require(loading.jobs.size() == 2, "eviction must not release outstanding load slots");
    loading.finish(false);
    q.update(); // Failure belongs to the old load of the SAME key.
    require(q.sourceStatus(1) == Status::NOT_READY, "old failure must not poison re-requested key");
    loading.drain(q);
    require(q.sourceStatus(1) == Status::HIT, "new load of reused key must publish");
    for (uint32_t i = 0; i < 64; ++i) {
        Loading pending;
        auto temporary = std::make_unique<LandscapeQuery>(data(), pending.loader(), 1);
        temporary->setSource(1, {1, 1}, 0);
        temporary->update();
        auto job = std::move(pending.jobs.front());
        pending.jobs.pop_front();
        std::thread worker([job = std::move(job)]() mutable {
            job.complete(tile(job.x, job.z), true);
        });
        temporary.reset();
        worker.join();
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) {
        benchmark();
        return 0;
    }
    sampling();
    budgetAndSharing();
    completionLifetime();
    failureAndSplat();
    triangleHeight();
    holes();
    malformedAndRapidMovement();
    regionOwnership();
    reusedKeyAndConcurrentDestruction();
    std::cout << "Landscape CPU query tests passed\n";
}
