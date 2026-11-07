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
    const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float projection[16] = {1,0,0,0, 0,1,0,0, 0,0,-1,-1, 0,0,-2,0};
    const Rect viewport = {7,11,1031,779};
    float boxLo[3] = {-1,-1,-10}, boxHi[3] = {1,1,-8};
    Rect small = ProjectScissor(boxLo,boxHi,identity,projection,viewport);
    assert(!small.Empty() && small.Area() < viewport.Area()/20);
    float behindLo[3] = {-1,-1,2}, behindHi[3] = {1,1,4};
    assert(ProjectScissor(behindLo,behindHi,identity,projection,viewport).Empty());
    float aroundLo[3] = {-10,-10,-2}, aroundHi[3] = {10,10,2};
    assert(ProjectScissor(aroundLo,aroundHi,identity,projection,viewport).Area()==viewport.Area());
    float eye[16]; std::copy(identity,identity+16,eye);
    eye[12]=0.5f; Rect left=ProjectScissor(boxLo,boxHi,eye,projection,viewport);
    eye[12]=-0.5f; Rect right=ProjectScissor(boxLo,boxHi,eye,projection,viewport);
    assert(left.x1>right.x1 && left.x2>right.x2);
    Rect both; both.Add(left);both.Add(right);both.Add({});
    assert(both.x1==right.x1 && both.x2==left.x2);
    // Sample points independently with float shader-style MVP operations.
    // Every point in the eye's clip volume must survive the CPU scissor,
    // including near-plane intersections, mirrored axes and asymmetric eyes.
    int visiblePoints=0;
    for(int n=0;n<10000;++n) {
        for(int axis=0;axis<3;++axis) {
            boxLo[axis]=float(int(rng()%800)-400)*0.1f;
            boxHi[axis]=boxLo[axis]+float(rng()%200+1)*0.1f;
        }
        std::copy(identity,identity+16,eye);
        const float angle=float(int(rng()%100)-50)*0.01f;
        eye[0]=std::cos(angle);eye[2]=std::sin(angle);
        eye[8]=-std::sin(angle);eye[10]=std::cos(angle);
        if(n&1) {eye[0]=-eye[0];eye[8]=-eye[8];}
        eye[12]=float(int(rng()%40)-20)*0.03f;
        eye[13]=float(int(rng()%40)-20)*0.03f;
        projection[8]=float(int(rng()%60)-30)*0.01f;
        projection[9]=float(int(rng()%60)-30)*0.01f;
        const Rect r=ProjectScissor(boxLo,boxHi,eye,projection,viewport);
        assert(r.Empty() || (r.x1>=viewport.x1 && r.x2<=viewport.x2 && r.y1>=viewport.y1 && r.y2<=viewport.y2));
        for(int sample=0;sample<64;++sample) {
            float point[4]={0,0,0,1},v[4]={},clip[4]={};
            for(int axis=0;axis<3;++axis) point[axis]=boxLo[axis]+(boxHi[axis]-boxLo[axis])*float(rng()%10001)/10000.0f;
            for(int row=0;row<4;++row) for(int col=0;col<4;++col) v[row]+=eye[4*col+row]*point[col];
            for(int row=0;row<4;++row) for(int col=0;col<4;++col) clip[row]+=projection[4*col+row]*v[col];
            if(clip[3]<=0 || clip[2]<-clip[3] || clip[2]>clip[3] ||
                std::abs(clip[0])>clip[3] || std::abs(clip[1])>clip[3]) continue;
            const float x=viewport.x1+(clip[0]/clip[3]*0.5f+0.5f)*(viewport.x2-viewport.x1);
            const float y=viewport.y1+(clip[1]/clip[3]*0.5f+0.5f)*(viewport.y2-viewport.y1);
            assert(!r.Empty() && x>=r.x1 && x<=r.x2 && y>=r.y1 && y<=r.y2);
            ++visiblePoints;
        }
    }
    assert(visiblePoints>10000);
    std::cout<<"PASS: eye-specific scissors retain "<<visiblePoints<<" visible points across 10,000 rotated/mirrored/near-clipped boxes\n";
}
