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

/* OutputMixExt implementation */

#include "sles_allinclusive.h"
#include <math.h>


// OutputMixExt is used by SDL, but is not specific to or dependent on SDL


// stereo is a frame consisting of a pair of 16-bit PCM samples

typedef struct {
    short left;
    short right;
} stereo;


/** \brief Cast float to int32 with clamping to int16 range (for gain application before accumulation) */

static inline int32_t gain_to_i32(float f) {
    if (f > 32767.0f) return 32767;
    if (f < -32768.0f) return -32768;
    return (int32_t)f;
}

/** \brief Clamp int32 to int16 range */

static inline short clamp_s16(int32_t v) {
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (short)v;
}


/** \brief Summary of the gain, as an optimization for the mixer */

typedef enum {
    GAIN_MUTE  = 0,  // mValue == 0.0f within epsilon
    GAIN_UNITY = 1,  // mValue == 1.0f within epsilon
    GAIN_OTHER = 2   // 0.0f < mValue < 1.0f
} Summary;


/** \brief Check whether a track has any data for us to read */

static SLboolean track_check(Track *track)
{
    assert(NULL != track);
    SLboolean trackHasData = SL_BOOLEAN_FALSE;

    CAudioPlayer *audioPlayer = track->mAudioPlayer;
    if (NULL != audioPlayer) {

        // track is initialized

        // FIXME This lock could block and result in stuttering;
        // a trylock with retry or lockless solution would be ideal
        object_lock_exclusive(&audioPlayer->mObject);
        assert(audioPlayer->mTrack == track);

        SLuint32 framesMixed = track->mFramesMixed;
        if (0 != framesMixed) {
            track->mFramesMixed = 0;
            audioPlayer->mPlay.mFramesSinceLastSeek += framesMixed;
            audioPlayer->mPlay.mFramesSincePositionUpdate += framesMixed;
        }

        SLboolean doBroadcast = SL_BOOLEAN_FALSE;
        const BufferHeader *oldFront;

        if (audioPlayer->mBufferQueue.mClearRequested) {
            // application thread(s) that call BufferQueue::Clear while mixer is active
            // will block synchronously until mixer acknowledges the Clear request
            // Free all queued buffers before clearing
            {
                const BufferHeader *cf = audioPlayer->mBufferQueue.mFront;
                const BufferHeader *cr = audioPlayer->mBufferQueue.mRear;
                while (cf != cr) {
                    if (cf->mBuffer != NULL) {
                        free((void *)cf->mBuffer);
                        ((BufferHeader *)cf)->mBuffer = NULL;
                    }
                    if (++cf == &audioPlayer->mBufferQueue.mArray[audioPlayer->mBufferQueue.mNumBuffers + 1]) {
                        cf = audioPlayer->mBufferQueue.mArray;
                    }
                }
            }
            audioPlayer->mBufferQueue.mFront = &audioPlayer->mBufferQueue.mArray[0];
            audioPlayer->mBufferQueue.mRear = &audioPlayer->mBufferQueue.mArray[0];
            audioPlayer->mBufferQueue.mState.count = 0;
            audioPlayer->mBufferQueue.mState.playIndex = 0;
            audioPlayer->mBufferQueue.mClearRequested = SL_BOOLEAN_FALSE;
            track->mReader = NULL;
            track->mAvail = 0;
            doBroadcast = SL_BOOLEAN_TRUE;
        }

        if (audioPlayer->mDestroyRequested) {
            // an application thread that calls Object::Destroy while mixer is active will block
            // synchronously in the PreDestroy hook until mixer acknowledges the Destroy request
            COutputMix *outputMix = CAudioPlayer_GetOutputMix(audioPlayer);
            unsigned i = track - outputMix->mOutputMixExt.mTracks;
            assert( /* 0 <= i && */ i < MAX_TRACK);
            unsigned mask = 1 << i;
            track->mAudioPlayer = NULL;
            assert(outputMix->mOutputMixExt.mActiveMask & mask);
            outputMix->mOutputMixExt.mActiveMask &= ~mask;
            audioPlayer->mTrack = NULL;
            audioPlayer->mDestroyRequested = SL_BOOLEAN_FALSE;
            doBroadcast = SL_BOOLEAN_TRUE;
            goto broadcast;
        }

        switch (audioPlayer->mPlay.mState) {

        case SL_PLAYSTATE_PLAYING:  // continue playing current track data
            // Always copy gains so distance attenuation updates (via
            // SetVolumeLevel → audioPlayerGainUpdate) reach the mixer
            // even while a buffer is being consumed.  Without this,
            // short SFX plays at whatever gain was set at dequeue time,
            // ignoring later distance-based volume changes.
            track->mGains[0] = audioPlayer->mGains[0];
            track->mGains[1] = audioPlayer->mGains[1];
            // SFX (non-SndFile) tracks get -26dB reduction.
            // At 0.1f (-20dB), 10 NPC footsteps summed to ~music volume.
            // 0.05f gives more headroom for many simultaneous SFX.
            if (audioPlayer->mSndFile.mSNDFILE == NULL) {
                track->mGains[0] *= 0.05f;
                track->mGains[1] *= 0.05f;
            }

            if (0 < track->mAvail) {
                trackHasData = SL_BOOLEAN_TRUE;
                break;
            }

            // try to get another buffer from queue
            oldFront = audioPlayer->mBufferQueue.mFront;
            if (oldFront != audioPlayer->mBufferQueue.mRear) {
                assert(0 < audioPlayer->mBufferQueue.mState.count);
                track->mReader = oldFront->mBuffer;
                track->mAvail = oldFront->mSize;
                // note that the buffer stays on the queue while we are reading
                audioPlayer->mPlay.mState = SL_PLAYSTATE_PLAYING;
                trackHasData = SL_BOOLEAN_TRUE;
            } else {
                // No buffers on queue — fire HEAD_AT_END callback for SFX
                // (non-SndFile) players so the game's voice lifecycle can
                // release the AudioPlayer slot for reuse.
                if (audioPlayer->mSndFile.mSNDFILE == NULL) {
                    if (audioPlayer->mPlay.mEventFlags & SL_PLAYEVENT_HEADATEND) {
                        slPlayCallback cb = audioPlayer->mPlay.mCallback;
                        void *ctx = audioPlayer->mPlay.mContext;
                        // Clear event flags so callback fires only once
                        audioPlayer->mPlay.mEventFlags = 0;
                        if (NULL != cb) {
                            // Unlock before callback to avoid deadlock with game thread
                            object_unlock_exclusive(&audioPlayer->mObject);
                            (*cb)(&audioPlayer->mPlay.mItf, ctx, SL_PLAYEVENT_HEADATEND);
                            object_lock_exclusive(&audioPlayer->mObject);
                        }
                    }
                }
            }
            break;

        case SL_PLAYSTATE_STOPPING: // application thread(s) called Play::SetPlayState(STOPPED)
            audioPlayer->mPlay.mPosition = (SLmillisecond) 0;
            audioPlayer->mPlay.mFramesSinceLastSeek = 0;
            audioPlayer->mPlay.mFramesSincePositionUpdate = 0;
            audioPlayer->mPlay.mLastSeekPosition = 0;
            audioPlayer->mPlay.mState = SL_PLAYSTATE_STOPPED;
            // stop cancels a pending seek
            audioPlayer->mSeek.mPos = SL_TIME_UNKNOWN;
            oldFront = audioPlayer->mBufferQueue.mFront;
            if (oldFront != audioPlayer->mBufferQueue.mRear) {
                assert(0 < audioPlayer->mBufferQueue.mState.count);
                track->mReader = oldFront->mBuffer;
                track->mAvail = oldFront->mSize;
            }
            doBroadcast = SL_BOOLEAN_TRUE;
            break;

        case SL_PLAYSTATE_STOPPED:  // idle
        case SL_PLAYSTATE_PAUSED:   // idle
            break;

        default:
            assert(SL_BOOLEAN_FALSE);
            break;
        }

broadcast:
        if (doBroadcast) {
            object_cond_broadcast(&audioPlayer->mObject);
        }

        object_unlock_exclusive(&audioPlayer->mObject);

    }

    return trackHasData;

}


