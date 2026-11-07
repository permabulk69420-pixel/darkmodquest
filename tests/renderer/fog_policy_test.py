#!/usr/bin/env python3
"""Exercise production VR fog policy and portal culling without a headset."""
from pathlib import Path
from contextlib import contextmanager
import ctypes as C
import subprocess
import tempfile
import unittest
from shader_install_test import method

ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / 'Q3E/src/main/jni/darkmod/renderer'


@contextmanager
def compiled_surface_policy():
    """Use shipping C++ policy and content classification, also in GLES tests."""
    policy = method(RENDERER / 'backend/stages/MobileLightingStage.cpp', 'bool R_QuestFogDisabled(')
    surface = method(RENDERER / 'backend/stages/MobileLightingStage.cpp', 'bool R_QuestSkipFogSurface(')
    volume = method(RENDERER / 'backend/stages/MobileLightingStage.cpp', 'bool R_QuestSkipFogVolume(')
    render_volumes = method(RENDERER / 'backend/stages/VolumetricStage.cpp', 'void VolumetricStage::RenderAll(')
    source = r'''
#include "renderer/QuestAtmosphere.h"
#include "renderer/QuestRenderDiagnostics.h"
struct viewLight_t;
struct viewDef_t {bool lightgem;viewLight_t *viewLights=nullptr;bool IsLightGem()const{return lightgem;}};
struct {bool active;bool IsActive()const{return active;}} backend;
auto *vrBackend=&backend;
struct {bool enabled;bool GetBool()const{return enabled;}} vr_fog;
struct srfTriangles_t {bool atmosphericFog;};
struct idMaterial {bool fog;bool fogLight=false;bool IsAtmosphericFog()const{return fog;}bool IsFogLight()const{return fogLight;}};
struct viewLight_t {const idMaterial *lightShader;float volumetricDust;viewLight_t *next;};
QuestRenderCounters questRenderCounters;
class VolumetricStage {public:void RenderAll(const viewDef_t *view);bool RenderLight(const viewDef_t*,const viewLight_t*){return true;}};
POLICY
SURFACE
VOLUME
RENDER_VOLUMES
extern "C" bool quest_fog_skip(bool active,bool fog,bool lightgem,const char *particle,const char *material){
 backend.active=active;vr_fog.enabled=fog;viewDef_t view{lightgem};
 idMaterial mat{QuestAtmosphere::IsFogMaterial(material)};
 srfTriangles_t tri{QuestAtmosphere::IsFogParticle(particle)||mat.fog};
 return R_QuestSkipFogSurface(&view,&tri,&mat);
}
extern "C" int quest_volume_passes(bool fog,bool lighting,bool lightgem){
 backend.active=true;vr_fog.enabled=fog;
 idMaterial regular{false,false}, atmosphere{true,true};
 viewLight_t fogLight{&atmosphere,lighting?1.0f:0.0f,nullptr};
 viewLight_t lamp{&regular,lighting?1.0f:0.0f,&fogLight};
 viewDef_t view{lightgem,&lamp};questRenderCounters={};
 VolumetricStage stage;stage.RenderAll(&view);
 return questRenderCounters.volumetricLights;
}
extern "C" const char *quest_scissor_state(bool enabled,long long area,int checks,int mismatches){
 return QuestScissorStatus(enabled,area,checks,mismatches);
}
'''.replace('POLICY', policy).replace('SURFACE', surface).replace('RENDER_VOLUMES', render_volumes).replace('VOLUME', volume)
    with tempfile.TemporaryDirectory(prefix='tdm-fog-surface-') as directory:
        cpp = Path(directory) / 'fog.cpp'
        library = Path(directory) / 'fog.so'
        cpp.write_text(source)
        subprocess.run(['g++', '-std=c++14', '-O2', '-shared', '-fPIC', '-DTDM_VR_OPENXR',
                        '-I'+str(RENDERER.parent), str(cpp), '-o', str(library)], check=True)
        lib = C.CDLL(str(library))
        skip = lib.quest_fog_skip
        skip.argtypes = [C.c_bool, C.c_bool, C.c_bool, C.c_char_p, C.c_char_p]
        skip.restype = C.c_bool
        skip.volume_passes = lib.quest_volume_passes
        skip.volume_passes.argtypes = [C.c_bool, C.c_bool, C.c_bool]
        skip.volume_passes.restype = C.c_int
        skip.scissor_state = lib.quest_scissor_state
        skip.scissor_state.argtypes = [C.c_bool, C.c_longlong, C.c_int, C.c_int]
        skip.scissor_state.restype = C.c_char_p
        yield skip


