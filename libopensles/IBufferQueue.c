/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* BufferQueue implementation */

#include "sles_allinclusive.h"


/** Determine the state of the audio player or audio recorder associated with a buffer queue.
 *  Note that PLAYSTATE and RECORDSTATE values are equivalent (where PLAYING == RECORDING).
 */

static SLuint32 getAssociatedState(IBufferQueue *this)
{
    SLuint32 state;
    switch (InterfaceToObjectID(this)) {
    case SL_OBJECTID_AUDIOPLAYER:
        state = ((CAudioPlayer *) this->mThis)->mPlay.mState;
        break;
    case SL_OBJECTID_AUDIORECORDER:
        state = ((CAudioRecorder *) this->mThis)->mRecord.mState;
        break;
    default:
        // unreachable, but just in case we will assume it is stopped
        assert(SL_BOOLEAN_FALSE);
        state = SL_PLAYSTATE_STOPPED;
        break;
    }
    return state;
}

/** \brief Read one stereo frame from source buffer, handling mono→stereo and 8→16 conversion.
 *  Returns left and right as int16 values via pointers.
 */
static void read_source_frame(const void *pBuffer, SLuint32 frameIndex,
    int channels, int bps, int16_t *outL, int16_t *outR)
{
    if (bps == 8) {
        const uint8_t *src = (const uint8_t *)pBuffer;
        if (channels == 2) {
            *outL = ((int16_t)src[frameIndex * 2]     - 0x80) << 8;
            *outR = ((int16_t)src[frameIndex * 2 + 1] - 0x80) << 8;
        } else {
            int16_t s = ((int16_t)src[frameIndex] - 0x80) << 8;
            *outL = s;
            *outR = s;
        }
    } else {
        const int16_t *src = (const int16_t *)pBuffer;
        if (channels == 2) {
            *outL = src[frameIndex * 2];
            *outR = src[frameIndex * 2 + 1];
        } else {
            *outL = src[frameIndex];
            *outR = *outL;
        }
    }
}


