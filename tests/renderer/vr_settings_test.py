#!/usr/bin/env python3
"""Compile production VR preference mapping with the actual command parser."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from shader_install_test import JAVA, method


def main():
    jdk = Path(os.environ.get('JAVA_HOME', '')) / 'bin'
    java = str(jdk / 'java') if (jdk / 'java').is_file() else shutil.which('java')
    javac = str(jdk / 'javac') if (jdk / 'javac').is_file() else shutil.which('javac')
    with tempfile.TemporaryDirectory(prefix='quest-vr-settings-') as directory:
        base = Path(directory)
        package = base / 'com/n0n3m4/q3e'
        (package / 'karin').mkdir(parents=True)
        for name in ('KidTechCommand.java', 'KidTech4Command.java'):
            shutil.copyfile(JAVA / 'karin' / name, package / 'karin' / name)
        (package / 'Q3EGameConstants.java').write_text(
            'package com.n0n3m4.q3e; public class Q3EGameConstants { public static final String GAME_EXECUABLE="tdm"; }')
        (package / 'Q3EGlobals.java').write_text('package com.n0n3m4.q3e; public class Q3EGlobals {}')
        keys = re.findall(r'public static final String pref_vr_\w+\s*=\s*"[^"]*";',
                          (JAVA / 'Q3EPreference.java').read_text())
        mapping = method(JAVA / 'Q3EGameHelper.java', 'private String AddVRSettings(')
        harness = '''
import com.n0n3m4.q3e.karin.*;
import java.util.*;
class Q3EPreference { KEYS }
class SharedPreferences extends HashMap<String,Object> {
    boolean contains(String key) { return containsKey(key); }
    boolean getBoolean(String key,boolean fallback) { return (Boolean)getOrDefault(key,fallback); }
    int getInt(String key,int fallback) { return (Integer)getOrDefault(key,fallback); }
}
class Q3E {
    static Q3E q3ei=new Q3E();
    KidTechCommand GetGameCommandEngine(String cmd) { return new KidTech4Command(cmd); }
}
public class VRSettingsHarness {
    MAPPING
    static void check(boolean condition,String message) { if(!condition)throw new AssertionError(message); }
    public static void main(String[] args) {
        VRSettingsHarness helper=new VRSettingsHarness();
        String existing="tdm +set r_questFramebufferOptimizations 0 +set r_questMixedPrecision 0 +set r_frameAllocChunks 0 +set vr_rendererDiagnostics 1 +set vr_fog 1 +set r_volumetricEnable 1 +set vr_resolutionScale 1.0";
        KidTechCommand defaults=new KidTech4Command(helper.AddVRSettings(new SharedPreferences(),existing));
        check(defaults.Prop("r_questFramebufferOptimizations").equals("1"),"copy default");
        check(defaults.Prop("r_questMixedPrecision").equals("1"),"precision default");
        check(defaults.Prop("r_frameAllocChunks").equals("1"),"scene memory default");
        check(defaults.Prop("r_questBufferStreaming").equals("1"),"streaming default");
        check(defaults.Prop("vr_spaceWarp").equals("1"),"SpaceWarp default");
        check(defaults.Prop("r_questShadowBatching").equals("1"),"shadow batching default");
        check(defaults.Prop("r_questShadowEarlyReject").equals("1"),"shadow early rejection default");
        check(defaults.Prop("vr_rendererDiagnostics").equals("0"),"overlay default");
        check(defaults.Prop("vr_fog").equals("0"),"fog default");
        for(int mask=0;mask<512;mask++) {
            SharedPreferences prefs=new SharedPreferences();
            String[] keys={Q3EPreference.pref_vr_framebuffer_optimizations,Q3EPreference.pref_vr_mixed_precision,
                           Q3EPreference.pref_vr_renderer_diagnostics,Q3EPreference.pref_vr_fog,Q3EPreference.pref_vr_frame_memory,Q3EPreference.pref_vr_spacewarp,Q3EPreference.pref_vr_shadow_batching,Q3EPreference.pref_vr_shadow_early_reject,Q3EPreference.pref_vr_buffer_streaming};
            String[] cvars={"r_questFramebufferOptimizations","r_questMixedPrecision","vr_rendererDiagnostics","vr_fog","r_frameAllocChunks","vr_spaceWarp","r_questShadowBatching","r_questShadowEarlyReject","r_questBufferStreaming"};
            for(int bit=0;bit<9;bit++)prefs.put(keys[bit],(mask&(1<<bit))!=0);
            KidTechCommand cmd=new KidTech4Command(helper.AddVRSettings(prefs,existing));
            for(int bit=0;bit<9;bit++)
                check(cmd.Prop(cvars[bit]).equals((mask&(1<<bit))!=0?"1":"0"),"checkbox override: "+cvars[bit]);
            check(cmd.Prop("r_volumetricEnable").equals("1"),"independent volumetric setting");
            check(cmd.Prop("vr_resolutionScale").equals("1.0"),"resolution preserved");
        }
        System.out.println("PASS: production VR preferences override saved command values in all 512 switch combinations; resolution and volumetric lighting preserved");
    }
}
'''.replace('KEYS', '\n'.join(keys)).replace('MAPPING', mapping)
        source = base / 'VRSettingsHarness.java'
        source.write_text(harness)
        subprocess.run([javac, '-d', str(base), *map(str, base.rglob('*.java'))], check=True)
        subprocess.run([java, '-cp', str(base), 'VRSettingsHarness'], check=True)


if __name__ == '__main__':
    main()