/** \brief This is the track mixer: fill the specified 16-bit stereo PCM buffer.
 *
 *  Uses int32 accumulators to avoid per-track saturation distortion.
 *  All tracks are summed in 32-bit, then clamped to int16 once at the end.
 *  This prevents the harsh clipping artifacts that occur when many
 *  simultaneous tracks (e.g. NPC footsteps) are saturate-added pairwise.
 */

void IOutputMixExt_FillBuffer(SLOutputMixExtItf self, void *pBuffer, SLuint32 size)
{
    SL_ENTER_INTERFACE_VOID

    // Force to be a multiple of a frame, assumes stereo 16-bit PCM
    size &= ~3;
    SLuint32 numSamples = size / sizeof(short);

    // Int32 accumulator — all tracks sum here, single final clamp to int16
    int32_t accum[SndFile_BUFSIZE / sizeof(short)];
    memset(accum, 0, numSamples * sizeof(int32_t));

    SLboolean anyTrackContributed = SL_BOOLEAN_FALSE;
    IOutputMixExt *this = (IOutputMixExt *) self;
    IObject *thisObject = this->mThis;
    // This lock should never block, except when the application destroys the output mix object
    object_lock_exclusive(thisObject);
    unsigned activeMask;
    // If the output mix is marked for destruction, then acknowledge the request
    if (this->mDestroyRequested) {
        IEngine *thisEngine = thisObject->mEngine;
        interface_lock_exclusive(thisEngine);
        assert(&thisEngine->mOutputMix->mObject == thisObject);
        thisEngine->mOutputMix = NULL;
        // Note we don't attempt to connect another output mix, even if there is one
        interface_unlock_exclusive(thisEngine);
        // Acknowledge the destroy request, and notify the pre-destroy hook
        this->mDestroyRequested = SL_BOOLEAN_FALSE;
        object_cond_broadcast(thisObject);
        activeMask = 0;
    } else {
        activeMask = this->mActiveMask;
    }
    while (activeMask) {
        unsigned i = ctz(activeMask);
        assert(MAX_TRACK > i);
        activeMask &= ~(1 << i);
        Track *track = &this->mTracks[i];

        // track is allocated

        if (!track_check(track)) {
            continue;
        }

        // track is playing
        unsigned dstOffset = 0;  // byte offset into accumulator
        unsigned desired = size;
        SLboolean trackContributedToMix = SL_BOOLEAN_FALSE;
        float gainL = track->mGains[0];
        float gainR = track->mGains[1];
        SLboolean isMuted = (gainL <= 0.001f && gainR <= 0.001f);
        SLboolean isUnity = (gainL >= 0.999f && gainR >= 0.999f);

        while (desired > 0) {
            unsigned actual = desired;
            if (track->mAvail < actual) {
                actual = track->mAvail;
            }
            // force actual to be a frame multiple
            if (actual > 0) {
                assert(NULL != track->mReader);
                if (!isMuted) {
                    int32_t *acc = accum + (dstOffset / sizeof(short));
                    const stereo *source = (const stereo *) track->mReader;
                    unsigned j;
                    if (isUnity) {
                        // No gain adjustment — add raw samples to accumulator
                        for (j = 0; j < actual; j += sizeof(stereo), acc += 2, ++source) {
                            acc[0] += (int32_t)source->left;
                            acc[1] += (int32_t)source->right;
                        }
                    } else {
                        // Apply per-channel gain, then add to accumulator
                        for (j = 0; j < actual; j += sizeof(stereo), acc += 2, ++source) {
                            acc[0] += gain_to_i32(source->left * gainL);
                            acc[1] += gain_to_i32(source->right * gainR);
                        }
                    }
                    trackContributedToMix = SL_BOOLEAN_TRUE;
                }
                dstOffset += actual;
                desired -= actual;
                track->mReader = (char *) track->mReader + actual;
                track->mAvail -= actual;
                if (track->mAvail == 0) {
                    IBufferQueue *bufferQueue = &track->mAudioPlayer->mBufferQueue;
                    interface_lock_exclusive(bufferQueue);
                    const BufferHeader *oldFront, *newFront, *rear;
                    oldFront = bufferQueue->mFront;
                    rear = bufferQueue->mRear;
                    // a buffer stays on queue while playing, so it better still be there
                    assert(oldFront != rear);
                    newFront = oldFront;
                    if (++newFront == &bufferQueue->mArray[bufferQueue->mNumBuffers + 1]) {
                        newFront = bufferQueue->mArray;
                    }
                    bufferQueue->mFront = (BufferHeader *) newFront;
                    // Free the consumed buffer (allocated by Enqueue)
                    if (oldFront->mBuffer != NULL) {
                        free((void *)oldFront->mBuffer);
                        ((BufferHeader *)oldFront)->mBuffer = NULL;
                    }
                    assert(0 < bufferQueue->mState.count);
                    --bufferQueue->mState.count;
                    if (newFront != rear) {
                        // we don't acknowledge application requests between buffers
                        // within the same mixer frame
                        assert(0 < bufferQueue->mState.count);
                        track->mReader = newFront->mBuffer;
                        track->mAvail = newFront->mSize;
                    }
                    // else we would set play state to playable but not playing during next mixer
                    // frame if the queue is still empty at that time
                    ++bufferQueue->mState.playIndex;
                    slBufferQueueCallback callback = bufferQueue->mCallback;
                    void *context = bufferQueue->mContext;
                    interface_unlock_exclusive(bufferQueue);
                    // The callback function is called on each buffer completion
                    if (NULL != callback) {
                        (*callback)((SLBufferQueueItf) bufferQueue, context);
                        // Maybe it enqueued another buffer, or maybe it didn't.
                        // We will find out later during the next mixer frame.
                    }
                }
                // no lock, but safe because noone else updates this field
                track->mFramesMixed += actual >> 2;    // sizeof(short) * STEREO_CHANNELS
                continue;
            }
            // we need more data: desired > 0 but actual == 0
            if (track_check(track)) {
                continue;
            }
            // underflow: accumulator is already zeroed, nothing to do
            break;
        }
        if (trackContributedToMix) {
            anyTrackContributed = SL_BOOLEAN_TRUE;
        }
    }
    object_unlock_exclusive(thisObject);

    // Final clamp: int32 accumulator → int16 output buffer
    if (anyTrackContributed) {
        short *out = (short *)pBuffer;
        SLuint32 i;
        for (i = 0; i < numSamples; ++i) {
            out[i] = clamp_s16(accum[i]);
        }
    } else {
        // No active tracks, output silence
        memset(pBuffer, 0, size);
    }

    SL_LEAVE_INTERFACE_VOID
}