SLresult IBufferQueue_Enqueue(SLBufferQueueItf self, const void *pBuffer, SLuint32 size)
{
    SL_ENTER_INTERFACE
    //SL_LOGV("IBufferQueue_Enqueue(%p, %p, %lu)", self, pBuffer, size);

    // Note that Enqueue while a Clear is pending is equivalent to Enqueue followed by Clear

    if (NULL == pBuffer || 0 == size) {
        result = SL_RESULT_PARAMETER_INVALID;
    } else {
        IBufferQueue *this = (IBufferQueue *) self;
        interface_lock_exclusive(this);
        BufferHeader *oldRear = this->mRear, *newRear;
        if ((newRear = oldRear + 1) == &this->mArray[this->mNumBuffers + 1]) {
            newRear = this->mArray;
        }
        if (newRear == this->mFront) {
            result = SL_RESULT_BUFFER_INSUFFICIENT;
        } else {
            if (this->samplerate == 0) {
                // SndFile source: already in correct format, just copy
                void *copiedBuffer = calloc(1, size);
                if (copiedBuffer == NULL) {
                    result = SL_RESULT_MEMORY_FAILURE;
                } else {
                    memcpy(copiedBuffer, pBuffer, size);
                    oldRear->mBuffer = copiedBuffer;
                    oldRear->mSize = size;
                    this->mRear = newRear;
                    ++this->mState.count;
#ifdef USE_OUTPUTMIXEXT
                    if (SL_OBJECTID_AUDIOPLAYER == InterfaceToObjectID(this)) {
                        CAudioPlayer *ap = (CAudioPlayer *) this->mThis;
                        ap->mPlay.mHeadAtEndFired = SL_BOOLEAN_FALSE;
                    }
#endif
                    result = SL_RESULT_SUCCESS;
                }
            } else {
                // BufferQueue source: resample + convert to stereo 16-bit
                uint32_t outRate = (&_opensles_user_freq != NULL) ? _opensles_user_freq : 44100;
                uint32_t inRate = this->samplerate / 1000; // samplerate is in milliHz
                int channels = this->channels;
                int bps = this->bps;

                // Calculate number of source frames
                int bytesPerFrame = channels * (bps / 8);
                SLuint32 srcFrames = size / bytesPerFrame;

                if (srcFrames == 0) {
                    result = SL_RESULT_PARAMETER_INVALID;
                } else {
                    // Calculate output frames using 64-bit to avoid overflow
                    // outFrames = srcFrames * outRate / inRate
                    SLuint32 outFrames;
                    if (inRate == outRate) {
                        outFrames = srcFrames;
                    } else {
                        outFrames = (SLuint32)(((uint64_t)srcFrames * outRate + inRate - 1) / inRate);
                    }

                    // Output is always stereo 16-bit: 4 bytes per frame
                    SLuint32 totalSize = outFrames * 4;
                    void *copiedBuffer = calloc(1, totalSize);
                    if (copiedBuffer == NULL) {
                        result = SL_RESULT_MEMORY_FAILURE;
                    } else {
                        int16_t *dst = (int16_t *)copiedBuffer;

                        if (inRate == outRate && channels == 2 && bps == 16) {
                            // Fast path: stereo 16-bit at native rate, just copy
                            memcpy(copiedBuffer, pBuffer, size < totalSize ? size : totalSize);
                        } else if (inRate == outRate) {
                            // Same rate, just needs format conversion
                            for (SLuint32 i = 0; i < srcFrames; i++) {
                                int16_t l, r;
                                read_source_frame(pBuffer, i, channels, bps, &l, &r);
                                dst[i * 2]     = l;
                                dst[i * 2 + 1] = r;
                            }
                        } else {
                            // Linear interpolation resampler using fixed-point
                            // position accumulator (32.32 format)
                            uint64_t step = ((uint64_t)inRate << 32) / outRate;
                            uint64_t pos = 0;

                            for (SLuint32 i = 0; i < outFrames; i++) {
                                uint32_t idx = (uint32_t)(pos >> 32);
                                uint32_t frac = (uint32_t)(pos & 0xFFFFFFFF);

                                // Clamp index to valid range
                                if (idx >= srcFrames) idx = srcFrames - 1;
                                uint32_t idx1 = (idx + 1 < srcFrames) ? idx + 1 : idx;

                                int16_t l0, r0, l1, r1;
                                read_source_frame(pBuffer, idx, channels, bps, &l0, &r0);
                                read_source_frame(pBuffer, idx1, channels, bps, &l1, &r1);

                                // Linear interpolation: out = s0 + (s1 - s0) * frac
                                // frac is 0..0xFFFFFFFF representing 0.0..1.0
                                int32_t f = frac >> 16; // reduce to 16-bit fraction for mul
                                dst[i * 2]     = (int16_t)(l0 + (((l1 - l0) * f) >> 16));
                                dst[i * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * f) >> 16));

                                pos += step;
                            }
                        }

                        oldRear->mBuffer = copiedBuffer;
                        oldRear->mSize = totalSize;
                        this->mRear = newRear;
                        ++this->mState.count;
#ifdef USE_OUTPUTMIXEXT
                        if (SL_OBJECTID_AUDIOPLAYER == InterfaceToObjectID(this)) {
                            CAudioPlayer *ap = (CAudioPlayer *) this->mThis;
                            ap->mPlay.mHeadAtEndFired = SL_BOOLEAN_FALSE;
                        }
#endif
                        result = SL_RESULT_SUCCESS;
                    }
                }
            }
        }
        // set enqueue attribute if state is PLAYING and the first buffer is enqueued
        interface_unlock_exclusive_attributes(this, ((SL_RESULT_SUCCESS == result) &&
            (1 == this->mState.count) && (SL_PLAYSTATE_PLAYING == getAssociatedState(this))) ?
            ATTR_ENQUEUE : ATTR_NONE);
    }
    SL_LEAVE_INTERFACE
}


