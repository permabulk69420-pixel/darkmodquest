// Standalone CPU tests: g++ -std=c++14 -I. tests/renderer/mobile_plan_test.cpp -o /tmp/mobile-plan-test
#include "Q3E/src/main/jni/darkmod/renderer/backend/MobileRenderPlan.h"
#include <cassert>
#include <iostream>
#include <random>
using namespace MobileRenderPlan;

static void CheckPages(int extent, const std::vector<int> &requested,
    const std::vector<std::pair<int,int>> &shapes = {}) {
    std::vector<Page> pages, repeat;
    assert(Pack(extent, requested, pages, shapes));
    assert(Pack(extent, requested, repeat, shapes));
    assert(pages.size() == requested.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        const Page &p = pages[i];
        assert(p.size > 0 && p.size <= requested[i]);
        int w=(shapes.empty()?3:shapes[i].first)*p.size, h=(shapes.empty()?4:shapes[i].second)*p.size;
        assert(p.x >= 0 && p.y >= 0 && p.x + w <= extent && p.y + h <= extent);
        assert(p.x == repeat[i].x && p.y == repeat[i].y && p.size == repeat[i].size);
        for (size_t j = 0; j < i; ++j) {
            const Page &q = pages[j];
            int qw=(shapes.empty()?3:shapes[j].first)*q.size, qh=(shapes.empty()?4:shapes[j].second)*q.size;
            assert(p.x >= q.x + qw || q.x >= p.x + w || p.y >= q.y + qh || q.y >= p.y + h);
        }
    }
}
int main() {
    CheckPages(2048, {});
    CheckPages(2048, {512});
    CheckPages(512, std::vector<int>(1024, 256)); // pressure must reduce resolution, not lose lights
    CheckPages(2048, {512,256,128,128,256}, {{1,2},{3,2},{3,4},{1,4},{3,2}});
    std::vector<Page> pages;
    assert(!Pack(3, {1}, pages));
    assert(!Pack(4, std::vector<int>(4, 1), pages));
    std::mt19937 rng(31);
    for (int count = 1; count <= 200; ++count) {
        std::vector<int> requested;
        for (int i = 0; i < count; ++i) requested.push_back(32 << (rng()%5));
        CheckPages(2048, requested);
    }
    // A small AABB around each cardinal direction belongs to that face alone.
    for (int face = 0; face < 6; ++face) {
        float lo[3] = {-1,-1,-1}, hi[3] = {1,1,1};
        int axis = face/2; lo[axis] = face%2 ? -11 : 9; hi[axis] = face%2 ? -9 : 11;
        assert(CubeFaceMask(lo,hi) == (1u << face));
    }
    float lo[3] = {-10,-10,-10}, hi[3] = {10,10,10};
    assert(CubeFaceMask(lo,hi) == 63);
    // Every point actually inside the near-clipped cube face must be retained
    // by its enclosing AABB, including boxes crossing a seam / surrounding a light.
    for (int n = 0; n < 10000; ++n) {
        for (int a = 0; a < 3; ++a) { lo[a] = int(rng()%200)-100; hi[a] = lo[a]+int(rng()%100); }
        unsigned mask = CubeFaceMask(lo,hi);
        for (int k = 0; k < 8; ++k) {
            float p[3]; for (int a = 0; a < 3; ++a) p[a] = k & (1<<a) ? hi[a] : lo[a];
            float major = std::max(std::abs(p[0]),std::max(std::abs(p[1]),std::abs(p[2])));
            if (major < 1) continue;
            for (int a = 0; a < 3; ++a) if (std::abs(p[a]) == major)
                assert(mask & (1u << (2*a+(p[a]<0))));
        }
    }
    std::cout << "PASS: atlas bounds/overlap/pressure/determinism; six face directions; 80,000 conservative corner checks\n";
}
