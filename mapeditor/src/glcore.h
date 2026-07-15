// Minimal OpenGL 3.3 core function loader (no glad/glew dependency).
// GL 1.1 entry points come from the system GL (declared via GLFW's <GL/gl.h>);
// the modern (1.5+/2.0+) ones are loaded through glfwGetProcAddress. C++17 inline
// variables keep everything in this header — call loadGLCore() once after making
// a context current.
#pragma once
#include <GLFW/glfw3.h>
#include <cstddef>

#ifndef APIENTRY
#define APIENTRY
#endif

// types not present in GL 1.1 headers
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

// enums not in GL 1.1
#define GL_ARRAY_BUFFER              0x8892
#define GL_ELEMENT_ARRAY_BUFFER      0x8893
#define GL_STATIC_DRAW               0x88E4
#define GL_FRAGMENT_SHADER           0x8B30
#define GL_VERTEX_SHADER             0x8B31
#define GL_COMPILE_STATUS            0x8B81
#define GL_LINK_STATUS               0x8B82
#define GL_VERTEX_PROGRAM_POINT_SIZE 0x8642
#define GL_FRAMEBUFFER               0x8D40
#define GL_RENDERBUFFER              0x8D41
#define GL_COLOR_ATTACHMENT0         0x8CE0
#define GL_DEPTH_ATTACHMENT          0x8D00
#define GL_DEPTH_COMPONENT24         0x81A6
#define GL_FRAMEBUFFER_COMPLETE      0x8CD5
#ifndef GL_TEXTURE0
#define GL_TEXTURE0                  0x84C0
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                     0x8058
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE             0x812F   // GL 1.2; absent from ancient Windows gl.h
#endif
#ifndef GL_POLYGON_OFFSET_FILL
#define GL_POLYGON_OFFSET_FILL       0x8037
#endif

#define GL_FUNCS(X) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void,   glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void,   glCompileShader, (GLuint)) \
    X(void,   glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void,   glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void,   glDeleteShader, (GLuint)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void,   glAttachShader, (GLuint, GLuint)) \
    X(void,   glLinkProgram, (GLuint)) \
    X(void,   glGetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void,   glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void,   glUseProgram, (GLuint)) \
    X(void,   glDeleteProgram, (GLuint)) \
    X(GLint,  glGetUniformLocation, (GLuint, const GLchar*)) \
    X(void,   glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*)) \
    X(void,   glUniform3fv, (GLint, GLsizei, const GLfloat*)) \
    X(void,   glUniform1f, (GLint, GLfloat)) \
    X(void,   glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void,   glUniform1i, (GLint, GLint)) \
    X(void,   glUniform1iv, (GLint, GLsizei, const GLint*)) \
    X(void,   glUniform1fv, (GLint, GLsizei, const GLfloat*)) \
    X(void,   glActiveTexture, (GLenum)) \
    X(void,   glGenBuffers, (GLsizei, GLuint*)) \
    X(void,   glDeleteBuffers, (GLsizei, const GLuint*)) \
    X(void,   glBindBuffer, (GLenum, GLuint)) \
    X(void,   glBufferData, (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(void,   glGenVertexArrays, (GLsizei, GLuint*)) \
    X(void,   glDeleteVertexArrays, (GLsizei, const GLuint*)) \
    X(void,   glBindVertexArray, (GLuint)) \
    X(void,   glEnableVertexAttribArray, (GLuint)) \
    X(void,   glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void,   glGenFramebuffers, (GLsizei, GLuint*)) \
    X(void,   glBindFramebuffer, (GLenum, GLuint)) \
    X(void,   glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void,   glGenRenderbuffers, (GLsizei, GLuint*)) \
    X(void,   glBindRenderbuffer, (GLenum, GLuint)) \
    X(void,   glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void,   glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum))

#define GL_DECL(ret, name, args) typedef ret (APIENTRY *PFN_##name) args; inline PFN_##name name = nullptr;
GL_FUNCS(GL_DECL)
#undef GL_DECL

inline bool loadGLCore() {
    bool ok = true;
#define CPCW_GL_LOAD(ret, name, args) \
    name = (PFN_##name)glfwGetProcAddress(#name); if (!name) ok = false;
    GL_FUNCS(CPCW_GL_LOAD)
#undef CPCW_GL_LOAD
    return ok;
}
