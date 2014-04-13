/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2006 Lubos Lunak <l.lunak@kde.org>
Copyright (C) 2012 Martin Gräßlin <mgraesslin@kde.org>

Based on glcompmgr code by Felix Bellaby.
Using code from Compiz and Beryl.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/

// TODO: cmake magic
#ifndef KWIN_HAVE_OPENGLES
// own
#include "glxbackend.h"
// kwin
#include "options.h"
#include "utils.h"
#include "overlaywindow.h"
// kwin libs
#include <kwinglplatform.h>
#include <kwinxrenderutils.h>
// Qt
#include <QDebug>
#include <QOpenGLContext>
// system
#include <unistd.h>

#include <tuple>

namespace KWin
{
GlxBackend::GlxBackend()
    : OpenGLBackend()
    , m_overlayWindow(new OverlayWindow())
    , window(None)
    , fbconfig(NULL)
    , glxWindow(None)
    , ctx(nullptr)
    , m_bufferAge(0)
    , haveSwapInterval(false)
{
    init();
}

GlxBackend::~GlxBackend()
{
    if (isFailed()) {
        m_overlayWindow->destroy();
    }
    // TODO: cleanup in error case
    // do cleanup after initBuffer()
    cleanupGL();
    checkGLError("Cleanup");
    doneCurrent();

    if (ctx)
        glXDestroyContext(display(), ctx);

    if (glxWindow)
        glXDestroyWindow(display(), glxWindow);

    if (window)
        XDestroyWindow(display(), window);

    overlayWindow()->destroy();
    delete m_overlayWindow;
}

static bool gs_tripleBufferUndetected = true;
static bool gs_tripleBufferNeedsDetection = false;

void GlxBackend::init()
{
    initGLX();
    // require at least GLX 1.3
    if (!hasGLXVersion(1, 3)) {
        setFailed(QStringLiteral("Requires at least GLX 1.3"));
        return;
    }
    if (!initDrawableConfigs()) {
        setFailed(QStringLiteral("Could not initialize the drawable configs"));
        return;
    }
    if (!initBuffer()) {
        setFailed(QStringLiteral("Could not initialize the buffer"));
        return;
    }
    if (!initRenderingContext()) {
        setFailed(QStringLiteral("Could not initialize rendering context"));
        return;
    }

    // Initialize OpenGL
    GLPlatform *glPlatform = GLPlatform::instance();
    glPlatform->detect(GlxPlatformInterface);
    if (GLPlatform::instance()->driver() == Driver_Intel)
        options->setUnredirectFullscreen(false); // bug #252817
    options->setGlPreferBufferSwap(options->glPreferBufferSwap()); // resolve autosetting
    if (options->glPreferBufferSwap() == Options::AutoSwapStrategy)
        options->setGlPreferBufferSwap('e'); // for unknown drivers - should not happen
    glPlatform->printResults();
    initGL(GlxPlatformInterface);

    // Check whether certain features are supported
    m_haveMESACopySubBuffer = hasGLExtension(QByteArrayLiteral("GLX_MESA_copy_sub_buffer"));
    m_haveMESASwapControl   = hasGLExtension(QByteArrayLiteral("GLX_MESA_swap_control"));
    m_haveEXTSwapControl    = hasGLExtension(QByteArrayLiteral("GLX_EXT_swap_control"));
    m_haveSGISwapControl    = hasGLExtension(QByteArrayLiteral("GLX_SGI_swap_control"));

    haveSwapInterval = m_haveMESASwapControl || m_haveEXTSwapControl || m_haveSGISwapControl;

    setSupportsBufferAge(false);

    if (hasGLExtension(QByteArrayLiteral("GLX_EXT_buffer_age"))) {
        const QByteArray useBufferAge = qgetenv("KWIN_USE_BUFFER_AGE");

        if (useBufferAge != "0")
            setSupportsBufferAge(true);
    }

    setSyncsToVBlank(false);
    setBlocksForRetrace(false);
    haveWaitSync = false;
    gs_tripleBufferNeedsDetection = false;
    m_swapProfiler.init();
    const bool wantSync = options->glPreferBufferSwap() != Options::NoSwapEncourage;
    if (wantSync && glXIsDirect(display(), ctx)) {
        if (haveSwapInterval) { // glXSwapInterval is preferred being more reliable
            setSwapInterval(1);
            setSyncsToVBlank(true);
            const QByteArray tripleBuffer = qgetenv("KWIN_TRIPLE_BUFFER");
            if (!tripleBuffer.isEmpty()) {
                setBlocksForRetrace(qstrcmp(tripleBuffer, "0") == 0);
                gs_tripleBufferUndetected = false;
            }
            gs_tripleBufferNeedsDetection = gs_tripleBufferUndetected;
        } else if (hasGLExtension(QByteArrayLiteral("GLX_SGI_video_sync"))) {
            unsigned int sync;
            if (glXGetVideoSyncSGI(&sync) == 0 && glXWaitVideoSyncSGI(1, 0, &sync) == 0) {
                setSyncsToVBlank(true);
                setBlocksForRetrace(true);
                haveWaitSync = true;
            } else
                qWarning() << "NO VSYNC! glXSwapInterval is not supported, glXWaitVideoSync is supported but broken";
        } else
            qWarning() << "NO VSYNC! neither glSwapInterval nor glXWaitVideoSync are supported";
    } else {
        // disable v-sync (if possible)
        setSwapInterval(0);
    }
    if (glPlatform->isVirtualBox()) {
        // VirtualBox does not support glxQueryDrawable
        // this should actually be in kwinglutils_funcs, but QueryDrawable seems not to be provided by an extension
        // and the GLPlatform has not been initialized at the moment when initGLX() is called.
        glXQueryDrawable = NULL;
    }

    setIsDirectRendering(bool(glXIsDirect(display(), ctx)));

    qDebug() << "Direct rendering:" << isDirectRendering() << endl;
}

bool GlxBackend::initRenderingContext()
{
    const bool direct = true;

    // Use glXCreateContextAttribsARB() when it's available
    if (hasGLExtension(QByteArrayLiteral("GLX_ARB_create_context"))) {
        const int attribs_31_core_robustness[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB,               3,
            GLX_CONTEXT_MINOR_VERSION_ARB,               1,
            GLX_CONTEXT_FLAGS_ARB,                       GLX_CONTEXT_ROBUST_ACCESS_BIT_ARB,
            GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB, GLX_LOSE_CONTEXT_ON_RESET_ARB,
            0
        };

        const int attribs_31_core[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 1,
            0
        };

        const int attribs_legacy_robustness[] = {
            GLX_CONTEXT_FLAGS_ARB,                       GLX_CONTEXT_ROBUST_ACCESS_BIT_ARB,
            GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB, GLX_LOSE_CONTEXT_ON_RESET_ARB,
            0
        };

        const int attribs_legacy[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB,               1,
            GLX_CONTEXT_MINOR_VERSION_ARB,               2,
            0
        };

        const bool have_robustness = hasGLExtension(QByteArrayLiteral("GLX_ARB_create_context_robustness"));

        // Try to create a 3.1 context first
        if (options->glCoreProfile()) {
            if (have_robustness)
                ctx = glXCreateContextAttribsARB(display(), fbconfig, 0, direct, attribs_31_core_robustness);

            if (!ctx)
                ctx = glXCreateContextAttribsARB(display(), fbconfig, 0, direct, attribs_31_core);
        }

        if (!ctx && have_robustness)
            ctx = glXCreateContextAttribsARB(display(), fbconfig, 0, direct, attribs_legacy_robustness);

        if (!ctx)
            ctx = glXCreateContextAttribsARB(display(), fbconfig, 0, direct, attribs_legacy);
    }

    if (!ctx)
        ctx = glXCreateNewContext(display(), fbconfig, GLX_RGBA_TYPE, NULL, direct);

    if (!ctx) {
        qDebug() << "Failed to create an OpenGL context.";
        return false;
    }

    if (!glXMakeCurrent(display(), glxWindow, ctx)) {
        qDebug() << "Failed to make the OpenGL context current.";
        glXDestroyContext(display(), ctx);
        ctx = 0;
        return false;
    }

    return true;
}

bool GlxBackend::initBuffer()
{
    if (!initFbConfig())
        return false;

    if (overlayWindow()->create()) {
        // Try to create double-buffered window in the overlay
        XVisualInfo* visual = glXGetVisualFromFBConfig(display(), fbconfig);
        if (!visual) {
           qCritical() << "Failed to get visual from fbconfig";
           return false;
        }
        XSetWindowAttributes attrs;
        attrs.colormap = XCreateColormap(display(), rootWindow(), visual->visual, AllocNone);
        window = XCreateWindow(display(), overlayWindow()->window(), 0, 0, displayWidth(), displayHeight(),
                               0, visual->depth, InputOutput, visual->visual, CWColormap, &attrs);
        glxWindow = glXCreateWindow(display(), fbconfig, window, NULL);
        overlayWindow()->setup(window);
        XFree(visual);
    } else {
        qCritical() << "Failed to create overlay window";
        return false;
    }

    int vis_buffer;
    glXGetFBConfigAttrib(display(), fbconfig, GLX_VISUAL_ID, &vis_buffer);
    XVisualInfo* visinfo_buffer = glXGetVisualFromFBConfig(display(), fbconfig);
    qDebug() << "Buffer visual (depth " << visinfo_buffer->depth << "): 0x" << QString::number(vis_buffer, 16);
    XFree(visinfo_buffer);

    return true;
}

bool GlxBackend::initFbConfig()
{
    const int attribs[] = {
        GLX_RENDER_TYPE,    GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE,  GLX_WINDOW_BIT,
        GLX_RED_SIZE,       1,
        GLX_GREEN_SIZE,     1,
        GLX_BLUE_SIZE,      1,
        GLX_ALPHA_SIZE,     0,
        GLX_DEPTH_SIZE,     0,
        GLX_STENCIL_SIZE,   0,
        GLX_CONFIG_CAVEAT,  GLX_NONE,
        GLX_DOUBLEBUFFER,   true,
        0
    };

    // Try to find a double buffered configuration
    int count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(display(), DefaultScreen(display()), attribs, &count);

    if (count > 0) {
        fbconfig = configs[0];
        XFree(configs);
    }

    if (fbconfig == NULL) {
        qCritical() << "Failed to find a usable framebuffer configuration";
        return false;
    }

    return true;
}

bool GlxBackend::initDrawableConfigs()
{
    const int attribs[] = {
        GLX_RENDER_TYPE,    GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE,  GLX_WINDOW_BIT | GLX_PIXMAP_BIT,
        GLX_X_VISUAL_TYPE,  GLX_TRUE_COLOR,
        GLX_X_RENDERABLE,   True,
        GLX_CONFIG_CAVEAT,  int(GLX_DONT_CARE), // The ARGB32 visual is marked non-conformant in Catalyst
        GLX_RED_SIZE,       5,
        GLX_GREEN_SIZE,     5,
        GLX_BLUE_SIZE,      5,
        GLX_ALPHA_SIZE,     0,
        GLX_STENCIL_SIZE,   0,
        GLX_DEPTH_SIZE,     0,
        0
    };

    int count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(display(), DefaultScreen(display()), attribs, &count);

    if (count < 1) {
        qCritical() << "Could not find any usable framebuffer configurations.";
        return false;
    }

    for (int i = 0; i <= 32; i++) {
        fbcdrawableinfo[i].fbconfig            = NULL;
        fbcdrawableinfo[i].bind_texture_format = 0;
        fbcdrawableinfo[i].texture_targets     = 0;
        fbcdrawableinfo[i].y_inverted          = 0;
        fbcdrawableinfo[i].mipmap              = 0;
    }

    // Find the first usable framebuffer configuration for each depth.
    // Single-buffered ones will appear first in the list.
    const int depths[] = { 15, 16, 24, 30, 32 };
    for (unsigned int i = 0; i < sizeof(depths) / sizeof(depths[0]); i++) {
        const int depth = depths[i];

        for (int j = 0; j < count; j++) {
            int alpha_size, buffer_size;
            glXGetFBConfigAttrib(display(), configs[j], GLX_ALPHA_SIZE,  &alpha_size);
            glXGetFBConfigAttrib(display(), configs[j], GLX_BUFFER_SIZE, &buffer_size);

            if (buffer_size != depth && (buffer_size - alpha_size) != depth)
                continue;

            if (depth == 32 && alpha_size != 8)
                continue;

            XVisualInfo *vi = glXGetVisualFromFBConfig(display(), configs[j]);
            if (vi == NULL)
                continue;

            int visual_depth = vi->depth;
            XFree(vi);

            if (visual_depth != depth)
                continue;

            int bind_rgb, bind_rgba;
            glXGetFBConfigAttrib(display(), configs[j], GLX_BIND_TO_TEXTURE_RGBA_EXT, &bind_rgba);
            glXGetFBConfigAttrib(display(), configs[j], GLX_BIND_TO_TEXTURE_RGB_EXT,  &bind_rgb);

            // Skip this config if it cannot be bound to a texture
            if (!bind_rgb && !bind_rgba)
                continue;

            int texture_format;
            if (depth == 32)
                texture_format = bind_rgba ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT;
            else
                texture_format = bind_rgb ? GLX_TEXTURE_FORMAT_RGB_EXT : GLX_TEXTURE_FORMAT_RGBA_EXT;

            int y_inverted, texture_targets;
            glXGetFBConfigAttrib(display(), configs[j], GLX_BIND_TO_TEXTURE_TARGETS_EXT, &texture_targets);
            glXGetFBConfigAttrib(display(), configs[j], GLX_Y_INVERTED_EXT, &y_inverted);

            fbcdrawableinfo[depth].fbconfig            = configs[j];
            fbcdrawableinfo[depth].bind_texture_format = texture_format;
            fbcdrawableinfo[depth].texture_targets     = texture_targets;
            fbcdrawableinfo[depth].y_inverted          = y_inverted;
            fbcdrawableinfo[depth].mipmap              = 0;
            break;
        }
    }

    if (count)
        XFree(configs);

    if (fbcdrawableinfo[DefaultDepth(display(), DefaultScreen(display()))].fbconfig == NULL) {
        qCritical() << "Could not find a framebuffer configuration for the default depth.";
        return false;
    }

    if (fbcdrawableinfo[32].fbconfig == NULL) {
        qCritical() << "Could not find a framebuffer configuration for depth 32.";
        return false;
    }

    for (int i = 0; i <= 32; i++) {
        if (fbcdrawableinfo[i].fbconfig == NULL)
            continue;

        int vis_drawable = 0;
        glXGetFBConfigAttrib(display(), fbcdrawableinfo[i].fbconfig, GLX_VISUAL_ID, &vis_drawable);

        qDebug() << "Drawable visual (depth " << i << "): 0x" << QString::number(vis_drawable, 16);
    }

    return true;
}

FBConfigInfo *GlxBackend::infoForVisual(xcb_visualid_t visual)
{
    FBConfigInfo *&info = m_fbconfigHash[visual];

    if (info)
        return info;

    info = new FBConfigInfo;
    info->fbconfig            = nullptr;
    info->bind_texture_format = 0;
    info->texture_targets     = 0;
    info->y_inverted          = 0;
    info->mipmap              = 0;

    const xcb_render_pictformat_t format = XRenderUtils::findPictFormat(visual);
    const xcb_render_directformat_t *direct = XRenderUtils::findPictFormatInfo(format);

    if (!direct) {
        qCritical().nospace() << "Could not find a picture format for visual 0x" << hex << visual;
        return info;
    }

    const int red_bits   = bitCount(direct->red_mask);
    const int green_bits = bitCount(direct->green_mask);
    const int blue_bits  = bitCount(direct->blue_mask);
    const int alpha_bits = bitCount(direct->alpha_mask);

    const auto rgb_sizes = std::tie(red_bits, green_bits, blue_bits);

    const int attribs[] = {
        GLX_RENDER_TYPE,    GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE,  GLX_WINDOW_BIT | GLX_PIXMAP_BIT,
        GLX_X_VISUAL_TYPE,  GLX_TRUE_COLOR,
        GLX_X_RENDERABLE,   True,
        GLX_CONFIG_CAVEAT,  int(GLX_DONT_CARE), // The ARGB32 visual is marked non-conformant in Catalyst
        GLX_BUFFER_SIZE,    red_bits + green_bits + blue_bits + alpha_bits,
        GLX_RED_SIZE,       red_bits,
        GLX_GREEN_SIZE,     green_bits,
        GLX_BLUE_SIZE,      blue_bits,
        GLX_ALPHA_SIZE,     alpha_bits,
        GLX_STENCIL_SIZE,   0,
        GLX_DEPTH_SIZE,     0,
        0
    };

    int count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(display(), DefaultScreen(display()), attribs, &count);

    if (count < 1) {
        qCritical().nospace() << "Could not find a framebuffer configuration for visual 0x" << hex << visual;
        return info;
    }

    for (int i = 0; i < count; i++) {
        int red, green, blue;
        glXGetFBConfigAttrib(display(), configs[i], GLX_RED_SIZE,   &red);
        glXGetFBConfigAttrib(display(), configs[i], GLX_GREEN_SIZE, &green);
        glXGetFBConfigAttrib(display(), configs[i], GLX_BLUE_SIZE,  &blue);

        if (std::tie(red, green, blue) != rgb_sizes)
            continue;

        int bind_rgb, bind_rgba;
        glXGetFBConfigAttrib(display(), configs[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &bind_rgba);
        glXGetFBConfigAttrib(display(), configs[i], GLX_BIND_TO_TEXTURE_RGB_EXT,  &bind_rgb);

        if (!bind_rgb && !bind_rgba)
            continue;

        int texture_format;
        if (alpha_bits)
            texture_format = bind_rgba ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT;
        else
            texture_format = bind_rgb ? GLX_TEXTURE_FORMAT_RGB_EXT : GLX_TEXTURE_FORMAT_RGBA_EXT;

        int y_inverted, texture_targets;
        glXGetFBConfigAttrib(display(), configs[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT, &texture_targets);
        glXGetFBConfigAttrib(display(), configs[i], GLX_Y_INVERTED_EXT, &y_inverted);

        info->fbconfig            = configs[i];
        info->bind_texture_format = texture_format;
        info->texture_targets     = texture_targets;
        info->y_inverted          = y_inverted;
        info->mipmap              = 0;
        break;
    }

    if (count > 0)
        XFree(configs);

    if (info->fbconfig) {
        int fbc_id = 0;
        int visual_id = 0;

        glXGetFBConfigAttrib(display(), info->fbconfig, GLX_FBCONFIG_ID, &fbc_id);
        glXGetFBConfigAttrib(display(), info->fbconfig, GLX_VISUAL_ID,   &visual_id);

        qDebug().nospace() << "Using FBConfig 0x" << hex << fbc_id << " for visual 0x" << hex << visual_id;
    }

    return info;
}

void GlxBackend::setSwapInterval(int interval)
{
    if (m_haveEXTSwapControl)
        glXSwapIntervalEXT(display(), glxWindow, interval);
    else if (m_haveMESASwapControl)
        glXSwapIntervalMESA(interval);
    else if (m_haveSGISwapControl)
        glXSwapIntervalSGI(interval);
}

void GlxBackend::waitSync()
{
    // NOTE that vsync has no effect with indirect rendering
    if (haveWaitSync) {
        uint sync;
#if 0
        // TODO: why precisely is this important?
        // the sync counter /can/ perform multiple steps during glXGetVideoSync & glXWaitVideoSync
        // but this only leads to waiting for two frames??!?
        glXGetVideoSync(&sync);
        glXWaitVideoSync(2, (sync + 1) % 2, &sync);
#else
        glXWaitVideoSyncSGI(1, 0, &sync);
#endif
    }
}

void GlxBackend::present()
{
    if (lastDamage().isEmpty())
        return;

    const QRegion displayRegion(0, 0, displayWidth(), displayHeight());
    const bool fullRepaint = supportsBufferAge() || (lastDamage() == displayRegion);

    if (fullRepaint) {
        if (haveSwapInterval) {
            if (gs_tripleBufferNeedsDetection) {
                glXWaitGL();
                m_swapProfiler.begin();
            }
            glXSwapBuffers(display(), glxWindow);
            if (gs_tripleBufferNeedsDetection) {
                glXWaitGL();
                if (char result = m_swapProfiler.end()) {
                    gs_tripleBufferUndetected = gs_tripleBufferNeedsDetection = false;
                    if (result == 'd' && GLPlatform::instance()->driver() == Driver_NVidia) {
                        // TODO this is a workaround, we should get __GL_YIELD set before libGL checks it
                        if (qstrcmp(qgetenv("__GL_YIELD"), "USLEEP")) {
                            options->setGlPreferBufferSwap(0);
                            setSwapInterval(0);
                            qWarning() << "\nIt seems you are using the nvidia driver without triple buffering\n"
                                              "You must export __GL_YIELD=\"USLEEP\" to prevent large CPU overhead on synced swaps\n"
                                              "Preferably, enable the TripleBuffer Option in the xorg.conf Device\n"
                                              "For this reason, the tearing prevention has been disabled.\n"
                                              "See https://bugs.kde.org/show_bug.cgi?id=322060\n";
                        }
                    }
                    setBlocksForRetrace(result == 'd');
                }
            }
        } else {
            waitSync();
            glXSwapBuffers(display(), glxWindow);
        }
        if (supportsBufferAge()) {
            glXQueryDrawable(display(), glxWindow, GLX_BACK_BUFFER_AGE_EXT, (GLuint *) &m_bufferAge);
        }
    } else if (m_haveMESACopySubBuffer) {
        foreach (const QRect & r, lastDamage().rects()) {
            // convert to OpenGL coordinates
            int y = displayHeight() - r.y() - r.height();
            glXCopySubBufferMESA(display(), glxWindow, r.x(), y, r.width(), r.height());
        }
    } else { // Copy Pixels (horribly slow on Mesa)
        glDrawBuffer(GL_FRONT);
        SceneOpenGL::copyPixels(lastDamage());
        glDrawBuffer(GL_BACK);
    }

    setLastDamage(QRegion());
    if (!supportsBufferAge()) {
        glXWaitGL();
        XFlush(display());
    }
}

void GlxBackend::screenGeometryChanged(const QSize &size)
{
    doneCurrent();

    XMoveResizeWindow(display(), window, 0, 0, size.width(), size.height());
    overlayWindow()->setup(window);
    Xcb::sync();

    makeCurrent();
    glViewport(0, 0, size.width(), size.height());

    // The back buffer contents are now undefined
    m_bufferAge = 0;
}

SceneOpenGL::TexturePrivate *GlxBackend::createBackendTexture(SceneOpenGL::Texture *texture)
{
    return new GlxTexture(texture, this);
}

QRegion GlxBackend::prepareRenderingFrame()
{
    QRegion repaint;

    if (gs_tripleBufferNeedsDetection) {
        // the composite timer floors the repaint frequency. This can pollute our triple buffering
        // detection because the glXSwapBuffers call for the new frame has to wait until the pending
        // one scanned out.
        // So we compensate for that by waiting an extra milisecond to give the driver the chance to
        // fllush the buffer queue
        usleep(1000);
    }

    present();

    if (supportsBufferAge())
        repaint = accumulatedDamageHistory(m_bufferAge);

    startRenderTimer();
    glXWaitX();

    return repaint;
}

void GlxBackend::endRenderingFrame(const QRegion &renderedRegion, const QRegion &damagedRegion)
{
    if (damagedRegion.isEmpty()) {
        setLastDamage(QRegion());

        // If the damaged region of a window is fully occluded, the only
        // rendering done, if any, will have been to repair a reused back
        // buffer, making it identical to the front buffer.
        //
        // In this case we won't post the back buffer. Instead we'll just
        // set the buffer age to 1, so the repaired regions won't be
        // rendered again in the next frame.
        if (!renderedRegion.isEmpty())
            glFlush();

        m_bufferAge = 1;
        return;
    }

    setLastDamage(renderedRegion);

    if (!blocksForRetrace()) {
        // This also sets lastDamage to empty which prevents the frame from
        // being posted again when prepareRenderingFrame() is called.
        present();
    } else {
        // Make sure that the GPU begins processing the command stream
        // now and not the next time prepareRenderingFrame() is called.
        glFlush();
    }

    if (overlayWindow()->window())  // show the window only after the first pass,
        overlayWindow()->show();   // since that pass may take long

    // Save the damaged region to history
    if (supportsBufferAge())
        addToDamageHistory(damagedRegion);
}

bool GlxBackend::makeCurrent()
{
    if (QOpenGLContext *context = QOpenGLContext::currentContext()) {
        // Workaround to tell Qt that no QOpenGLContext is current
        context->doneCurrent();
    }
    const bool current = glXMakeCurrent(display(), glxWindow, ctx);
    return current;
}

void GlxBackend::doneCurrent()
{
    glXMakeCurrent(display(), None, nullptr);
}

OverlayWindow* GlxBackend::overlayWindow()
{
    return m_overlayWindow;
}

bool GlxBackend::usesOverlayWindow() const
{
    return true;
}

/********************************************************
 * GlxTexture
 *******************************************************/
GlxTexture::GlxTexture(SceneOpenGL::Texture *texture, GlxBackend *backend)
    : SceneOpenGL::TexturePrivate()
    , q(texture)
    , m_backend(backend)
    , m_glxpixmap(None)
{
}

GlxTexture::~GlxTexture()
{
    if (m_glxpixmap != None) {
        if (!options->isGlStrictBinding()) {
            glXReleaseTexImageEXT(display(), m_glxpixmap, GLX_FRONT_LEFT_EXT);
        }
        glXDestroyPixmap(display(), m_glxpixmap);
        m_glxpixmap = None;
    }
}

void GlxTexture::onDamage()
{
    if (options->isGlStrictBinding() && m_glxpixmap) {
        glXReleaseTexImageEXT(display(), m_glxpixmap, GLX_FRONT_LEFT_EXT);
        glXBindTexImageEXT(display(), m_glxpixmap, GLX_FRONT_LEFT_EXT, NULL);
    }
    GLTexturePrivate::onDamage();
}

void GlxTexture::findTarget()
{
    unsigned int new_target = 0;
    if (glXQueryDrawable && m_glxpixmap != None)
        glXQueryDrawable(display(), m_glxpixmap, GLX_TEXTURE_TARGET_EXT, &new_target);
    // HACK: this used to be a hack for Xgl.
    // without this hack the NVIDIA blob aborts when trying to bind a texture from
    // a pixmap icon
    if (new_target == 0) {
        if (GLTexture::NPOTTextureSupported() ||
            (isPowerOfTwo(m_size.width()) && isPowerOfTwo(m_size.height()))) {
            new_target = GLX_TEXTURE_2D_EXT;
        } else {
            new_target = GLX_TEXTURE_RECTANGLE_EXT;
        }
    }
    switch(new_target) {
    case GLX_TEXTURE_2D_EXT:
        m_target = GL_TEXTURE_2D;
        m_scale.setWidth(1.0f / m_size.width());
        m_scale.setHeight(1.0f / m_size.height());
        break;
    case GLX_TEXTURE_RECTANGLE_EXT:
        m_target = GL_TEXTURE_RECTANGLE_ARB;
        m_scale.setWidth(1.0f);
        m_scale.setHeight(1.0f);
        break;
    default:
        abort();
    }
}

bool GlxTexture::loadTexture(xcb_pixmap_t pixmap, const QSize &size, xcb_visualid_t visual)
{
    if (pixmap == XCB_NONE || size.isEmpty() || visual == XCB_NONE)
        return false;

    const FBConfigInfo *info = m_backend->infoForVisual(visual);
    if (!info || info->fbconfig == nullptr)
        return false;

    if ((info->texture_targets & GLX_TEXTURE_2D_BIT_EXT) &&
            (GLTexture::NPOTTextureSupported() ||
              (isPowerOfTwo(size.width()) && isPowerOfTwo(size.height())))) {
        m_target = GL_TEXTURE_2D;
        m_scale.setWidth(1.0f / m_size.width());
        m_scale.setHeight(1.0f / m_size.height());
    } else {
        assert(info->texture_targets & GLX_TEXTURE_RECTANGLE_BIT_EXT);

        m_target = GL_TEXTURE_RECTANGLE;
        m_scale.setWidth(1.0f);
        m_scale.setHeight(1.0f);
    }

    const int attrs[] = {
        GLX_TEXTURE_FORMAT_EXT, info->bind_texture_format,
        GLX_MIPMAP_TEXTURE_EXT, info->mipmap,
        GLX_TEXTURE_TARGET_EXT, m_target == GL_TEXTURE_2D ? GLX_TEXTURE_2D_EXT : GLX_TEXTURE_RECTANGLE_EXT,
        0
    };

    m_glxpixmap     = glXCreatePixmap(display(), info->fbconfig, pixmap, attrs);
    m_size          = size;
    m_yInverted     = info->y_inverted ? true : false;
    m_canUseMipmaps = info->mipmap;

    glGenTextures(1, &m_texture);

    q->setDirty();
    q->setFilter(info->mipmap > 0 ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST);

    glBindTexture(m_target, m_texture);
    glXBindTexImageEXT(display(), m_glxpixmap, GLX_FRONT_LEFT_EXT, nullptr);

    updateMatrix();
    return true;
}

OpenGLBackend *GlxTexture::backend()
{
    return m_backend;
}

} // namespace
#endif
