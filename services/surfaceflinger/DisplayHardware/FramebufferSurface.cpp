/*
 **
 ** Copyright 2012 The Android Open Source Project
 **
 ** Licensed under the Apache License Version 2.0(the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing software
 ** distributed under the License is distributed on an "AS IS" BASIS
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <cutils/log.h>

#include <utils/String8.h>

#include <ui/Rect.h>

#include <EGL/egl.h>

#include <hardware/hardware.h>
#include <gui/SurfaceTextureClient.h>
#include <ui/GraphicBuffer.h>

#include "DisplayHardware/FramebufferSurface.h"
#include "DisplayHardware/HWComposer.h"

// ----------------------------------------------------------------------------
namespace android {
// ----------------------------------------------------------------------------

class GraphicBufferAlloc : public BnGraphicBufferAlloc {
public:
    GraphicBufferAlloc() { };
    virtual ~GraphicBufferAlloc() { };
    virtual sp<GraphicBuffer> createGraphicBuffer(uint32_t w, uint32_t h,
            PixelFormat format, uint32_t usage, status_t* error) {
        sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(w, h, format, usage));
        return graphicBuffer;
    }
};


/*
 * This implements the (main) framebuffer management. This class is used
 * mostly by SurfaceFlinger, but also by command line GL application.
 *
 */

FramebufferSurface::FramebufferSurface(HWComposer& hwc) :
    ConsumerBase(new BufferQueue(true, new GraphicBufferAlloc())),
    mCurrentBufferSlot(-1),
    mCurrentBuffer(0),
    mHwc(hwc)
{
    mName = "FramebufferSurface";
    mBufferQueue->setConsumerName(mName);
    mBufferQueue->setConsumerUsageBits(GRALLOC_USAGE_HW_FB |
                                       GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER);
    mBufferQueue->setDefaultBufferFormat(mHwc.getFormat(HWC_DISPLAY_PRIMARY));
    mBufferQueue->setDefaultBufferSize(mHwc.getWidth(HWC_DISPLAY_PRIMARY),
                                       mHwc.getHeight(HWC_DISPLAY_PRIMARY));
    mBufferQueue->setSynchronousMode(true);
    mBufferQueue->setDefaultMaxBufferCount(NUM_FRAME_BUFFERS);
}

status_t FramebufferSurface::nextBuffer(sp<GraphicBuffer>& outBuffer, sp<Fence>& outFence) {
    Mutex::Autolock lock(mMutex);

    BufferQueue::BufferItem item;
    status_t err = acquireBufferLocked(&item);
    if (err == BufferQueue::NO_BUFFER_AVAILABLE) {
        outBuffer = mCurrentBuffer;
        return NO_ERROR;
    } else if (err != NO_ERROR) {
        ALOGE("error acquiring buffer: %s (%d)", strerror(-err), err);
        return err;
    }

    // If the BufferQueue has freed and reallocated a buffer in mCurrentSlot
    // then we may have acquired the slot we already own.  If we had released
    // our current buffer before we call acquireBuffer then that release call
    // would have returned STALE_BUFFER_SLOT, and we would have called
    // freeBufferLocked on that slot.  Because the buffer slot has already
    // been overwritten with the new buffer all we have to do is skip the
    // releaseBuffer call and we should be in the same state we'd be in if we
    // had released the old buffer first.
    if (mCurrentBufferSlot != BufferQueue::INVALID_BUFFER_SLOT &&
        item.mBuf != mCurrentBufferSlot) {
        // Release the previous buffer.
        err = releaseBufferLocked(mCurrentBufferSlot, EGL_NO_DISPLAY,
                EGL_NO_SYNC_KHR);
        if (err != NO_ERROR && err != BufferQueue::STALE_BUFFER_SLOT) {
            ALOGE("error releasing buffer: %s (%d)", strerror(-err), err);
            return err;
        }
    }
    mCurrentBufferSlot = item.mBuf;
    mCurrentBuffer = mSlots[mCurrentBufferSlot].mGraphicBuffer;
    outFence = item.mFence;
    outBuffer = mCurrentBuffer;
    return NO_ERROR;
}

// Overrides ConsumerBase::onFrameAvailable(), does not call base class impl.
void FramebufferSurface::onFrameAvailable() {
    sp<GraphicBuffer> buf;
    sp<Fence> acquireFence;
    status_t err = nextBuffer(buf, acquireFence);
    if (err != NO_ERROR) {
        ALOGE("error latching nnext FramebufferSurface buffer: %s (%d)",
                strerror(-err), err);
        return;
    }
    err = mHwc.fbPost(0, acquireFence, buf); // FIXME: use real display id
    if (err != NO_ERROR) {
        ALOGE("error posting framebuffer: %d", err);
    }
}

void FramebufferSurface::freeBufferLocked(int slotIndex) {
    ConsumerBase::freeBufferLocked(slotIndex);
    if (slotIndex == mCurrentBufferSlot) {
        mCurrentBufferSlot = BufferQueue::INVALID_BUFFER_SLOT;
    }
}

status_t FramebufferSurface::setReleaseFenceFd(int fenceFd) {
    status_t err = NO_ERROR;
    if (fenceFd >= 0) {
        sp<Fence> fence(new Fence(fenceFd));
        if (mCurrentBufferSlot != BufferQueue::INVALID_BUFFER_SLOT) {
            status_t err = addReleaseFence(mCurrentBufferSlot, fence);
            ALOGE_IF(err, "setReleaseFenceFd: failed to add the fence: %s (%d)",
                    strerror(-err), err);
        }
    }
    return err;
}

status_t FramebufferSurface::setUpdateRectangle(const Rect& r)
{
    return INVALID_OPERATION;
}

status_t FramebufferSurface::compositionComplete()
{
    return mHwc.fbCompositionComplete();
}

void FramebufferSurface::dump(String8& result) {
    mHwc.fbDump(result);
    ConsumerBase::dump(result);
}

// ----------------------------------------------------------------------------
}; // namespace android
// ----------------------------------------------------------------------------
