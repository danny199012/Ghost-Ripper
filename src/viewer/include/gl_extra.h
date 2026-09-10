// Supplemental OpenGL loader: the ImGui backend ships a *stripped* GL loader
// (imgui_impl_opengl3_loader.h) that only covers the functions ImGui itself
// uses. Our 3D viewport additionally needs FBOs, depth buffers, mipmaps and a
// few extra enums. This header declares just those, loaded via the same
// GLFW proc-address callback the app uses for ImGui.
//
// Call gl_extra_init() once after the OpenGL context is current and after
// ImGui_ImplOpenGL3_Init() (which populates ImGui's loader).
#pragma once

#include <cstdint>

// APIENTRY: __stdcall on Windows, nothing elsewhere. The imgui loader defines
// it too, but we include gl_extra.h before that header in some TUs, so define
// it here (guarded so it won't conflict).
#ifndef APIENTRY
#if defined(_WIN32)
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif

// ---- extra GL enums not present in imgui_impl_opengl3_loader.h -----------
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR 0x8137
#endif
#ifndef GL_LINE
#define GL_LINE 0x1B01
#endif
#ifndef GL_DEPTH_BUFFER_BIT
#define GL_DEPTH_BUFFER_BIT 0x00000100
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

// ---- function pointer types ----------------------------------------------
using PFNGLGENFRAMEBUFFERSPROC    = void (APIENTRY*)(int, unsigned int*);
using PFNGLDELETEFRAMEBUFFERSPROC = void (APIENTRY*)(int, const unsigned int*);
using PFNGLBINDFRAMEBUFFERPROC    = void (APIENTRY*)(unsigned int, unsigned int);
using PFNGLFRAMEBUFFERTEXTURE2DPROC =
    void (APIENTRY*)(unsigned int, unsigned int, unsigned int, unsigned int, int);
using PFNGLGENRENDERBUFFERSPROC    = void (APIENTRY*)(int, unsigned int*);
using PFNGLDELETERENDERBUFFERSPROC = void (APIENTRY*)(int, const unsigned int*);
using PFNGLBINDRENDERBUFFERPROC    = void (APIENTRY*)(unsigned int, unsigned int);
using PFNGLRENDERBUFFERSTORAGEPROC =
    void (APIENTRY*)(unsigned int, unsigned int, int, int);
using PFNGLFRAMEBUFFERRENDERBUFFERPROC =
    void (APIENTRY*)(unsigned int, unsigned int, unsigned int, unsigned int);
using PFNGLGENERATEMIPMAPPROC = void (APIENTRY*)(unsigned int);
using PFNGLDEPTHFUNCPROC      = void (APIENTRY*)(unsigned int);

// ---- extern pointers (defined in gl_extra.cpp) ---------------------------
extern PFNGLGENFRAMEBUFFERSPROC    glGenFramebuffers;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC    glBindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
extern PFNGLGENRENDERBUFFERSPROC    glGenRenderbuffers;
extern PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers;
extern PFNGLBINDRENDERBUFFERPROC    glBindRenderbuffer;
extern PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
extern PFNGLGENERATEMIPMAPPROC glGenerateMipmap;
extern PFNGLDEPTHFUNCPROC      glDepthFunc;

// Callback type matching GLFWglproc (void* (*)(const char*)).
using GLExtraGetProcAddress = void* (*)(const char*);

// Loads the extra function pointers. Returns false if any are missing.
bool gl_extra_init(GLExtraGetProcAddress get_proc);
