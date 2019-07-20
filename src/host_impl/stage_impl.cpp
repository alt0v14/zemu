// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com

#include <string>
#include <stdexcept>
#include "stage_impl.h"
#include "driver/sound_driver_generic.h"

#ifdef __unix__
    #include "driver/sound_driver_oss.h"
#endif

#ifdef _WIN32
    #include <SDL_syswm.h>
    #include "driver/sound_driver_win32.h"
#endif

int stageImplRenderThreadFunction(void* data) {
    static_cast<StageImpl*>(data)->renderThreadLoop();
    return 0;
}

StageImpl::StageImpl(const StageConfig& stageConfig, Logger* logger) {
    #ifdef USE_SDL1
        uint32_t flags = SDL_INIT_VIDEO;
    #else
        uint32_t flags = SDL_INIT_EVENTS | SDL_INIT_VIDEO;
    #endif

    if (stageConfig.soundDriver == STAGE_SOUND_DRIVER_GENERIC) {
        flags |= SDL_INIT_AUDIO;
    }

    if (stageConfig.withJoystick) {
        flags |= SDL_INIT_JOYSTICK;
    }

    if (SDL_InitSubSystem(flags) < 0) {
        throw std::runtime_error(std::string("SDL_InitSubSystem() failed: ") + SDL_GetError());
    }

    if (stageConfig.withJoystick) {
        SDL_JoystickEventState(SDL_ENABLE);
    }

    hints = stageConfig.hints;
    renderMode = stageConfig.renderMode;
    lastFrameWidth = stageConfig.desiredFrameWidth;
    lastFrameHeight = stageConfig.desiredFrameHeight;
    fullscreen = stageConfig.fullscreen;

    #ifndef USE_SDL1
        stageTitle = stageConfig.title;
    #endif

    renderThreadPixelsReadySem = SDL_CreateSemaphore(0);

    if (!renderThreadPixelsReadySem) {
        throw std::runtime_error(std::string("SDL_CreateSemaphore() failed: ") + SDL_GetError());
    }

    refreshVideoSubsystem();

    #ifdef USE_SDL1
        SDL_WM_SetCaption(stageConfig.title.c_str(), stageConfig.title.c_str());
        SDL_EnableKeyRepeat(0, SDL_DEFAULT_REPEAT_INTERVAL);
    #endif

    // #ifdef USE_SDL1
    //     SDL_ShowCursor(SDL_DISABLE);
    //     SDL_WM_GrabInput(SDL_GRAB_ON);
    // #else
    //     SDL_SetRelativeMouseMode(SDL_TRUE);
    // #endif

    switch (stageConfig.soundDriver) {
        case STAGE_SOUND_DRIVER_NONE:
            soundEnabled = false;
            break;

        case STAGE_SOUND_DRIVER_GENERIC:
            soundEnabled = true;

            soundDriver.reset(new SoundDriverGeneric(
                stageConfig.soundFreq,
                stageConfig.soundParams[0],
                stageConfig.soundParams[1],
                stageConfig.soundParams[2]
            ));
            break;

        case STAGE_SOUND_DRIVER_NATIVE:
            #ifdef __unix__
                soundEnabled = true;

                soundDriver.reset(new SoundDriverOss(
                    stageConfig.soundFreq,
                    stageConfig.soundParams[0],
                    stageConfig.soundParams[1]
                ));
            #elif _WIN32
                soundEnabled = true;
                soundDriver.reset(new SoundDriverWin32(stageConfig.soundFreq, stageConfig.soundParams[0]));
            #else
                soundEnabled = false;
            #endif

            break;
    }

    #ifdef _WIN32
        if (!stageConfig.windowsIconResource) {
            return;
        }

        SDL_SysWMinfo sysWmInfo;
        SDL_VERSION(&sysWmInfo.version);

        #ifdef USE_SDL1
            if ((int result = SDL_GetWMInfo(&sysWmInfo)) != 1) {
                logger->log("SDL_GetWMInfo() failed: %d", result);
                return;
            }
        #else
            if ((int result = SDL_GetWindowWMInfo(nativeWindow, &sysWmInfo)) != SDL_TRUE) {
                logger->log("SDL_GetWindowWMInfo() failed: %d", result);
                return;
            }
        #endif

        windowsIcon = ::LoadIcon(::GetModuleHandle(nullptr), MAKEINTRESOURCE(stageConfig.windowsIconResource));

        if (!windowsIcon) {
            logger->log("LoadIcon() failed: %d", GetLastError());
            return;
        }

        ::SetClassLongPtr(sysWmInfo.window, GCLP_HICON, (LONG_PTR)windowsIcon);
    #endif
}

