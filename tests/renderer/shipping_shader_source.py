"""Compile and run the engine's shader include/define preprocessing verbatim."""
import atexit
from functools import lru_cache
import os
from pathlib import Path
import subprocess
import tempfile
from shader_install_test import method

ROOT=Path(__file__).resolve().parents[2]
ENGINE=ROOT/'Q3E/src/main/jni/darkmod'
SHADERS=Path(os.environ.get('TDM_SHADER_SOURCE_DIR',ENGINE/'glslprogs'))

@lru_cache(maxsize=1)
def compiler():
    directory=tempfile.TemporaryDirectory(prefix='tdm-shader-preprocessor-')
    atexit.register(directory.cleanup)
    base=Path(directory.name)
    program=ENGINE/'renderer/backend/GLSLProgram.cpp'
    cpp=base/'preprocess.cpp'
    production='\n'.join(method(program,signature) for signature in (
        'struct PragmaLine {','PragmaLine FindNextPragmaInText(',
        'void ResolveIncludes(', 'void ResolveDefines('))
    # method() extracts braces; a type declaration also needs its semicolon.
    pragma=method(program,'struct PragmaLine {')
    production=production.replace(pragma,pragma+';',1)
    split=method(ENGINE/'idlib/StdString.cpp','void split(')
    cpp.write_text(r'''
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
namespace stdext {
void split(std::vector<std::string> &,const std::string &,const char * = " \n\r\t\f\v");
SPLIT
}
struct idHashMapDict {
 struct Entry{std::string value;};std::map<std::string,Entry> entries;
 const Entry *Find(const char *name)const{auto it=entries.find(name);return it==entries.end()?nullptr:&it->second;}
};
std::string root;
std::string ReadFile(const char *name){
 std::ifstream file(root+"/"+name,std::ios::binary);
 if(!file)throw std::runtime_error(std::string("Missing shader: ")+name);
 return std::string(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
}
PRODUCTION
int main(int argc,char **argv){
 if(argc<3)return 2;root=argv[1];std::string source=ReadFile(argv[2]);
 std::vector<std::string> included={argv[2]};ResolveIncludes(source,included);
 idHashMapDict defines;
 for(int i=3;i<argc;++i){std::string flag=argv[i];auto split=flag.find('=');defines.entries[flag.substr(0,split)]={flag.substr(split+1)};}
 ResolveDefines(source,defines);std::cout<<source;
}
'''.replace('SPLIT',split).replace('PRODUCTION',production))
    binary=base/'preprocess'
    subprocess.run(['g++','-std=c++14','-O2',str(cpp),'-o',str(binary)],check=True)
    return str(binary)

def source(path,defines=None):
    flags=dict(defines or {})
    flags['VERTEX_SHADER' if path.endswith(('.vert.glsl','.vs')) else 'FRAGMENT_SHADER']='1'
    return subprocess.check_output([compiler(),str(SHADERS),path,
        *(f'{key}={value}' for key,value in sorted(flags.items()))],text=True)
