#include "gl_extra.h"

// Note: APIENTRY is defined by imgui_impl_opengl3_loader.h (included in the
// translation units that use these), but gl_extra.cpp is standalone so we
// define it here if missing.
#ifndef APIENTRY
#if defined(_WIN32)
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif

namespace {

template <typename T>
void load(GLExtraGetProcAddress get_proc, const char* name, T& ptr) {
    ptr = reinterpret_cast<T>(get_proc(name));
}

} // namespace

PFNGLGENFRAMEBUFFERSPROC    glGenFramebuffers = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = nullptr;
PFNGLBINDFRAMEBUFFERPROC    glBindFramebuffer = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
PFNGLGENRENDERBUFFERSPROC    glGenRenderbuffers = nullptr;
PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers = nullptr;
PFNGLBINDRENDERBUFFERPROC    glBindRenderbuffer = nullptr;
PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage = nullptr;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer = nullptr;
PFNGLGENERATEMIPMAPPROC glGenerateMipmap = nullptr;
PFNGLDEPTHFUNCPROC glDepthFunc = nullptr;

bool gl_extra_init(GLExtraGetProcAddress get_proc) {
    load(get_proc, "glGenFramebuffers", glGenFramebuffers);
    load(get_proc, "glDeleteFramebuffers", glDeleteFramebuffers);
    load(get_proc, "glBindFramebuffer", glBindFramebuffer);
    load(get_proc, "glFramebufferTexture2D", glFramebufferTexture2D);
    load(get_proc, "glGenRenderbuffers", glGenRenderbuffers);
    load(get_proc, "glDeleteRenderbuffers", glDeleteRenderbuffers);
    load(get_proc, "glBindRenderbuffer", glBindRenderbuffer);
    load(get_proc, "glRenderbufferStorage", glRenderbufferStorage);
    load(get_proc, "glFramebufferRenderbuffer", glFramebufferRenderbuffer);
    load(get_proc, "glGenerateMipmap", glGenerateMipmap);
    load(get_proc, "glDepthFunc", glDepthFunc);
    return glGenFramebuffers && glDeleteFramebuffers && glBindFramebuffer &&
           glFramebufferTexture2D && glGenRenderbuffers && glDeleteRenderbuffers &&
           glBindRenderbuffer && glRenderbufferStorage && glFramebufferRenderbuffer &&
           glGenerateMipmap && glDepthFunc;
}
