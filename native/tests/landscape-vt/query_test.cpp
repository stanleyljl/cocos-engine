// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
#include "landscape/LandscapeQuery.h"
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <thread>

using namespace cc::landscape;
using Status = LandscapeQueryStatus;

void require(bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
void close(float a, float b, const char *message) { require(std::abs(a-b) < 1e-3F, message); }

LandscapeData data() {
    LandscapeData d;
    d.sectorsX = 2; d.sectorsZ = 1; d.maxLevel = 1; d.minTileLevel = 0;
    d.tileResolution = 3; d.sectorSize = 4; d.heightScale = 65535; d.heightBias = -100;
    return d;
}
LandscapeQueryTile tile(uint32_t tx, uint32_t tz) {
    LandscapeQueryTile t;
    t.height.resize(18); t.splat.resize(18); t.normal.resize(27);
    for (uint32_t z=0; z<3; ++z) for (uint32_t x=0; x<3; ++x) {
        const uint16_t h = static_cast<uint16_t>(100 + (tx*2+x)*100 + (tz*2+z)*50);
        const size_t i=z*3+x;
        t.height[i*2]=h>>8; t.height[i*2+1]=h&255;
        const uint16_t s = 3U | (7U<<5U) | (21U<<10U);
        std::memcpy(t.splat.data()+i*2, &s, 2);
        // Deliberately different from the height gradient: queries must use RGB.
        t.normal[i*3]=64; t.normal[i*3+1]=220; t.normal[i*3+2]=160;
    }
    return t;
}
struct Loading {
    struct Job { uint32_t x,z; LandscapeQuery::Completion complete; };
    std::deque<Job> jobs;
    size_t requests{0};
    LandscapeQuery::Loader loader() {
        return [this](uint32_t x,uint32_t z,LandscapeQuery::Completion done) {
            ++requests; jobs.push_back({x,z,std::move(done)});
        };
    }
    void finish(bool ok=true) {
        auto job=std::move(jobs.front()); jobs.pop_front();
        job.complete(tile(job.x,job.z),ok);
    }
    void drain(LandscapeQuery &query) {
        query.update();
        for (int i=0;i<20 && !jobs.empty();++i) {
            while(!jobs.empty()) finish();
            query.update();
        }
    }
};

void sampling() {
    Loading loading;
    LandscapeQuery q(data(),loading.loader(),8);
    require(q.sample(0,0).status==Status::NOT_READY,"cold query must not load");
    require(loading.requests==0,"query triggered I/O");
    require(q.setSource(1,0,0,4)==Status::NOT_READY,"preload should be pending");
    q.update();
    require(loading.jobs.size()==2,"decode concurrency must be bounded");
    loading.drain(q);
    require(q.sourceStatus(1)==Status::HIT,"region ready");
    auto hit=q.sample(-3.25F,-1.75F);
    require(hit.status==Status::HIT,"sample hit");
    close(hit.position.y,87.5F,"bilinear height and height bias");
    const float length=std::sqrt(127.0F*127.0F+185.0F*185.0F+65.0F*65.0F);
    close(hit.normal.x,-127.0F/length,"source normal X");
    close(hit.normal.y,185.0F/length,"source normal Y");
    close(hit.normal.z,65.0F/length,"source normal Z");
    require(hit.surfaceType==3,"dominant material"); close(hit.surfaceWeight,2.0F/3.0F,"material weight");
    close(q.sample(4,2).position.y,1000,"inclusive far corner");
    close(q.sample(-4,-2).position.y,0,"inclusive near corner");
    close(q.sample(-2,0).position.y,300,"tile/sector boundary");
    close(q.sample(-2.000001F,0).position.y,q.sample(-1.999999F,0).position.y,"tile seam");
    require(q.sample(4.01F,2).status==Status::MISS,"outside should miss");
    require(q.sample(std::numeric_limits<float>::quiet_NaN(),0).status==Status::ERROR,"invalid coordinate");
    const auto loaded=loading.requests;
    q.removeSource(1);
    require(q.sample(0,0).status==Status::HIT,"unpinned data retained until evicted");
    q.update(); require(loading.requests==loaded,"queries must not schedule new work");
}

void budgetAndSharing() {
    Loading loading;
    LandscapeQuery q(data(),loading.loader(),2);
    q.setSource(1,-3,-1,0); q.setSource(2,-3,-1,0); q.setSource(3,-1,-1,0);
    loading.drain(q);
    require(loading.requests==2,"overlapping sources share decoding");
    require(q.setSource(4,1,-1,0)==Status::NOT_READY,"pinned capacity must reject admission");
    q.removeSource(1);
    require(q.setSource(4,1,-1,0)==Status::NOT_READY,"second owner must still protect tile");
    q.removeSource(2);
    q.setSource(4,1,-1,0); loading.drain(q);
    require(q.sample(-3,-1).status==Status::NOT_READY,"unpinned old tile evicted");
    require(q.sample(-1,-1).status==Status::HIT,"other source preserved");
    require(q.sample(1,-1).status==Status::HIT,"new source admitted");
    require(q.setSource(9,0,0,100000)==Status::NOT_READY,"huge region bounded");
    require(q.setSource(9,0,0,-1)==Status::ERROR,"negative radius rejected");
    require(q.setSource(9,99,99,1)==Status::MISS,"non-overlapping region");
}

void completionLifetime() {
    Loading loading;
    LandscapeQuery q(data(),loading.loader(),1);
    q.setSource(1,-3,-1,0); q.update();
    q.setSource(1,1,-1,0); q.update();
    require(loading.jobs.size()==2,"two outstanding generations");
    loading.finish(); q.update();
    require(q.sample(1,-1).status==Status::NOT_READY,"stale tile must not publish");
    std::thread worker([&loading]{loading.finish();}); worker.join(); q.update();
    require(q.sample(1,-1).status==Status::HIT,"worker result published on update");
    {
        LandscapeQuery temporary(data(),loading.loader(),1);
        temporary.setSource(1,-3,-1,0); temporary.update();
    }
    loading.finish(); // Must safely drop results after destruction.
}

void failureAndSplat() {
    Loading loading;
    LandscapeQuery q(data(),loading.loader(),1);
    q.setSource(1,-3,-1,0); q.update(); loading.finish(false); q.update();
    require(q.sourceStatus(1)==Status::ERROR && q.sample(-3,-1).status==Status::ERROR,"failed load distinguished from miss");
    q.removeSource(1); q.setSource(1,-3,-1,0); loading.drain(q);
    require(q.sourceStatus(1)==Status::HIT,"new registration retries failed data");
    Loading splatLoader;
    LandscapeQuery splat(data(),splatLoader.loader(),1);
    splat.setSource(1,-3,-1,0); splat.update();
    auto t=tile(0,0);
    for (const auto &pair : {std::pair<size_t,uint16_t>{0,3}, {1,7}, {3,11}, {4,13}})
        std::memcpy(t.splat.data()+pair.first*2,&pair.second,2);
    splatLoader.jobs.front().complete(std::move(t),true); splatLoader.jobs.pop_front(); splat.update();
    const auto hit=splat.sample(-3.25F,-1.75F);
    require(hit.surfaceType==7,"splat triangle interpolation"); close(hit.surfaceWeight,0.5F,"splat spatial weights");
}

void triangleHeight() {
    Loading loading;
    LandscapeQuery q(data(),loading.loader(),1);
    q.setSource(1,-3,-1,0); q.update();
    auto t=tile(0,0);
    for (const auto &pair : {std::pair<size_t,uint16_t>{0,100}, {1,102}, {3,104}, {4,110}}) {
        t.height[pair.first*2]=pair.second>>8; t.height[pair.first*2+1]=pair.second&255;
    }
    loading.jobs.front().complete(std::move(t),true); loading.jobs.pop_front(); q.update();
    close(q.sample(-3.5F,-1.5F).position.y,3,"B-C diagonal, not bilinear or A-D");
    close(q.sample(-3.8F,-1.7F).position.y,1.6F,"first triangle");
    close(q.sample(-3.2F,-1.3F).position.y,6.4F,"second triangle");
}

int main() {
    sampling(); budgetAndSharing(); completionLifetime(); failureAndSplat(); triangleHeight();
    std::cout << "Landscape CPU query tests passed\n";
}
