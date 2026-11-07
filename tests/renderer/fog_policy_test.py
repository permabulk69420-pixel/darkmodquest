#!/usr/bin/env python3
"""Exercise production VR fog policy and portal culling without a headset."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from shader_install_test import method

ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / 'Q3E/src/main/jni/darkmod/renderer'


class FogPolicyTests(unittest.TestCase):
    def test_fog_off_reveals_scenery_and_preserves_lightgem(self):
        policy = method(RENDERER / 'backend/stages/MobileLightingStage.cpp', 'bool R_QuestFogDisabled(')
        portal = method(RENDERER / 'frontend/RenderWorld_portals.cpp', 'bool idRenderWorldLocal::PortalIsFoggedOut(')
        # Compile the shipping functions verbatim. The surrounding types are
        # fixtures supplying an opaque fog portal and material registers.
        source = r'''
#include <cassert>
#include <cstdio>
#include <alloca.h>
#define _alloca alloca
constexpr float DEFAULT_FOG_DISTANCE=500;
struct idVec3 {float v[3];};
struct idPlane {
 float v[4];float &operator[](int i){return v[i];}
 float Distance(const idVec3 &p)const{return v[0]*p.v[0]+v[1]*p.v[1]+v[2]*p.v[2]+v[3];}
};
struct viewDef_t {
 struct {float modelViewMatrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};} worldSpace;
 bool lightgem=false;bool IsLightGem()const{return lightgem;}
};
struct Cvar {int value=0;bool GetBool()const{return value!=0;}int GetInteger()const{return value;}};
Cvar vr_fog,r_skipFogLights;
struct Backend {bool active=false;bool IsActive()const{return active;}} backend;
Backend *vrBackend=&backend;
struct {viewDef_t *viewDef;} tr;
struct shaderStage_t {struct {int registers[4]={0,1,2,3};} color;};
struct idMaterial {
 mutable int evaluations=0;shaderStage_t stage;
 int GetNumRegisters()const{return 4;}
 void EvaluateRegisters(float *r,const float*,const viewDef_t*,void*)const{
  ++evaluations;r[0]=r[1]=r[2]=0;r[3]=100;
 }
 const shaderStage_t *GetStage(int)const{return &stage;}
};
struct idRenderLightLocal {idMaterial *lightShader;struct {float shaderParms[4];void *referenceSound=nullptr;} parms;};
struct Point {idVec3 p;const idVec3 &ToVec3()const{return p;}};
struct Winding {Point points[4];int GetNumPoints()const{return 4;}const Point &operator[](int i)const{return points[i];}};
struct DoublePortal {idRenderLightLocal *fogLight;};
struct portal_t {DoublePortal *doublePortal;Winding w;};
struct idRenderWorldLocal {bool PortalIsFoggedOut(const portal_t *p);};
POLICY
PORTAL
int main(){
 viewDef_t view;tr.viewDef=&view;idMaterial material;
 idRenderLightLocal light;light.lightShader=&material;
 DoublePortal dp{&light};portal_t p;p.doublePortal=&dp;
 for(auto &point:p.w.points)point.p={{1,1,-200}};
 idRenderWorldLocal world;
 auto check=[&](bool active,bool fog,bool lightgem,int skip,bool opaque,int evaluations){
  backend.active=active;vr_fog.value=fog;view.lightgem=lightgem;
  r_skipFogLights.value=skip;material.evaluations=0;
  assert(world.PortalIsFoggedOut(&p)==opaque);
  assert(material.evaluations==evaluations);
 };
 check(true,true,false,0,true,1);   // opaque fog normally hides this portal
 check(true,false,false,0,false,0); // removed fog reveals its scenery
 check(true,false,true,0,true,1);   // preserve the gameplay lightgem view
 check(false,false,false,0,true,1); // flat rendering keeps its existing fog
 check(true,true,false,7,false,0);  // full-fog debug skip agrees with visibility
 check(false,true,false,7,false,0);
 for(auto &point:p.w.points)point.p.v[2]=-20;
 check(true,true,false,0,false,1);  // nearby portal is not fogged out
 std::puts("PASS: fog-off reveals scenery; enabled fog, flat views and lightgem retain original culling");
}
'''.replace('POLICY', policy).replace('PORTAL', portal)
        with tempfile.TemporaryDirectory(prefix='tdm-fog-policy-') as directory:
            cpp = Path(directory) / 'fog.cpp'
            binary = Path(directory) / 'fog-test'
            cpp.write_text(source)
            build = subprocess.run(['g++', '-std=c++14', '-O2', '-DTDM_VR_OPENXR',
                                    str(cpp), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main(verbosity=2)
