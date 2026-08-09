//
// Created by LiDon on 2025/9/25.
//
#pragma once

#include <cocos2d.h>
#include "Platform.h"

using TVPPostUpdateCallback = void (*)();

// This must be shared by every translation unit.  A namespace-scope
// `static inline` variable gives each .cpp its own callback slot, so the
// OpenGL renderer can register the restore callback in one TU while
// MainScene sees a different (null) slot.
inline TVPPostUpdateCallback &TVPGetPostUpdateCallbackSlot() {
    static TVPPostUpdateCallback callback = nullptr;
    return callback;
}

inline void TVPSetPostUpdateEvent(TVPPostUpdateCallback f) {
    TVPGetPostUpdateCallbackSlot() = f;
}

inline void TVPInvokePostUpdateEvent() {
    TVPPostUpdateCallback f = TVPGetPostUpdateCallbackSlot();
    if(f)
        f();
}

inline int TVPDrawSceneOnce(int interval) {
    static tjs_uint64 lastTick = TVPGetRoughTickCount32();
    tjs_uint64 curTick = TVPGetRoughTickCount32();
    int remain = interval - (curTick - lastTick);
    if(remain <= 0) {
        TVPInvokePostUpdateEvent();
        auto *director = cocos2d::Director::getInstance();
        director->drawScene();
        TVPForceSwapBuffer();
        lastTick = curTick;
        return 0;
    }
    return remain;
}