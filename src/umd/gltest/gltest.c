/*
 * gltest - OpenGL smoke test: creates a WGL context on a hidden window, prints the renderer,
 * draws a triangle into the back buffer and checks pixels with glReadPixels.
 * Run with Mesa's opengl32.dll next to it (app-local) or plain (system opengl32 + ICD).
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <GL/gl.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char path[MAX_PATH] = "?";
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (gl) GetModuleFileNameA(gl, path, sizeof path);
    printf("[INFO] opengl32.dll: %s\n", path);

    WNDCLASSA wc = { CS_OWNDC, DefWindowProcA, 0, 0, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "gltest" };
    RegisterClassA(&wc);
    HWND w = CreateWindowA("gltest", "gltest", WS_OVERLAPPEDWINDOW, 0, 0, 256, 256, NULL, NULL, wc.hInstance, NULL);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { printf("[FAIL] pixel format (%d) err %lu\n", pf, GetLastError()); return 1; }
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { printf("[FAIL] wglCreateContext/MakeCurrent err %lu\n", GetLastError()); return 1; }
    printf("[INFO] GL_VENDOR   = %s\n", (const char *)glGetString(GL_VENDOR));
    printf("[INFO] GL_RENDERER = %s\n", (const char *)glGetString(GL_RENDERER));
    printf("[INFO] GL_VERSION  = %s\n", (const char *)glGetString(GL_VERSION));

    RECT r; GetClientRect(w, &r);
    int W = r.right, H = r.bottom;
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.25f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.5f, 0.0f);
    glVertex2f(-0.5f, -0.5f); glVertex2f(0.0f, 0.5f); glVertex2f(0.5f, -0.5f);
    glEnd();
    glFinish();
    unsigned char c[4] = { 0 }, e[4] = { 0 };
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, e);
    printf("[INFO] center %02x %02x %02x, corner %02x %02x %02x\n", c[0], c[1], c[2], e[0], e[1], e[2]);
    int ok = c[0] == 0xff && c[1] >= 0x7e && c[1] <= 0x81 && c[2] == 0 && e[0] == 0 && e[1] >= 0x3f && e[1] <= 0x41 && e[2] == 0xff;
    printf("[%s] triangle and clear colour read back correctly\n", ok ? "PASS" : "FAIL");

    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (int i = 0; i < 200; i++) { glClear(GL_COLOR_BUFFER_BIT); glBegin(GL_TRIANGLES); glVertex2f(-1, -1); glVertex2f(0, 1); glVertex2f(1, -1); glEnd(); SwapBuffers(dc); }
    glFinish(); QueryPerformanceCounter(&t1);
    printf("[INFO] 200 frames (clear+draw+SwapBuffers): %.2f ms/frame\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / 200);
    wglMakeCurrent(NULL, NULL); wglDeleteContext(rc);
    printf("RESULT: %s\n", ok ? "all checks passed" : "FAILED");
    return ok ? 0 : 1;
}
