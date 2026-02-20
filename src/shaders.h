#pragma once
#include "common.h"

extern const char* vertex_shader_src;
extern const char* fragment_shader_src;

GLuint compile_shader(GLenum type, const char* src);
