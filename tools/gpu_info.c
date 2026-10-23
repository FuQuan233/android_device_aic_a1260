/* SPDX-License-Identifier: Apache-2.0 */
/* Live GPU identification and a completed offscreen draw for bring-up. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>

static GLuint shader(GLenum type, const char *source)
{
    GLuint result = glCreateShader(type);
    glShaderSource(result, 1, &source, NULL);
    glCompileShader(result);
    GLint compiled = 0;
    glGetShaderiv(result, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024];
        glGetShaderInfoLog(result, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compilation failed: %s\n", log);
        glDeleteShader(result);
        return 0;
    }
    return result;
}

int main(void)
{
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor, count;
    EGLConfig config;
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    if (!eglInitialize(display, &major, &minor) ||
        !eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) {
        fprintf(stderr, "EGL initialization/configuration failed: %#x\n", eglGetError());
        return 1;
    }
    const EGLint surface_attributes[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (!eglMakeCurrent(display, surface, surface, context)) {
        fprintf(stderr, "EGL make-current failed: %#x\n", eglGetError());
        return 1;
    }
    printf("EGL=%d.%d vendor=%s\nGL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\n",
           major, minor, eglQueryString(display, EGL_VENDOR), glGetString(GL_VENDOR),
           glGetString(GL_RENDERER), glGetString(GL_VERSION));
    GLuint vs = shader(GL_VERTEX_SHADER, "attribute vec2 position; void main() { gl_Position=vec4(position,0.0,1.0); }");
    GLuint fs = shader(GL_FRAGMENT_SHADER, "precision mediump float; void main() { gl_FragColor=vec4(0.0,1.0,0.0,1.0); }");
    if (!vs || !fs) return 1;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        fprintf(stderr, "Shader link failed\n");
        return 1;
    }
    const GLfloat vertices[] = {-1,-1, 3,-1, -1,3};
    glUseProgram(program);
    glViewport(0, 0, 64, 64);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char pixel[4] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glFinish();
    GLenum error = glGetError();
    printf("DRAW_RGBA=%u,%u,%u,%u GL_ERROR=%#x\n", pixel[0], pixel[1], pixel[2], pixel[3], error);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return error != GL_NO_ERROR || pixel[0] != 0 || pixel[1] != 255 || pixel[2] != 0 || pixel[3] != 255;
}
