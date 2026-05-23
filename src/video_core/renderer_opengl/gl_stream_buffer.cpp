// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/alignment.h"
#include "common/assert.h"
#include "common/microprofile.h"
#include "video_core/renderer_opengl/gl_driver.h"
#include "video_core/renderer_opengl/gl_stream_buffer.h"
#ifdef __SWITCH__
#include "video_core/renderer_opengl/gl_switch_compat.h"

// Switch-Mesa's eglGetProcAddress doesn't expose glBufferStorage even though
// the extension string advertises GL_ARB_buffer_storage. Devkit-A64 has no
// dlfcn, so use weak references: if Mesa's static lib provides the symbol,
// the linker resolves it and the address is non-null; otherwise it stays
// null and we fall through to glBufferData.
extern "C" {
__attribute__((weak)) void glBufferStorage(GLenum target, GLsizeiptr size, const void* data,
                                           GLbitfield flags);
__attribute__((weak)) void glBufferStorageEXT(GLenum target, GLsizeiptr size, const void* data,
                                              GLbitfield flags);
}

namespace Azahar::Switch {
bool AppendLogFormat(int* error_out, const char* format, ...);
}
#endif

MICROPROFILE_DEFINE(OpenGL_StreamBuffer, "OpenGL", "Stream Buffer Orphaning",
                    MP_RGB(128, 128, 192));

namespace OpenGL {

#ifdef __SWITCH__
namespace {
using BufferStorageProc = void (*)(GLenum, GLsizeiptr, const void*, GLbitfield);

struct BufferStorageResolution {
    BufferStorageProc proc;
    const char* source;
};

// Switch-Mesa advertises GL_ARB_buffer_storage in the extension string but its
// eglGetProcAddress only returns symbols for the currently-bound API class, so
// glBufferStorage / glBufferStorageARB / glBufferStorageEXT all come back null
// even though the underlying entry point is statically linked in. Fall back to
// dlsym against the running image, which can see the symbol directly.
BufferStorageResolution GetBufferStorageProc() {
    static const BufferStorageResolution resolved = [] {
        static constexpr const char* kNames[] = {
            "glBufferStorage",
            "glBufferStorageARB",
            "glBufferStorageEXT",
        };
        for (const char* name : kNames) {
            if (auto p = reinterpret_cast<BufferStorageProc>(eglGetProcAddress(name))) {
                return BufferStorageResolution{p, "egl"};
            }
        }
        if (&glBufferStorage != nullptr) {
            return BufferStorageResolution{&glBufferStorage, "weak"};
        }
        if (&glBufferStorageEXT != nullptr) {
            return BufferStorageResolution{&glBufferStorageEXT, "weak-ext"};
        }
        return BufferStorageResolution{nullptr, "none"};
    }();
    return resolved;
}
} // namespace
#endif

OGLStreamBuffer::OGLStreamBuffer(Driver& driver, GLenum target, GLsizeiptr size,
                                 bool prefer_coherent)
    : gl_target(target), buffer_size(size) {
    gl_buffer.Create();
    glBindBuffer(gl_target, gl_buffer.handle);

    GLsizeiptr allocate_size = size;
    if (driver.HasBug(DriverBug::VertexArrayOutOfBound) && target == GL_ARRAY_BUFFER) {
        allocate_size *= 2;
    }

#ifdef __SWITCH__
    const bool has_buffer_storage = driver.HasArbBufferStorage() || driver.HasExtBufferStorage();
    const auto resolved =
        has_buffer_storage && GL_MAP_PERSISTENT_BIT != 0 && GL_MAP_COHERENT_BIT != 0
            ? GetBufferStorageProc()
            : BufferStorageResolution{nullptr, "skipped"};
    const auto buffer_storage = resolved.proc;
    if (buffer_storage != nullptr) {
#else
    if (driver.HasArbBufferStorage()) {
#endif
        persistent = true;
        coherent = prefer_coherent;
        GLbitfield flags =
            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | (coherent ? GL_MAP_COHERENT_BIT : 0);
#ifdef __SWITCH__
        buffer_storage(gl_target, allocate_size, nullptr, flags);
#else
        glBufferStorage(gl_target, allocate_size, nullptr, flags);
#endif
        mapped_ptr = static_cast<u8*>(glMapBufferRange(
            gl_target, 0, buffer_size, flags | (coherent ? 0 : GL_MAP_FLUSH_EXPLICIT_BIT)));
    } else {
        glBufferData(gl_target, allocate_size, nullptr, GL_STREAM_DRAW);
    }
#ifdef __SWITCH__
    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=opengl.stream-buffer target=%u size=%lld allocate=%lld "
        "persistent=%u coherent=%u arb=%u ext=%u proc=%u proc_src=%s",
        static_cast<unsigned>(gl_target), static_cast<long long>(buffer_size),
        static_cast<long long>(allocate_size), persistent ? 1U : 0U, coherent ? 1U : 0U,
        driver.HasArbBufferStorage() ? 1U : 0U, driver.HasExtBufferStorage() ? 1U : 0U,
        buffer_storage != nullptr ? 1U : 0U, resolved.source);
#endif
}

OGLStreamBuffer::~OGLStreamBuffer() {
    if (persistent) {
        glBindBuffer(gl_target, gl_buffer.handle);
        glUnmapBuffer(gl_target);
    }
    gl_buffer.Release();
}

GLuint OGLStreamBuffer::GetHandle() const {
    return gl_buffer.handle;
}

GLsizeiptr OGLStreamBuffer::GetSize() const {
    return buffer_size;
}

std::tuple<u8*, GLintptr, bool> OGLStreamBuffer::Map(GLsizeiptr size, GLintptr alignment) {
    ASSERT_MSG(size <= buffer_size, "Requested size {} exceeds buffer size {}", size, buffer_size);
    ASSERT(alignment <= buffer_size);
    mapped_size = size;

    if (alignment > 0) {
        buffer_pos = Common::AlignUp<std::size_t>(buffer_pos, alignment);
    }

    bool invalidate = false;
    if (buffer_pos + size > buffer_size) {
        buffer_pos = 0;
        invalidate = true;

        if (persistent) {
            glUnmapBuffer(gl_target);
        }
    }

    if (invalidate || !persistent) {
        MICROPROFILE_SCOPE(OpenGL_StreamBuffer);
        GLbitfield flags = GL_MAP_WRITE_BIT | (persistent ? GL_MAP_PERSISTENT_BIT : 0) |
                           (coherent ? GL_MAP_COHERENT_BIT : GL_MAP_FLUSH_EXPLICIT_BIT) |
                           (invalidate ? GL_MAP_INVALIDATE_BUFFER_BIT : GL_MAP_UNSYNCHRONIZED_BIT);
        mapped_ptr = static_cast<u8*>(
            glMapBufferRange(gl_target, buffer_pos, buffer_size - buffer_pos, flags));
        mapped_offset = buffer_pos;
    }

    return std::make_tuple(mapped_ptr + buffer_pos - mapped_offset, buffer_pos, invalidate);
}

void OGLStreamBuffer::Unmap(GLsizeiptr size) {
    ASSERT(size <= mapped_size);

    if (!coherent && size > 0) {
        glFlushMappedBufferRange(gl_target, buffer_pos - mapped_offset, size);
    }

    if (!persistent) {
        glUnmapBuffer(gl_target);
    }

    buffer_pos += size;
}

} // namespace OpenGL
