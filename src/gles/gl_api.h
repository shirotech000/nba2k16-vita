#pragma once
// prototypes of every GL entry point we hand the game. anything taking a
// float by value is SOFTFP since the game is softfp and we're hardfp.
#include "gles/gl_defs.h"
#include "util/abi.h"

void glActiveTexture(GLenum tex);
void glAttachShader(GLuint prog, GLuint sh);
void glBindAttribLocation(GLuint prog, GLuint idx, const GLchar *name);
void glBindBuffer(GLenum target, GLuint buf);
void glBindFramebuffer(GLenum target, GLuint fb);
void glBindRenderbuffer(GLenum target, GLuint rb);
void glBindTexture(GLenum target, GLuint tex);
void glBlendEquation(GLenum mode);
void glBlendEquationSeparate(GLenum rgb, GLenum a);
void glBlendFunc(GLenum src, GLenum dst);
void glBlendFuncSeparate(GLenum srgb, GLenum drgb, GLenum sa, GLenum da);
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data);
void glClear(GLbitfield mask);
SOFTFP void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
SOFTFP void glClearDepthf(GLfloat d);
void glClearStencil(GLint s);
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glCompileShader(GLuint sh);
void glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                            GLint border, GLsizei size, const void *data);
GLuint glCreateProgram(void);
GLuint glCreateShader(GLenum type);
void glCullFace(GLenum mode);
void glDeleteBuffers(GLsizei n, const GLuint *bufs);
void glDeleteProgram(GLuint prog);
void glDeleteRenderbuffers(GLsizei n, const GLuint *rbs);
void glDeleteShader(GLuint sh);
void glDeleteTextures(GLsizei n, const GLuint *texs);
void glDepthFunc(GLenum f);
void glDepthMask(GLboolean m);
void glDisable(GLenum cap);
void glDisableVertexAttribArray(GLuint idx);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei inst);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx);
void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *idx, GLsizei inst);
void glEnable(GLenum cap);
void glEnableVertexAttribArray(GLuint idx);
void glFinish(void);
void glFlush(void);
void glFramebufferRenderbuffer(GLenum target, GLenum att, GLenum rbtarget, GLuint rb);
void glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level);
void glFrontFace(GLenum mode);
void glGenBuffers(GLsizei n, GLuint *out);
void glGenFramebuffers(GLsizei n, GLuint *out);
void glGenRenderbuffers(GLsizei n, GLuint *out);
void glGenTextures(GLsizei n, GLuint *out);
void glGetProgramiv(GLuint prog, GLenum pname, GLint *out);
const GLubyte *glGetString(GLenum name);
GLint glGetUniformLocation(GLuint prog, const GLchar *name);
void glInvalidateFramebuffer(GLenum target, GLsizei n, const GLenum *atts);
void glLinkProgram(GLuint prog);
void glPixelStorei(GLenum pname, GLint v);
SOFTFP void glPolygonOffset(GLfloat factor, GLfloat units);
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *out);
void glRenderbufferStorage(GLenum target, GLenum ifmt, GLsizei w, GLsizei h);
void glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h);
void glShaderSource(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len);
void glStencilFunc(GLenum f, GLint ref, GLuint mask);
void glStencilFuncSeparate(GLenum face, GLenum f, GLint ref, GLuint mask);
void glStencilMask(GLuint m);
void glStencilMaskSeparate(GLenum face, GLuint m);
void glStencilOp(GLenum sf, GLenum dpf, GLenum dpp);
void glStencilOpSeparate(GLenum face, GLenum sf, GLenum dpf, GLenum dpp);
void glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                  GLenum fmt, GLenum type, const void *data);
void glTexParameteri(GLenum target, GLenum pname, GLint v);
SOFTFP void glUniform1f(GLint loc, GLfloat x);
SOFTFP void glUniform2f(GLint loc, GLfloat x, GLfloat y);
SOFTFP void glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z);
SOFTFP void glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glUniform1fv(GLint loc, GLsizei n, const GLfloat *v);
void glUniform2fv(GLint loc, GLsizei n, const GLfloat *v);
void glUniform3fv(GLint loc, GLsizei n, const GLfloat *v);
void glUniform4fv(GLint loc, GLsizei n, const GLfloat *v);
void glUniform1i(GLint loc, GLint x);
void glUniform2i(GLint loc, GLint x, GLint y);
void glUniform3i(GLint loc, GLint x, GLint y, GLint z);
void glUniform4i(GLint loc, GLint x, GLint y, GLint z, GLint w);
void glUniform1iv(GLint loc, GLsizei n, const GLint *v);
void glUniform2iv(GLint loc, GLsizei n, const GLint *v);
void glUniform3iv(GLint loc, GLsizei n, const GLint *v);
void glUniform4iv(GLint loc, GLsizei n, const GLint *v);
void glUniformMatrix2fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v);
void glUniformMatrix3fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v);
void glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v);
void glUseProgram(GLuint prog);
void glVertexAttribPointer(GLuint idx, GLint size, GLenum type, GLboolean norm, GLsizei stride,
                           const void *ptr);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);

// not imported, cheap extras for dlsym/eglGetProcAddress style lookups
GLenum glGetError(void);
void glGetIntegerv(GLenum pname, GLint *out);
void glGetFloatv(GLenum pname, GLfloat *out);
void glGetBooleanv(GLenum pname, GLboolean *out);
void glGetShaderiv(GLuint sh, GLenum pname, GLint *out);
void glGetShaderInfoLog(GLuint sh, GLsizei max, GLsizei *len, GLchar *log);
void glGetProgramInfoLog(GLuint prog, GLsizei max, GLsizei *len, GLchar *log);
GLint glGetAttribLocation(GLuint prog, const GLchar *name);
void glDeleteFramebuffers(GLsizei n, const GLuint *fbs);
GLenum glCheckFramebufferStatus(GLenum target);
void glGenerateMipmap(GLenum target);
void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                     GLenum fmt, GLenum type, const void *data);
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
GLboolean glIsEnabled(GLenum cap);
void glHint(GLenum target, GLenum mode);
SOFTFP void glTexParameterf(GLenum target, GLenum pname, GLfloat v);
SOFTFP void glLineWidth(GLfloat w);
SOFTFP void glDepthRangef(GLfloat n, GLfloat f);
