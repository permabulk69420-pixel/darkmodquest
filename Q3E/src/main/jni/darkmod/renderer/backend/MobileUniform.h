/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once
#include "renderer/backend/GLSLUniforms.h"
#include <vector>
#include <cstring>
#include <type_traits>

// Private to the mobile programs: no other uniform group writes their values.
// Program reload destroys these groups and their cached values together.
template<class Scalar, int Components>
class MobileUniform : public GLSLUniformBase {
	std::vector<Scalar> previous;
public:
	MobileUniform(GLSLProgram *program,const char *name):GLSLUniformBase(program,name) {}
	void SetArray(int count,const Scalar *value) {
		if (!IsPresent()) return;
		const size_t length=size_t(count)*Components;
		if(previous.size()==length && !std::memcmp(previous.data(),value,length*sizeof(Scalar))) return;
		previous.assign(value,value+length);
		if(Components==16) qglUniformMatrix4fv(paramLocation,count,GL_FALSE,reinterpret_cast<const GLfloat *>(value));
		else if(std::is_same<Scalar,int>::value) {
			const GLint *p=reinterpret_cast<const GLint *>(value);
			if(Components==1) qglUniform1iv(paramLocation,count,p);
			if(Components==2) qglUniform2iv(paramLocation,count,p);
			if(Components==3) qglUniform3iv(paramLocation,count,p);
			if(Components==4) qglUniform4iv(paramLocation,count,p);
		} else {
			const GLfloat *p=reinterpret_cast<const GLfloat *>(value);
			if(Components==1) qglUniform1fv(paramLocation,count,p);
			if(Components==2) qglUniform2fv(paramLocation,count,p);
			if(Components==3) qglUniform3fv(paramLocation,count,p);
			if(Components==4) qglUniform4fv(paramLocation,count,p);
		}
	}
	void Set(const Scalar *p) { SetArray(1,p); }
	template<class Vector> auto Set(const Vector &value) -> decltype(value.ToFloatPtr(),void()) { Set(value.ToFloatPtr()); }
	void Set(Scalar value) { static_assert(Components==1,"scalar uniform");SetArray(1,&value); }
	void Set(Scalar x,Scalar y) { static_assert(Components==2,"vec2 uniform");Scalar p[]={x,y};SetArray(1,p); }
	void Set(Scalar x,Scalar y,Scalar z) { static_assert(Components==3,"vec3 uniform");Scalar p[]={x,y,z};SetArray(1,p); }
	void Set(Scalar x,Scalar y,Scalar z,Scalar w) { static_assert(Components==4,"vec4 uniform");Scalar p[]={x,y,z,w};SetArray(1,p); }
};
using MobileUniform_int=MobileUniform<int,1>;
using MobileUniform_sampler=MobileUniform<int,1>;
using MobileUniform_ivec3=MobileUniform<int,3>;
using MobileUniform_float=MobileUniform<float,1>;
using MobileUniform_vec2=MobileUniform<float,2>;
using MobileUniform_vec3=MobileUniform<float,3>;
using MobileUniform_vec4=MobileUniform<float,4>;
using MobileUniform_mat4=MobileUniform<float,16>;
#define DEFINE_MOBILE_UNIFORM(type,name) MobileUniform_##type name{program,"u_" #name};
