#include "shaders.h"

// Vertex shader: pass position + texcoord
const char* vertex_shader_src =
    "attribute vec2 aPos;"
    "attribute vec2 aTex;"
    "varying vec2 vTex;"
    "void main(){ vTex=aTex; gl_Position=vec4(aPos,0.0,1.0); }";

/*
  Fragment shader:
    - Decode NV12 for stream A: (uTexY_A, uTexUV_A)
    - Decode NV12 for stream B: (uTexY_B, uTexUV_B)
    - Blend with uBlend (0..1): output = mix(A, B, uBlend)
*/
const char* fragment_shader_src =
    "precision mediump float;"
    "varying vec2 vTex;"
    "uniform sampler2D uTexY_A;"
    "uniform sampler2D uTexUV_A;"
    "uniform sampler2D uTexY_B;"
    "uniform sampler2D uTexUV_B;"
    "uniform float uBlend;"
    ""
    "vec3 nv12_to_rgb(sampler2D tY, sampler2D tUV, vec2 tc){"
    "  float y  = texture2D(tY,  tc).r;"
    "  vec4  uv = texture2D(tUV, tc);"
    "  float u = uv.r - 0.5;"
    "  float v = uv.a - 0.5;"
    "  float Y = 1.1643 * (y - 0.0625);"   // video range
    "  float R = Y + 1.7927 * v;"
    "  float G = Y - 0.2132 * u - 0.5329 * v;"
    "  float B = Y + 2.1124 * u;"
    "  return vec3(R,G,B);"
    "}"
    ""
    "void main(){"
    "  vec2 tc = vec2(vTex.x, 1.0 - vTex.y);"
    "  vec3 A = nv12_to_rgb(uTexY_A, uTexUV_A, tc);"
    "  vec3 B = nv12_to_rgb(uTexY_B, uTexUV_B, tc);"
    "  vec3 C = mix(A, B, clamp(uBlend, 0.0, 1.0));"
    "  gl_FragColor = vec4(C, 1.0);"
    "}";

GLuint compile_shader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char buffer[1024];
        glGetShaderInfoLog(shader, sizeof(buffer), NULL, buffer);
        printf("Shader compile error: %s\n", buffer);
        fflush(stdout);
    }
    return shader;
}