StageImpl::~StageImpl() {
    if (isRenderThreadActive && renderThread) {
        isRenderThreadActive = false;
        SDL_SemPost(renderThreadPixelsReadySem);
        SDL_WaitThread(renderThread, nullptr);
    }

    if (renderThreadPixelsReadySem) {
        SDL_DestroySemaphore(renderThreadPixelsReadySem);
    }

    if (renderThreadPixels) {
        delete[] renderThreadPixels;
    }

    if (nativeSurface) {
        setFullscreen(false);
        SDL_FreeSurface(nativeSurface);
    }

    #ifndef USE_SDL1
        if (nativeTexture) {
            SDL_DestroyTexture(nativeTexture);
        }

        if (nativeRenderer) {
            SDL_DestroyRenderer(nativeRenderer);
        }

        if (nativeWindow) {
            SDL_DestroyWindow(nativeWindow);
        }
    #endif

    #ifdef _WIN32
        if (windowsIcon) {
            ::DestroyIcon(windowsIcon)
        }
    #endif
}

StageRenderMode StageImpl::getRenderMode() {
    return renderMode;
}

void StageImpl::setRenderMode(StageRenderMode renderMode) { //-V688
    if (this->renderMode == renderMode) {
        return;
    }

    StageRenderMode prevRenderMode = this->renderMode;
    this->renderMode = renderMode;

    if ((prevRenderMode == STAGE_RENDER_MODE_1X && renderMode != STAGE_RENDER_MODE_1X)
        || (prevRenderMode != STAGE_RENDER_MODE_1X && renderMode == STAGE_RENDER_MODE_1X)
    ) {
        refreshVideoSubsystem();
    }
}

bool StageImpl::isKeyRepeat() {
    return keyRepeat;
}

void StageImpl::setKeyRepeat(bool keyRepeat) {
    this->keyRepeat = keyRepeat;

    #ifdef USE_SDL1
        SDL_EnableKeyRepeat(keyRepeat ? SDL_DEFAULT_REPEAT_DELAY : 0, SDL_DEFAULT_REPEAT_INTERVAL);
    #endif
}

bool StageImpl::isFullscreen() {
    return fullscreen;
}

void StageImpl::setFullscreen(bool fullscreen) { //-V688
    if (this->fullscreen == fullscreen) {
        return;
    }

    this->fullscreen = fullscreen;

    #ifdef USE_SDL1
        if (!SDL_WM_ToggleFullScreen(nativeSurface)) {
            refreshVideoSubsystem();
        }
    #else
        SDL_SetWindowFullscreen(nativeWindow, fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    #endif
}

bool StageImpl::isSoundEnabled() {
    return soundEnabled;
}

void StageImpl::setSoundEnabled(bool soundEnabled) {
    if (soundDriver) {
        this->soundEnabled = soundEnabled;
    }
}

bool StageImpl::pollEvent(StageEvent* into) {
    return false;
}

void StageImpl::getMouseState(StageMouseState* into) {
    into->buttons = SDL_GetRelativeMouseState(&into->x, &into->y);
}

void StageImpl::renderFrame(uint32_t* pixels, int width, int height) {
    if (!isRenderThreadActive || !isRenderThreadPixelsConsumed || SDL_SemValue(renderThreadPixelsReadySem)) {
        return;
    }

    if (lastFrameWidth != width || lastFrameHeight != height) {
        refreshVideoSubsystem();
    }

    isRenderThreadPixelsConsumed = false;
    memcpy((void*)renderThreadPixels, (void*)pixels, width * height * sizeof(uint32_t));
    SDL_SemPost(renderThreadPixelsReadySem);
}

void StageImpl::renderSound(uint32_t* buffer, int samples) {
    if (soundEnabled && soundDriver) {
        soundDriver->render(buffer, samples);
    }
}

void StageImpl::refreshVideoSubsystem() {
    bool wasRenderThreadActive = isRenderThreadActive;

    if (wasRenderThreadActive) {
        isRenderThreadActive = false;
        SDL_SemPost(renderThreadPixelsReadySem);
        SDL_WaitThread(renderThread, nullptr);

        if (renderThreadPixels) {
            delete[] renderThreadPixels;
            renderThreadPixels = nullptr;
        }
    }

    int width = lastFrameWidth;
    int height = lastFrameHeight;

    if (renderMode != STAGE_RENDER_MODE_1X) {
        width *= 2;
        height *= 2;
    }

    #ifdef USE_SDL1
        if (nativeSurface) {
            SDL_FreeSurface(nativeSurface);
        }

        uint32_t flags = SDL_SWSURFACE;

        if (fullscreen) {
            flags |= SDL_FULLSCREEN;
        }

        if (hints & STAGE_HINT_FLIP_SURFACE) {
            flags |= SDL_DOUBLEBUF;
        }

        nativeSurface = SDL_SetVideoMode(width, height, 32, flags);

        if (!nativeSurface) {
            throw std::runtime_error(std::string("SDL_SetVideoMode() failed: ") + SDL_GetError());
        }
    #else
        if (nativeSurface) {
            SDL_FreeSurface(nativeSurface);
        }

        if (nativeTexture) {
            SDL_DestroyTexture(nativeTexture);
        }

        if (nativeRenderer) {
            SDL_DestroyRenderer(nativeRenderer);
        }

        if (nativeWindow) {
            SDL_DestroyWindow(nativeWindow);
        }

        nativeWindow = SDL_CreateWindow(
            stageTitle.c_str(),
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            width,
            height,
            fullscreen ? SDL_WINDOW_FULLSCREEN : 0
        );

        if (!nativeWindow) {
            throw std::runtime_error(std::string("SDL_CreateWindow() failed: ") + SDL_GetError());
        }

        nativeRenderer = SDL_CreateRenderer(nativeWindow, -1, 0);

        if (!nativeRenderer) {
            throw std::runtime_error(std::string("SDL_CreateRenderer() failed: ") + SDL_GetError());
        }

        nativeTexture = SDL_CreateTexture(nativeRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);

        if (!nativeTexture) {
            throw std::runtime_error(std::string("SDL_CreateTexture() failed: ") + SDL_GetError());
        }

        nativeSurface = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);

        if (!nativeSurface) {
            throw std::runtime_error(std::string("SDL_CreateRGBSurface() failed: ") + SDL_GetError());
        }
    #endif

    if (wasRenderThreadActive) {
        renderThreadPixels = new uint32_t[lastFrameWidth * lastFrameHeight];

        #ifdef USE_SDL1
            renderThread = SDL_CreateThread(stageImplRenderThreadFunction, (void*)this);
        #else
            renderThread = SDL_CreateThread(stageImplRenderThreadFunction, nullptr, (void*)this);
        #endif

        if (!renderThread) {
            throw std::runtime_error(std::string("SDL_CreateThread() failed: ") + SDL_GetError());
        }

        isRenderThreadPixelsConsumed = true;
    }
}