static const struct SLOutputMixExtItf_ IOutputMixExt_Itf = {
    IOutputMixExt_FillBuffer
};

void IOutputMixExt_init(void *self)
{
    IOutputMixExt *this = (IOutputMixExt *) self;
    this->mItf = &IOutputMixExt_Itf;
    this->mActiveMask = 0;
    Track *track = &this->mTracks[0];
    unsigned i;
    for (i = 0; i < MAX_TRACK; ++i, ++track) {
        track->mAudioPlayer = NULL;
    }
    this->mDestroyRequested = SL_BOOLEAN_FALSE;
}


/** \brief Called by Engine::CreateAudioPlayer to allocate a track */

SLresult IOutputMixExt_checkAudioPlayerSourceSink(CAudioPlayer *this)
{
    this->mTrack = NULL;

    // check the source for compatibility
    switch (this->mDataSource.mLocator.mLocatorType) {
    case SL_DATALOCATOR_BUFFERQUEUE:
    case SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE:
        switch (this->mDataSource.mFormat.mFormatType) {
        case SL_DATAFORMAT_PCM:
#ifdef USE_SDL
            // SDL is hard-coded to 44.1 kHz, and there is no sample rate converter
           // if (SL_SAMPLINGRATE_44_1 != this->mDataSource.mFormat.mPCM.samplesPerSec)
           //       SL_LOGE("WEEE");
           // return SL_RESULT_CONTENT_UNSUPPORTED;
#endif
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }

    // check the sink for compatibility
    const SLDataSink *pAudioSnk = &this->mDataSink.u.mSink;
    Track *track = NULL;
    switch (*(SLuint32 *)pAudioSnk->pLocator) {
    case SL_DATALOCATOR_OUTPUTMIX:
        {
        // pAudioSnk->pFormat is ignored
        IOutputMixExt *omExt = &((COutputMix *) ((SLDataLocator_OutputMix *)
            pAudioSnk->pLocator)->outputMix)->mOutputMixExt;
        // allocate an entry within OutputMix for this track
        interface_lock_exclusive(omExt);
        unsigned availMask = ~omExt->mActiveMask;
        if (!availMask) {
            interface_unlock_exclusive(omExt);
            // All track slots full in output mix
            return SL_RESULT_MEMORY_FAILURE;
        }
        unsigned i = ctz(availMask);
        assert(MAX_TRACK > i);
        omExt->mActiveMask |= 1 << i;
        track = &omExt->mTracks[i];
        track->mAudioPlayer = NULL;    // only field that is accessed before full initialization
        interface_unlock_exclusive(omExt);
        this->mTrack = track;
        this->mGains[0] = 1.0f;
        this->mGains[1] = 1.0f;
        this->mDestroyRequested = SL_BOOLEAN_FALSE;
        }
        break;
    default:
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }

    assert(NULL != track);
    track->mBufferQueue = &this->mBufferQueue;
    track->mAudioPlayer = this;
    track->mReader = NULL;
    track->mAvail = 0;
    track->mGains[0] = 1.0f;
    track->mGains[1] = 1.0f;
    track->mFramesMixed = 0;
    return SL_RESULT_SUCCESS;
}


/** \brief Called when a gain-related field (mute, solo, volume, stereo position, etc.) updated */

void audioPlayerGainUpdate(CAudioPlayer *audioPlayer)
{
    SLboolean mute = audioPlayer->mVolume.mMute;
    SLuint8 muteMask = audioPlayer->mMuteMask;
    SLuint8 soloMask = audioPlayer->mSoloMask;
    SLmillibel level = audioPlayer->mVolume.mLevel;
    SLboolean enableStereoPosition = audioPlayer->mVolume.mEnableStereoPosition;
    SLpermille stereoPosition = audioPlayer->mVolume.mStereoPosition;

    if (soloMask) {
        muteMask |= ~soloMask;
    }
    if (mute || !(~muteMask & 3)) {
        audioPlayer->mGains[0] = 0.0f;
        audioPlayer->mGains[1] = 0.0f;
    } else {
        float playerGain = powf(10.0f, level / 2000.0f);
        unsigned channel;
        for (channel = 0; channel < STEREO_CHANNELS; ++channel) {
            float gain;
            if (muteMask & (1 << channel)) {
                gain = 0.0f;
            } else {
                gain = playerGain;
                if (enableStereoPosition) {
                    switch (channel) {
                    case 0:
                        if (stereoPosition > 0) {
                            gain *= (1000 - stereoPosition) / 1000.0f;
                        }
                        break;
                    case 1:
                        if (stereoPosition < 0) {
                            gain *= (1000 + stereoPosition) / 1000.0f;
                        }
                        break;
                    default:
                        assert(SL_BOOLEAN_FALSE);
                        break;
                    }
                }
            }
            audioPlayer->mGains[channel] = gain;
        }
    }
}
