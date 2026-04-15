#include "shaders.h"

#include <stdio.h>
#include <GLES2/gl2.h>

GLuint compile_shader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
        fflush(stderr);
    }
    return shader;
}

const char* vertex_shader_src =
    "attribute vec2 aPos;"
    "attribute vec2 aTex;"
    "varying vec2 vTex;"
    "void main(){"
    "  vTex = aTex;"
    "  gl_Position = vec4(aPos, 0.0, 1.0);"
    "}";

const char* fragment_shader_src =
    "precision mediump float;"
    "varying vec2 vTex;"
    "uniform sampler2D uTexY;"
    "uniform sampler2D uTexU;"
    "uniform sampler2D uTexV;"
    "uniform int uVideoRange;"
    "uniform int uBT709;"
    "uniform float uAlpha;"

    "vec3 yuv_to_rgb(float y, float u, float v) {"
    "  float Y = (uVideoRange==1) ? (1.1643 * (y - 0.0625)) : y;"
    "  float R; float G; float B;"
    "  if (uBT709==1) {"
    "    R = Y + 1.7927 * v;"
    "    G = Y - 0.2132 * u - 0.5329 * v;"
    "    B = Y + 2.1124 * u;"
    "  } else {"
    "    R = Y + 1.4020 * v;"
    "    G = Y - 0.3441 * u - 0.7141 * v;"
    "    B = Y + 1.7720 * u;"
    "  }"
    "  return vec3(R, G, B);"
    "}"

    "void main(){"
    "  vec2 tc = vec2(vTex.x, 1.0 - vTex.y);"
    "  float y = texture2D(uTexY, tc).r;"
    "  float u = texture2D(uTexU, tc).r - 0.5;"
    "  float v = texture2D(uTexV, tc).r - 0.5;"
    "  vec3 rgb = clamp(yuv_to_rgb(y, u, v), 0.0, 1.0);"
    "  gl_FragColor = vec4(rgb, uAlpha);"
    "}";
