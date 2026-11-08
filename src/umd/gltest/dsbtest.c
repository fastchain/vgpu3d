/*
 * dsbtest - OpenGL dual-source blending (ARB_blend_func_extended) check: draws a full-window quad
 * with a fragment shader that writes index 0 and index 1 outputs, under several blend modes, and
 * compares the read-back pixel with the expected value.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <GL/gl.h>

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_SRC1_COLOR 0x88F9
#define GL_ONE_MINUS_SRC1_COLOR 0x88FA
#define GL_SRC1_ALPHA 0x8589
#define GL_ONE_MINUS_SRC1_ALPHA 0x88FB
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x2

#define FN(ret, name, args) typedef ret(APIENTRY *PFN_##name) args; static PFN_##name p##name;
FN(GLuint, glCreateShader, (GLenum))
FN(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
FN(void, glCompileShader, (GLuint))
FN(void, glGetShaderiv, (GLuint, GLenum, GLint *))
FN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
FN(GLuint, glCreateProgram, (void))
FN(void, glAttachShader, (GLuint, GLuint))
FN(void, glLinkProgram, (GLuint))
FN(void, glGetProgramiv, (GLuint, GLenum, GLint *))
FN(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
FN(void, glUseProgram, (GLuint))
FN(void, glBindFragDataLocationIndexed, (GLuint, GLuint, GLuint, const GLchar *))
FN(void, glGenBuffers, (GLsizei, GLuint *))
FN(void, glBindBuffer, (GLenum, GLuint))
FN(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))
FN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
FN(void, glEnableVertexAttribArray, (GLuint))
FN(void, glGenVertexArrays, (GLsizei, GLuint *))
FN(void, glBindVertexArray, (GLuint))
FN(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))
typedef HGLRC(WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);

#define LOAD(name) p##name = (PFN_##name)wglGetProcAddress(#name)

static const char *vs =
    "#version 330\n"
    "layout(location = 0) in vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";

/* explicit indices (GLSL 3.30 + ARB_blend_func_extended style) */
static const char *fs_layout =
    "#version 330\n"
    "layout(location = 0, index = 0) out vec4 c0;\n"
    "layout(location = 0, index = 1) out vec4 c1;\n"
    "void main() { c0 = vec4(1.0, 0.5, 0.25, 1.0); c1 = vec4(0.5, 0.25, 0.75, 0.5); }\n";

/* indices bound from the API (glBindFragDataLocationIndexed), as older engines do */
static const char *fs_api =
    "#version 330\n"
    "out vec4 c0;\n"
    "out vec4 c1;\n"
    "void main() { c0 = vec4(1.0, 0.5, 0.25, 1.0); c1 = vec4(0.5, 0.25, 0.75, 0.5); }\n";

static GLuint
shader(GLenum type, const char *src)
{
    GLuint s = pglCreateShader(type);
    pglShaderSource(s, 1, &src, NULL);
    pglCompileShader(s);
    GLint ok = 0;
    pglGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        pglGetShaderInfoLog(s, sizeof log, NULL, log);
        printf("[FAIL] shader compile: %s\n", log);
    }
    return s;
}

static GLuint
program(const char *fs, int api_bind)
{
    GLuint p = pglCreateProgram();
    pglAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    pglAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    if (api_bind) {
        pglBindFragDataLocationIndexed(p, 0, 0, "c0");
        pglBindFragDataLocationIndexed(p, 0, 1, "c1");
    }
    pglLinkProgram(p);
    GLint ok = 0;
    pglGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        pglGetProgramInfoLog(p, sizeof log, NULL, log);
        printf("[FAIL] link: %s\n", log);
    }
    return p;
}