void StageImpl::renderThreadLoop() {
    while (isRenderThreadActive) {
        SDL_SemWait(renderThreadPixelsReadySem);

        if (!renderThreadPixels || (SDL_MUSTLOCK(nativeSurface) && SDL_LockSurface(nativeSurface) < 0)) {
            return;
        }

        switch (renderMode) {
            case STAGE_RENDER_MODE_1X:
                renderThreadUpdateSurface1x();
                break;

            case STAGE_RENDER_MODE_2X:
                renderThreadUpdateSurface2x();
                break;

            case STAGE_RENDER_MODE_2X_SCANLINES:
                renderThreadUpdateSurface2xScanlines();
                break;
        }

        isRenderThreadPixelsConsumed = true;

        #ifdef USE_SDL1
            if (SDL_MUSTLOCK(nativeSurface)) {
                SDL_UnlockSurface(nativeSurface);
            }

            if (hints & STAGE_HINT_FLIP_SURFACE) {
                SDL_Flip(nativeSurface);
            } else {
                SDL_UpdateRect(nativeSurface, 0, 0, 0, 0);
            }
        #else
            SDL_UpdateTexture(nativeTexture, nullptr, nativeSurface->pixels, nativeSurface->pitch);

            if (SDL_MUSTLOCK(nativeSurface)) {
                SDL_UnlockSurface(nativeSurface);
            }

            SDL_RenderClear(nativeRenderer);
            SDL_RenderCopy(nativeRenderer, nativeTexture, nullptr, nullptr);
            SDL_RenderPresent(nativeRenderer);
        #endif
    }
}

void StageImpl::renderThreadUpdateSurface1x() {
    uint32_t* src = renderThreadPixels;
    uint8_t* dst = (uint8_t*)nativeSurface->pixels;

    for (int y = lastFrameHeight; y--;) {
        memcpy((void*)dst, (void*)src, lastFrameWidth * sizeof(uint32_t));
        dst += nativeSurface->pitch;
    }
}

void StageImpl::renderThreadUpdateSurface2x() {
    uint32_t* src = renderThreadPixels;
    uint8_t* dst = (uint8_t*)nativeSurface->pixels;

    for (int y = lastFrameHeight; y--;) {
        uint32_t* dstA = (uint32_t*)dst;
        uint32_t* dstB = (uint32_t*)(dst + nativeSurface->pitch);

        for (int x = lastFrameWidth; x--;) {
            int c = *(src++);

            *(dstA++) = c;
            *(dstA++) = c;

            *(dstB++) = c;
            *(dstB++) = c;
        }

        dst += nativeSurface->pitch * 2;
    }
}

void StageImpl::renderThreadUpdateSurface2xScanlines() {
    uint32_t* src = renderThreadPixels;
    uint8_t* dst = (uint8_t*)nativeSurface->pixels;

    for (int y = lastFrameHeight; y--;) {
        uint32_t* dstA = (uint32_t*)dst;
        uint32_t* dstB = (uint32_t*)(dst + nativeSurface->pitch);

        for (int x = lastFrameWidth; x--;) {
            int ca = *(src++);
            int cb = (ca & 0xFEFEFE) >> 1;

            *(dstA++) = ca;
            *(dstA++) = ca;

            *(dstB++) = cb;
            *(dstB++) = cb;
        }

        dst += nativeSurface->pitch * 2;
    }
}