class FogPolicyTests(unittest.TestCase):
    def test_fog_and_volumetric_lighting_are_independent(self):
        # Run the actual production VolumetricStage::RenderAll dispatch, not a
        # duplicate of its control flow. The lamp's shaft must survive fog-off.
        with compiled_surface_policy() as policy:
            self.assertEqual(policy.volume_passes(False, True, False), 1)
            self.assertEqual(policy.volume_passes(True, True, False), 2)
            self.assertEqual(policy.volume_passes(False, False, False), 0)
            self.assertEqual(policy.volume_passes(True, False, False), 0)
            self.assertEqual(policy.volume_passes(False, True, True), 0)

    def test_diagnostics_require_actual_scissor_samples(self):
        with compiled_surface_policy() as policy:
            self.assertEqual(policy.scissor_state(False, 1000, 2, 0), b'OFF')
            self.assertTrue(policy.scissor_state(True, 0, 0, 0).startswith(b'IDLE'))
            self.assertTrue(policy.scissor_state(True, 1000, 0, 0).startswith(b'IDLE'))
            self.assertTrue(policy.scissor_state(True, 1000, 2, 1).startswith(b'FAIL'))
            self.assertTrue(policy.scissor_state(True, 1000, 2, 0).startswith(b'VERIFIED'))

    def test_particle_fog_and_sheets_follow_switch_without_removing_shared_smoke(self):
        # Actual core content identifiers: dustfog uses the SAME shotgunpuff
        # material as ordinary smoke. Disabling that material globally is wrong.
        cases = [
            ('tdm_dustfog1', 'textures/sfx/shotgunpuff', True),
            ('tdm_dustfog2', 'textures/sfx/shotgunpuff', True),
            ('tdm_dustfog3', 'textures/sfx/shotgunpuff', True),
            ('tdm_dustfog4', 'textures/sfx/shotgunpuff', True),
            ('tdm_fog_overcast', 'textures/darkmod/sfx/overcast', True),
            ('ea001_etherfog_high', 'textures/darkmod/sfx/overcast2', True),
            ('custom_mist', 'textures/sfx/shotgunpuff', True),
            ('map_effect', 'textures/darkmod/sfx/overcast2', True),
            ('', 'textures/darkmod/fogs/real_fog_layer_moving', True),
            ('', 'fogs/real_fog_moving', True),
            ('TDM_DUSTFOG3.PRT', 'textures/sfx/shotgunpuff', True),
            ('tdm_smoke', 'textures/sfx/shotgunpuff', False),
            ('tdm_fire', 'textures/darkmod/sfx/fake_haze_01', False),
            ('waterarrow', 'textures/water_source/watermist_m1add', False),
            ('alchemist_magic', 'textures/particles/spark', False),
            ('', 'textures/darkmod/nature/skybox/skybox_fogged_01', False),
            ('', 'textures/darkmod/window/smallpanels_4w_dirty02_fogblend_litdull', False),
            ('', 'lights/ambientCube/skybox_fogged_01_cube', False),
            ('', '', False),
        ]
        with compiled_surface_policy() as skip:
            for particle, material, atmosphere in cases:
                args = (particle.encode(), material.encode())
                with self.subTest(particle=particle, material=material):
                    for eye in range(2):
                        self.assertEqual(skip(True, False, False, *args), atmosphere)
                        self.assertFalse(skip(True, True, False, *args))  # enabled
                        self.assertFalse(skip(True, False, True, *args))  # lightgem
                        self.assertFalse(skip(False, False, False, *args))  # flat

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