static const float clear[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
static const float c0[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
static const float c1[4] = { 0.5f, 0.25f, 0.75f, 0.5f };

static int
check(const char *name, const float expect[4], int W, int H)
{
    unsigned char px[4];
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int ok = 1;
    for (int i = 0; i < 4; i++)
        if (fabsf(px[i] - expect[i] * 255.0f) > 3.0f)
            ok = 0;
    printf("[%s] %-40s got %3u %3u %3u %3u expect %3.0f %3.0f %3.0f %3.0f\n", ok ? "PASS" : "FAIL", name, px[0], px[1],
           px[2], px[3], expect[0] * 255, expect[1] * 255, expect[2] * 255, expect[3] * 255);
    return ok;
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    WNDCLASSA wc = { CS_OWNDC, DefWindowProcA, 0, 0, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "dsbtest" };
    RegisterClassA(&wc);
    HWND w = CreateWindowA("dsbtest", "dsbtest", WS_OVERLAPPEDWINDOW, 0, 0, 256, 256, NULL, NULL, wc.hInstance, NULL);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cAlphaBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC legacy = wglCreateContext(dc);
    wglMakeCurrent(dc, legacy);
    PFN_wglCreateContextAttribsARB create =
        (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    const int attrs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                          WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, 0 };
    HGLRC rc = create ? create(dc, NULL, attrs) : NULL;
    if (rc) {
        wglMakeCurrent(dc, rc);
        wglDeleteContext(legacy);
    }
    printf("[INFO] GL_RENDERER = %s\n", (const char *)glGetString(GL_RENDERER));
    printf("[INFO] GL_VERSION  = %s\n", (const char *)glGetString(GL_VERSION));
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    printf("[INFO] ARB_blend_func_extended: %s\n", ext && strstr(ext, "GL_ARB_blend_func_extended") ? "yes" : "no");

    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader); LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog); LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glLinkProgram);
    LOAD(glGetProgramiv); LOAD(glGetProgramInfoLog); LOAD(glUseProgram); LOAD(glBindFragDataLocationIndexed);
    LOAD(glGenBuffers); LOAD(glBindBuffer); LOAD(glBufferData); LOAD(glVertexAttribPointer);
    LOAD(glEnableVertexAttribArray); LOAD(glGenVertexArrays); LOAD(glBindVertexArray); LOAD(glBlendFuncSeparate);

    RECT r;
    GetClientRect(w, &r);
    int W = r.right, H = r.bottom;
    glViewport(0, 0, W, H);
    GLuint vao, vbo;
    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    pglGenVertexArrays(1, &vao);
    pglBindVertexArray(vao);
    pglGenBuffers(1, &vbo);
    pglBindBuffer(GL_ARRAY_BUFFER, vbo);
    pglBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    pglVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    pglEnableVertexAttribArray(0);

    GLuint progs[2] = { program(fs_layout, 0), program(fs_api, 1) };
    const char *pname[2] = { "layout(index)", "BindFragDataLocationIndexed" };
    int fails = 0;
    for (int k = 0; k < 2; k++) {
        pglUseProgram(progs[k]);
        char name[96];
        float e[4];

        /* A: blending off -> index 0 output */
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        snprintf(name, sizeof name, "%s, no blend", pname[k]);
        fails += !check(name, c0, W, H);

        /* B: ordinary alpha blend (dual outputs declared, not used by the blend) */
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        snprintf(name, sizeof name, "%s, SRC_ALPHA blend", pname[k]);
        fails += !check(name, c0, W, H); /* c0.a = 1 */

        /* C: dual-source: src*SRC1_COLOR + dst*(1-SRC1_COLOR), alpha ONE/ZERO */
        glClear(GL_COLOR_BUFFER_BIT);
        pglBlendFuncSeparate(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR, GL_ONE, GL_ZERO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        for (int i = 0; i < 3; i++)
            e[i] = c0[i] * c1[i] + clear[i] * (1.0f - c1[i]);
        e[3] = c0[3];
        snprintf(name, sizeof name, "%s, SRC1_COLOR blend", pname[k]);
        fails += !check(name, e, W, H);

        /* D: dual-source alpha: src*SRC1_ALPHA + dst*(1-SRC1_ALPHA) */
        glClear(GL_COLOR_BUFFER_BIT);
        pglBlendFuncSeparate(GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA, GL_ONE, GL_ZERO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        for (int i = 0; i < 3; i++)
            e[i] = c0[i] * c1[3] + clear[i] * (1.0f - c1[3]);
        e[3] = c0[3];
        snprintf(name, sizeof name, "%s, SRC1_ALPHA blend", pname[k]);
        fails += !check(name, e, W, H);
    }
    printf("[%s] %d failures, GL error 0x%x\n", fails ? "FAIL" : "PASS", fails, glGetError());
    return fails ? 1 : 0;
}