SLresult IBufferQueue_Clear(SLBufferQueueItf self)
{
    SL_ENTER_INTERFACE

    result = SL_RESULT_SUCCESS;
    IBufferQueue *this = (IBufferQueue *) self;
    interface_lock_exclusive(this);

#ifdef ANDROID
    if (SL_OBJECTID_AUDIOPLAYER == InterfaceToObjectID(this)) {
        CAudioPlayer *audioPlayer = (CAudioPlayer *) this->mThis;
        // flush associated audio player
        result = android_audioPlayer_bufferQueue_onClear(audioPlayer);
        if (SL_RESULT_SUCCESS == result) {
            this->mFront = &this->mArray[0];
            this->mRear = &this->mArray[0];
            this->mState.count = 0;
            this->mState.playIndex = 0;
            this->mSizeConsumed = 0;
        }
    }
#endif

#ifdef USE_OUTPUTMIXEXT
    // mixer might be reading from the front buffer, so tread carefully here
    // NTH asynchronous cancel instead of blocking until mixer acknowledges
    this->mClearRequested = SL_BOOLEAN_TRUE;
    do {
        interface_cond_wait(this);
    } while (this->mClearRequested);
#endif

    interface_unlock_exclusive(this);

    SL_LEAVE_INTERFACE
}


static SLresult IBufferQueue_GetState(SLBufferQueueItf self, SLBufferQueueState *pState)
{
    SL_ENTER_INTERFACE

    // Note that GetState while a Clear is pending is equivalent to GetState before the Clear

    if (NULL == pState) {
        result = SL_RESULT_PARAMETER_INVALID;
    } else {
        IBufferQueue *this = (IBufferQueue *) self;
        SLBufferQueueState state;
        interface_lock_shared(this);
#ifdef __cplusplus // FIXME Is this a compiler bug?
        state.count = this->mState.count;
        state.playIndex = this->mState.playIndex;
#else
        state = this->mState;
#endif
        interface_unlock_shared(this);
        *pState = state;
        result = SL_RESULT_SUCCESS;
    }

    SL_LEAVE_INTERFACE
}


SLresult IBufferQueue_RegisterCallback(SLBufferQueueItf self,
    slBufferQueueCallback callback, void *pContext)
{
    SL_ENTER_INTERFACE

    IBufferQueue *this = (IBufferQueue *) self;
    interface_lock_exclusive(this);
    // verify pre-condition that media object is in the SL_PLAYSTATE_STOPPED state
    if (SL_PLAYSTATE_STOPPED == getAssociatedState(this)) {
        this->mCallback = callback;
        this->mContext = pContext;
        result = SL_RESULT_SUCCESS;
    } else {
        result = SL_RESULT_PRECONDITIONS_VIOLATED;
    }
    interface_unlock_exclusive(this);

    SL_LEAVE_INTERFACE
}


static const struct SLBufferQueueItf_ IBufferQueue_Itf = {
    IBufferQueue_Enqueue,
    IBufferQueue_Clear,
    IBufferQueue_GetState,
    IBufferQueue_RegisterCallback
};

void IBufferQueue_init(void *self)
{
    //SL_LOGV("IBufferQueue_init(%p) entering", self);
    IBufferQueue *this = (IBufferQueue *) self;
    this->mItf = &IBufferQueue_Itf;
    this->mState.count = 0;
    this->mState.playIndex = 0;
    this->mCallback = NULL;
    this->mContext = NULL;
    this->mNumBuffers = 0;
    this->mClearRequested = SL_BOOLEAN_FALSE;
    this->mArray = NULL;
    this->mFront = NULL;
    this->mRear = NULL;
#ifdef ANDROID
    this->mSizeConsumed = 0;
#endif
    BufferHeader *bufferHeader = this->mTypical;
    unsigned i;
    for (i = 0; i < BUFFER_HEADER_TYPICAL+1; ++i, ++bufferHeader) {
        bufferHeader->mBuffer = NULL;
        bufferHeader->mSize = 0;
    }
}


/** \brief Free the buffer queue, if it was larger than typical.
  * Called by CAudioPlayer_Destroy and CAudioRecorder_Destroy.
  */

void IBufferQueue_Destroy(IBufferQueue *this)
{
    // Free any remaining queued buffers (allocated by Enqueue)
    if (NULL != this->mArray) {
        const BufferHeader *f = this->mFront;
        const BufferHeader *r = this->mRear;
        while (f != NULL && r != NULL && f != r) {
            if (f->mBuffer != NULL) {
                free((void *)f->mBuffer);
                ((BufferHeader *)f)->mBuffer = NULL;
            }
            if (++f == &this->mArray[this->mNumBuffers + 1]) {
                f = this->mArray;
            }
        }
    }
    if ((NULL != this->mArray) && (this->mArray != this->mTypical)) {
        free(this->mArray);
        this->mArray = NULL;
    }
}
