#pragma once
#include <GLES2/gl2.h>

extern const char* vertex_shader_src;
extern const char* fragment_shader_src;

GLuint compile_shader(GLenum type, const char* src);
