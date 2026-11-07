#!/usr/bin/env python3
"""Run the shipping Android ZIP extractor on the shipping shader archive.

Only Android Context/AssetManager and logging are stubbed. The extraction and
file-copy method bodies are compiled verbatim from production Java sources.
Requires a JDK. SHADER_INSTALL_APK optionally checks the archive inside an APK.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
JAVA = ROOT / 'Q3E/src/main/java/com/n0n3m4/q3e'
ARCHIVE = ROOT / 'Q3E/src/main/assets/pak/darkmod/glprogs.pk4'
SHADERS = ROOT / 'Q3E/src/main/jni/darkmod/glslprogs'


def method(path, signature):
    source = path.read_text()
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class ShaderInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='tdm-shader-install-')
        cls.base = Path(cls.temp.name)
        jdk = Path(os.environ.get('JAVA_HOME', '')) / 'bin'
        cls.java = str(jdk / 'java') if (jdk / 'java').is_file() else shutil.which('java')
        javac = str(jdk / 'javac') if (jdk / 'javac').is_file() else shutil.which('javac')
        if not cls.java or not javac:
            raise RuntimeError('A JDK is required to test the production Android extractor')
        extractor = method(JAVA / 'Q3EContextUtils.java', 'public static boolean ExtractZip(')
        utils = '\n'.join(method(JAVA / 'Q3EUtils.java', signature) for signature in (
            'public static void Close(Closeable', 'public static boolean mkdir(',
            'public static long Copy(OutputStream', 'public static long Write(String filePath, InputStream'))
        harness = '''
import java.io.*;
import java.util.zip.*;
class Context {
    public Context getAssets() { return this; }
    public InputStream open(String path) throws IOException { return new FileInputStream(path); }
}
class Log { public static void i(String tag, String message) {} }
class Q3EGlobals { public static final String CONST_Q3E_LOG_TAG = "test"; }
class Q3EUtils { UTILS }
public class ShaderInstallHarness {
    EXTRACTOR
    public static void main(String[] args) {
        if (!ExtractZip(new Context(), args[0], args[1], Boolean.parseBoolean(args[2])))
            throw new AssertionError("The shipping shader extractor failed");
    }
}
'''.replace('UTILS', utils).replace('EXTRACTOR', extractor)
        source = cls.base / 'ShaderInstallHarness.java'
        source.write_text(harness)
        subprocess.run([javac, '-d', str(cls.base), str(source)], check=True, capture_output=True)
        cls.archive = cls.base / 'glprogs.pk4'
        if os.environ.get('SHADER_INSTALL_APK'):
            with zipfile.ZipFile(os.environ['SHADER_INSTALL_APK']) as apk:
                cls.archive.write_bytes(apk.read('assets/pak/darkmod/glprogs.pk4'))
        else:
            cls.archive.write_bytes(ARCHIVE.read_bytes())

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def extract(self, archive, target, overwrite=True):
        result = subprocess.run([self.java, '-cp', str(self.base), 'ShaderInstallHarness',
                                 str(archive), str(target), str(overwrite).lower()],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def check_sources(self, target):
        for path in SHADERS.rglob('*'):
            if path.is_file():
                installed = target / 'glslprogs' / path.relative_to(SHADERS)
                self.assertTrue(installed.is_file(), f'Missing shader: {installed}')
                self.assertEqual(installed.read_bytes(), path.read_bytes(), str(installed))

    def test_fresh_install(self):
        target = Path(os.environ.get('TDM_SHADER_INSTALL_DIR', self.base / 'fresh'))
        self.extract(self.archive, target)
        self.check_sources(target)

    def test_upgrade_creates_new_mobile_shader_directory(self):
        target = self.base / 'upgrade'
        # Reproduce an installation with every pre-mobile shader directory.
        for path in SHADERS.rglob('*'):
            relative = path.relative_to(SHADERS)
            if path.is_file() and relative.parts[:2] != ('stages', 'mobile'):
                (target / 'glslprogs' / relative.parent).mkdir(parents=True, exist_ok=True)
        stale = target / 'glslprogs/stages/depth/depth.vert.glsl'
        stale.write_text('old installed shader')
        self.extract(self.archive, target)
        self.check_sources(target)

    def test_extract_archive_without_directory_entries(self):
        # ZIP directory records are optional. Existing files must survive an
        # overwrite=false extraction, and a subsequent upgrade must replace them.
        archive = self.base / 'files-only.pk4'
        with zipfile.ZipFile(archive, 'w') as output:
            output.writestr('glslprogs/stages/mobile/forward.vert.glsl', 'first')
        target = self.base / 'files-only'
        self.extract(archive, target)
        installed = target / 'glslprogs/stages/mobile/forward.vert.glsl'
        with zipfile.ZipFile(archive, 'w') as output:
            output.writestr('glslprogs/stages/mobile/forward.vert.glsl', 'second')
        self.extract(archive, target, overwrite=False)
        self.assertEqual(installed.read_text(), 'first')
        self.extract(archive, target)
        self.assertEqual(installed.read_text(), 'second')


if __name__ == '__main__':
    unittest.main(verbosity=2)
