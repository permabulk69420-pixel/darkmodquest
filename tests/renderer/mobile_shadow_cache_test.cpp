// g++ -std=c++14 -I Q3E/src/main/jni/darkmod tests/renderer/mobile_shadow_cache_test.cpp -o /tmp/cache-test
#include "renderer/backend/MobileShadowCache.h"
#include <cassert>
#include <iostream>
#include <random>
using namespace MobileShadowCache;

static Signature Content(uint64_t value) { Signature s;s.Add(value);return s; }
static void CheckLayout(AtlasLayout &layout,int extent,std::vector<Request> requests) {
    std::vector<int> sizes;std::vector<std::pair<int,int>> shapes;
    for(const auto &r:requests){sizes.push_back(r.size);shapes.emplace_back(r.columns,r.rows);}
    std::vector<MobileRenderPlan::Page> budget,pages;
    assert(MobileRenderPlan::Pack(extent,sizes,budget,shapes));
    for(size_t i=0;i<requests.size();++i)requests[i].size=budget[i].size;
    bool repacked=false;assert(layout.Prepare(extent,requests,pages,repacked));
    assert(pages.size()==requests.size());assert(layout.slots.size()==requests.size());
    for(size_t i=0;i<pages.size();++i){
        const auto &p=pages[i];const auto &r=requests[i];
        assert(p.size>0&&p.x>=0&&p.y>=0&&p.x+r.columns*p.size<=extent&&p.y+r.rows*p.size<=extent);
        for(size_t j=0;j<i;++j){
            const auto &q=pages[j];const auto &t=requests[j];
            assert(p.x>=q.x+t.columns*q.size||q.x>=p.x+r.columns*p.size||p.y>=q.y+t.rows*q.size||q.y>=p.y+r.rows*p.size);
        }
    }
}
int main(){
    assert(MobileRenderPlan::ReuseCapturedView(true,true,false,false,false,false,true,false));
    for(int bit=0;bit<8;++bit){
        bool flags[8]={true,true,false,false,false,false,true,false};flags[bit]=!flags[bit];
        assert(!MobileRenderPlan::ReuseCapturedView(flags[0],flags[1],flags[2],flags[3],flags[4],flags[5],flags[6],flags[7]));
    }
    const Signature stone=Content(1),doorClosed=Content(2),doorOpen=Content(3),alphaChanged=Content(4);
    Signature upload1,upload2,changedIndices;
    assert(IndexSignature(upload1,91,false,16,128,1));
    assert(IndexSignature(upload2,91,false,4096,128,2));
    assert(upload1==upload2); // temporary index-buffer rotation must not defeat caching
    assert(IndexSignature(changedIndices,92,false,16,128,1));assert(upload1!=changedIndices);
    Signature dynamicIndices;assert(!IndexSignature(dynamicIndices,0,false,16,128,1));
    CasterSet a,b;a.Add(stone);a.Add(doorClosed);b.Add(doorClosed);b.Add(stone);
    assert(a.Finish(Content(0))==b.Finish(Content(0))); // traversal order does not invalidate depth
    b.Add(stone);assert(a.Finish(Content(0))!=b.Finish(Content(0)));
    FaceState face;
    assert(face.FinalDirty(stone));face.finalContent=stone;face.haveFinal=true;
    assert(!face.FinalDirty(stone));assert(face.FinalDirty(doorOpen));
    assert(face.FinalDirty(alphaChanged));face={};assert(face.FinalDirty(stone));
    AtlasLayout layout;
    CheckLayout(layout,2048,{{1,256,3,4},{2,128,3,2},{3,64,1,4}});
    const auto first=layout.slots;
    CheckLayout(layout,2048,{{3,64,1,4},{2,128,3,2},{1,256,3,4}});
    for(const auto &s:first){assert(layout.slots.at(s.first).page.x==s.second.page.x&&layout.slots.at(s.first).page.y==s.second.page.y);}
    CheckLayout(layout,2048,{{2,128,3,2},{3,64,1,4},{4,256,3,4}});assert(!layout.slots.count(1));
    CheckLayout(layout,1024,{{2,256,3,4},{3,256,3,4},{4,256,1,2}});
    std::mt19937 rng(91);
    for(int frame=0;frame<1000;++frame){
        std::vector<Request> requests;
        for(uint64_t light=1;light<=60;++light)if(rng()%3)
            requests.push_back({light,int(32u<<(rng()%4)),rng()%4?3:1,rng()%2?4:2});
        std::shuffle(requests.begin(),requests.end(),rng);
        CheckLayout(layout,frame%50==0?1024:2048,requests);
    }
    std::cout<<"PASS: retained-face shadow invalidation, movement/alpha changes, retirement, resize, persistent slots and 1,000 changing light sets\n";
}
