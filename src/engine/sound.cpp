// sound.cpp: basic positional sound using OpenAL Soft and libsndfile

#include "engine.h"
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/efx.h"
#include "AL/efx-presets.h"
#include "sndfile.h"

extern vec hitsurface;

namespace sound
{
    static const int MaxVolume = 128;
    static const int MusicBuffers = 4;
    static const int MusicBufferFrames = 32768;

    bool nosound = true;

    static ALCdevice *alDevice = NULL;
    static ALCcontext *alContext = NULL;
    static int maxChannels = 0;
    static bool efxFilters = false;
    static bool efxReverb = false;
    static LPALGENFILTERS alGenFilters_ = NULL;
    static LPALDELETEFILTERS alDeleteFilters_ = NULL;
    static LPALFILTERI alFilteri_ = NULL;
    static LPALFILTERF alFilterf_ = NULL;
    static LPALGENEFFECTS alGenEffects_ = NULL;
    static LPALDELETEEFFECTS alDeleteEffects_ = NULL;
    static LPALEFFECTI alEffecti_ = NULL;
    static LPALEFFECTF alEffectf_ = NULL;
    static LPALGENAUXILIARYEFFECTSLOTS alGenAuxiliaryEffectSlots_ = NULL;
    static LPALDELETEAUXILIARYEFFECTSLOTS alDeleteAuxiliaryEffectSlots_ = NULL;
    static LPALAUXILIARYEFFECTSLOTI alAuxiliaryEffectSloti_ = NULL;
    static LPALAUXILIARYEFFECTSLOTF alAuxiliaryEffectSlotf_ = NULL;
    static ALuint efxReverbEffect = 0, efxReverbSlot = 0, efxDistanceReverbEffect = 0, efxDistanceReverbSlot = 0;
    static int efxMaxAuxiliarySends = 0;

    int play(int n, const vec *loc, extentity *ent, int flags, int loops, int fade, int chanid, int radius, int expire);
    void stopAll();

    static sf_count_t sfLength(void *userData)
    {
        stream *file = (stream *)userData;
        stream::offset size = file ? file->size() : stream::offset(-1);
        return size >= 0 ? sf_count_t(size) : 0;
    }

    static sf_count_t sfSeek(sf_count_t offset, int whence, void *userData)
    {
        stream *file = (stream *)userData;
        if(!file || !file->seek(stream::offset(offset), whence)) return -1;
        return sf_count_t(file->tell());
    }

    static sf_count_t sfRead(void *ptr, sf_count_t count, void *userData)
    {
        stream *file = (stream *)userData;
        return file ? sf_count_t(file->read(ptr, size_t(count))) : 0;
    }

    static sf_count_t sfWrite(const void *ptr, sf_count_t count, void *userData)
    {
        return 0;
    }

    static sf_count_t sfTell(void *userData)
    {
        stream *file = (stream *)userData;
        return file ? sf_count_t(file->tell()) : -1;
    }

    static SF_VIRTUAL_IO sfIo = { sfLength, sfSeek, sfRead, sfWrite, sfTell };

    static ALenum alFormatForChannels(int channels)
    {
        switch(channels)
        {
            case 1: return AL_FORMAT_MONO16;
            case 2: return AL_FORMAT_STEREO16;
            default: return 0;
        }
    }

    static short floatToShort(float sample, float scale)
    {
        float scaled = sample*scale;
        int isample = scaled >= 0 ? int(scaled + 0.5f) : int(scaled - 0.5f);
        return short(clamp(isample, -32768, 32767));
    }

    static const char *alErrorName(ALenum error)
    {
        switch(error)
        {
            case AL_NO_ERROR: return "no error";
            case AL_INVALID_NAME: return "invalid name";
            case AL_INVALID_ENUM: return "invalid enum";
            case AL_INVALID_VALUE: return "invalid value";
            case AL_INVALID_OPERATION: return "invalid operation";
            case AL_OUT_OF_MEMORY: return "out of memory";
            default: return "unknown error";
        }
    }

    static bool checkAl(const char *op)
    {
        ALenum error = alGetError();
        if(error == AL_NO_ERROR) return true;
        conoutf(CON_ERROR, "%s failed (OpenAL): %s", op, alErrorName(error));
        return false;
    }

    struct AudioFile
    {
        stream *file;
        SNDFILE *handle;
        SF_INFO info;

        AudioFile() : file(NULL), handle(NULL) { memset(&info, 0, sizeof(info)); }
        ~AudioFile() { close(); }

        bool open(const char *name)
        {
            close();
            file = openfile(name, "rb");
            if(!file) return false;
            memset(&info, 0, sizeof(info));
            handle = sf_open_virtual(&sfIo, SFM_READ, &info, file);
            if(!handle)
            {
                close();
                return false;
            }
            return true;
        }

        void close()
        {
            if(handle) { sf_close(handle); handle = NULL; }
            DELETEP(file);
            memset(&info, 0, sizeof(info));
        }
    };

    struct SoundSample
    {
        char *name;
        ALuint buffer;

        SoundSample() : name(NULL), buffer(0) {}
        ~SoundSample() { DELETEA(name); }

        void cleanup()
        {
            if(buffer)
            {
                alDeleteBuffers(1, &buffer);
                buffer = 0;
            }
        }

        bool load(const char *dir, bool msg = false);
    };

    struct SoundSlot
    {
        SoundSample *sample;
        int volume;
    };

    struct SoundConfig
    {
        int slots, numslots;
        int maxuses;

        bool hasSlot(const SoundSlot *p, const vector<SoundSlot> &v) const
        {
            return p >= v.getbuf() + slots && p < v.getbuf() + slots+numslots && slots+numslots <= v.length();
        }

        int chooseSlot(int flags) const
        {
            if(flags&SND_NO_ALT || numslots <= 1) return slots;
            if(flags&SND_USE_ALT) return slots + 1 + rnd(numslots - 1);
            return slots + rnd(numslots);
        }
    };

    struct SoundChannel
    {
        int id;
        ALuint source, filter, sendFilter, distanceSendFilter;
        bool inuse;
        vec loc;
        SoundSlot *slot;
        extentity *ent;
        int radius, volume, targetVolume, pan, flags, expire, soundentity;
        int fadeStart, fadeEnd, fadeFrom;
        float pitch, gainhf, targetGainHF, gainlf, targetGainLF, reverbSend, targetReverbSend, distanceReverbSend, targetDistanceReverbSend;
        bool dirty, stopping;

        SoundChannel(int id) : id(id), source(0), filter(0), sendFilter(0), distanceSendFilter(0) { reset(); }
        ~SoundChannel() { cleanup(); }

        bool hasLoc() const { return loc.x >= -1e15f; }
        void clearLoc() { loc = vec(-1e16f, -1e16f, -1e16f); }

        void reset()
        {
            inuse = false;
            clearLoc();
            slot = NULL;
            ent = NULL;
            radius = 0;
            volume = targetVolume = -1;
            pan = -1;
            flags = 0;
            expire = -1;
            soundentity = 0;
            fadeStart = fadeEnd = fadeFrom = 0;
            pitch = 1.0f;
            gainhf = -1.0f;
            targetGainHF = 1.0f;
            gainlf = -1.0f;
            targetGainLF = 1.0f;
            reverbSend = -1.0f;
            targetReverbSend = 0.0f;
            distanceReverbSend = -1.0f;
            targetDistanceReverbSend = 0.0f;
            dirty = false;
            stopping = false;
        }

        bool ensureSource()
        {
            if(source) return true;
            alGenSources(1, &source);
            if(!checkAl("alGenSources")) { source = 0; return false; }
            alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
            alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
            if(efxFilters) alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
            return true;
        }

        bool ensureFilter(ALuint &filterId, ALenum type)
        {
            if(!efxFilters) return false;
            if(!filterId)
            {
                alGenFilters_(1, &filterId);
                if(!checkAl("alGenFilters")) { filterId = 0; return false; }
            }
            alFilteri_(filterId, AL_FILTER_TYPE, type);
            switch(type)
            {
                case AL_FILTER_LOWPASS:
                    alFilterf_(filterId, AL_LOWPASS_GAIN, 1.0f);
                    alFilterf_(filterId, AL_LOWPASS_GAINHF, 1.0f);
                    break;
                case AL_FILTER_HIGHPASS:
                    alFilterf_(filterId, AL_HIGHPASS_GAIN, 1.0f);
                    alFilterf_(filterId, AL_HIGHPASS_GAINLF, 1.0f);
                    break;
                case AL_FILTER_BANDPASS:
                    alFilterf_(filterId, AL_BANDPASS_GAIN, 1.0f);
                    alFilterf_(filterId, AL_BANDPASS_GAINLF, 1.0f);
                    alFilterf_(filterId, AL_BANDPASS_GAINHF, 1.0f);
                    break;
            }
            if(!checkAl("OpenAL filter setup"))
            {
                alDeleteFilters_(1, &filterId);
                filterId = 0;
                return false;
            }
            return true;
        }

        void cleanup()
        {
            if(source)
            {
                alSourceStop(source);
                alSourcei(source, AL_BUFFER, 0);
                alDeleteSources(1, &source);
                source = 0;
            }
            if(filter)
            {
                if(alDeleteFilters_) alDeleteFilters_(1, &filter);
                filter = 0;
            }
            if(sendFilter)
            {
                if(alDeleteFilters_) alDeleteFilters_(1, &sendFilter);
                sendFilter = 0;
            }
            if(distanceSendFilter)
            {
                if(alDeleteFilters_) alDeleteFilters_(1, &distanceSendFilter);
                distanceSendFilter = 0;
            }
            reset();
        }
    };

    static vector<SoundChannel> channels;

    static SoundChannel &newChannel(int n, SoundSlot *slot, const vec *loc = NULL, extentity *ent = NULL, int flags = 0, int radius = 0, int soundentity = 0)
    {
        if(ent)
        {
            loc = &ent->o;
            ent->flags |= EF_SOUND;
        }
        while(!channels.inrange(n)) channels.add(channels.length());
        SoundChannel &chan = channels[n];
        ALuint source = chan.source;
        ALuint filter = chan.filter;
        ALuint sendFilter = chan.sendFilter;
        ALuint distanceSendFilter = chan.distanceSendFilter;
        chan.reset();
        chan.source = source;
        chan.filter = filter;
        chan.sendFilter = sendFilter;
        chan.distanceSendFilter = distanceSendFilter;
        chan.inuse = true;
        if(loc) chan.loc = *loc;
        chan.slot = slot;
        chan.ent = ent;
        chan.flags = flags;
        chan.radius = radius;
        chan.soundentity = soundentity;
        return chan;
    }

    static void freeChannel(int n)
    {
        if(!channels.inrange(n) || !channels[n].inuse) return;
        SoundChannel &chan = channels[n];
        if(chan.ent) chan.ent->flags &= ~EF_SOUND;
        chan.reset();
    }

    static void haltChannel(int n)
    {
        if(!channels.inrange(n)) return;
        SoundChannel &chan = channels[n];
        if(chan.source)
        {
            alSourceStop(chan.source);
            alSourcei(chan.source, AL_BUFFER, 0);
        }
        freeChannel(n);
    }

    static int effectiveVolume(const SoundChannel &chan)
    {
        if(chan.fadeEnd <= chan.fadeStart) return chan.targetVolume;
        int now = totalmillis;
        if(now >= chan.fadeEnd) return chan.targetVolume;
        float t = float(now - chan.fadeStart)/float(chan.fadeEnd - chan.fadeStart);
        return clamp(int(chan.fadeFrom + (chan.targetVolume - chan.fadeFrom)*t + 0.5f), 0, MaxVolume);
    }

    static ALuint syncDirectFilter(SoundChannel &chan)
    {
        if(!efxFilters || (chan.targetGainHF >= 0.999f && chan.targetGainLF >= 0.999f))
        {
            alSourcei(chan.source, AL_DIRECT_FILTER, AL_FILTER_NULL);
            chan.gainhf = chan.gainlf = 1.0f;
            return AL_FILTER_NULL;
        }

        ALenum type = chan.targetGainHF < 0.999f && chan.targetGainLF < 0.999f ? AL_FILTER_BANDPASS :
                      chan.targetGainHF < 0.999f ? AL_FILTER_LOWPASS : AL_FILTER_HIGHPASS;
        if(!chan.ensureFilter(chan.filter, type))
        {
            alSourcei(chan.source, AL_DIRECT_FILTER, AL_FILTER_NULL);
            chan.gainhf = chan.gainlf = 1.0f;
            return AL_FILTER_NULL;
        }

        switch(type)
        {
            case AL_FILTER_LOWPASS:
                alFilterf_(chan.filter, AL_LOWPASS_GAINHF, chan.targetGainHF);
                break;
            case AL_FILTER_HIGHPASS:
                alFilterf_(chan.filter, AL_HIGHPASS_GAINLF, chan.targetGainLF);
                break;
            case AL_FILTER_BANDPASS:
                alFilterf_(chan.filter, AL_BANDPASS_GAINLF, chan.targetGainLF);
                alFilterf_(chan.filter, AL_BANDPASS_GAINHF, chan.targetGainHF);
                break;
        }
        alSourcei(chan.source, AL_DIRECT_FILTER, chan.filter);
        chan.gainhf = chan.targetGainHF;
        chan.gainlf = chan.targetGainLF;
        return chan.filter;
    }

    static ALuint syncSendFilter(SoundChannel &chan, ALuint &filterId, float sendGain)
    {
        if(!efxFilters) return AL_FILTER_NULL;
        if(sendGain >= 0.999f && chan.targetGainHF >= 0.999f && chan.targetGainLF >= 0.999f) return AL_FILTER_NULL;
        if(!chan.ensureFilter(filterId, AL_FILTER_BANDPASS)) return AL_FILTER_NULL;
        alFilterf_(filterId, AL_BANDPASS_GAIN, clamp(sendGain, 0.0f, 1.0f));
        alFilterf_(filterId, AL_BANDPASS_GAINLF, chan.targetGainLF);
        alFilterf_(filterId, AL_BANDPASS_GAINHF, chan.targetGainHF);
        return filterId;
    }

    static void syncChannel(SoundChannel &chan)
    {
        if(!chan.source) return;
        int volume = effectiveVolume(chan);
        if(!chan.dirty && volume == chan.volume && fabs(chan.targetGainHF - chan.gainhf) < 1e-3f && fabs(chan.targetGainLF - chan.gainlf) < 1e-3f &&
           fabs(chan.targetReverbSend - chan.reverbSend) < 1e-3f && fabs(chan.targetDistanceReverbSend - chan.distanceReverbSend) < 1e-3f) return;
        chan.volume = volume;
        alSourcef(chan.source, AL_GAIN, clamp(chan.volume/float(MaxVolume), 0.0f, 1.0f));
        float pan = clamp(chan.pan/127.5f - 1.0f, -1.0f, 1.0f);
        alSource3f(chan.source, AL_POSITION, pan, 0.0f, -1.0f);
        if(efxFilters) syncDirectFilter(chan);
        else
        {
            chan.gainhf = chan.targetGainHF;
            chan.gainlf = chan.targetGainLF;
        }
        if(efxReverb && efxReverbSlot && chan.targetReverbSend > 0.001f)
        {
            ALuint send = syncSendFilter(chan, chan.sendFilter, chan.targetReverbSend);
            alSource3i(chan.source, AL_AUXILIARY_SEND_FILTER, efxReverbSlot, 0, send);
            chan.reverbSend = chan.targetReverbSend;
        }
        else if(efxReverb)
        {
            alSource3i(chan.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
            chan.reverbSend = 0.0f;
        }
        if(efxReverb && efxDistanceReverbSlot && efxMaxAuxiliarySends > 1 && chan.targetDistanceReverbSend > 0.001f)
        {
            ALuint send = syncSendFilter(chan, chan.distanceSendFilter, chan.targetDistanceReverbSend);
            alSource3i(chan.source, AL_AUXILIARY_SEND_FILTER, efxDistanceReverbSlot, 1, send);
            chan.distanceReverbSend = chan.targetDistanceReverbSend;
        }
        else if(efxReverb && efxMaxAuxiliarySends > 1)
        {
            alSource3i(chan.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 1, AL_FILTER_NULL);
            chan.distanceReverbSend = 0.0f;
        }
        chan.dirty = false;
        checkAl("OpenAL source update");
    }

    static void stopChannels()
    {
        loopv(channels) if(channels[i].inuse) haltChannel(i);
    }

    static void setMusicVolume(int musicvol);
    extern int musicvol;
    static int curvol = 0;
    VARFP(soundvol, 0, 255, 255,
    {
        if(!soundvol) { stopChannels(); setMusicVolume(0); }
        else if(!curvol) setMusicVolume(musicvol);
        curvol = soundvol;
    });
    VARFP(musicvol, 0, 60, 255, setMusicVolume(soundvol ? musicvol : 0));

    struct MusicPlayer
    {
        char *filename, *donecmd;
        AudioFile audio;
        ALuint source, buffers[MusicBuffers];
        short *pcm;
        int pcmFrames;
        ALenum format;
        bool active, looping, finished;

        MusicPlayer() : filename(NULL), donecmd(NULL), source(0), pcm(NULL), pcmFrames(0), format(0), active(false), looping(false), finished(false)
        {
            memset(buffers, 0, sizeof(buffers));
        }

        ~MusicPlayer() { cleanup(false); }

        void setVolume(int vol)
        {
            if(source) alSourcef(source, AL_GAIN, soundvol ? clamp(vol/255.0f, 0.0f, 1.0f) : 0.0f);
        }

        bool streamBuffer(ALuint buffer)
        {
            if(!audio.handle) return false;

            sf_count_t frames = sf_readf_short(audio.handle, pcm, pcmFrames);
            if(frames <= 0 && looping)
            {
                sf_seek(audio.handle, 0, SEEK_SET);
                frames = sf_readf_short(audio.handle, pcm, pcmFrames);
            }
            if(frames <= 0) return false;

            alBufferData(buffer, format, pcm, ALsizei(frames*audio.info.channels*sizeof(short)), ALsizei(audio.info.samplerate));
            return checkAl("alBufferData");
        }

        bool createSource()
        {
            alGenSources(1, &source);
            if(!checkAl("alGenSources")) { source = 0; return false; }
            alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
            alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
            alSource3f(source, AL_POSITION, 0.0f, 0.0f, -1.0f);
            alGenBuffers(MusicBuffers, buffers);
            if(!checkAl("alGenBuffers")) return false;
            return true;
        }

        bool startFile(const char *file, const char *cmd)
        {
            cleanup(true);
            if(!audio.open(file)) return false;
            format = alFormatForChannels(audio.info.channels);
            if(!format)
            {
                conoutf(CON_ERROR, "unsupported music channel count: %d", audio.info.channels);
                cleanup(false);
                return false;
            }

            pcmFrames = MusicBufferFrames;
            pcm = new (false) short[pcmFrames*audio.info.channels];
            if(!pcm)
            {
                conoutf(CON_ERROR, "could not allocate music stream buffer");
                cleanup(false);
                return false;
            }
            if(!createSource())
            {
                cleanup(false);
                return false;
            }

            looping = !cmd[0];
            filename = newstring(file);
            if(cmd[0]) donecmd = newstring(cmd);

            int queued = 0;
            loopi(MusicBuffers)
            {
                if(!streamBuffer(buffers[i])) break;
                alSourceQueueBuffers(source, 1, &buffers[i]);
                queued++;
            }
            if(!queued)
            {
                cleanup(false);
                return false;
            }

            setVolume(musicvol);
            alSourcePlay(source);
            if(!checkAl("alSourcePlay"))
            {
                cleanup(false);
                return false;
            }
            active = true;
            finished = false;
            return true;
        }

        bool startPackage(const char *name, const char *cmd)
        {
            if(!soundvol || !musicvol || !name[0]) return false;
            defformatstring(file, "packages/%s", name);
            path(file);
            return startFile(file, cmd);
        }

        void finish()
        {
            char *cmd = donecmd;
            donecmd = NULL;
            cleanup(true);
            if(cmd)
            {
                execute(cmd);
                delete[] cmd;
            }
        }

        void update()
        {
            if(!active || !source) return;

            ALint processed = 0;
            alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
            while(processed-- > 0)
            {
                ALuint buffer = 0;
                alSourceUnqueueBuffers(source, 1, &buffer);
                if(streamBuffer(buffer)) alSourceQueueBuffers(source, 1, &buffer);
                else finished = true;
            }

            ALint queued = 0;
            alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
            if(finished && queued <= 0)
            {
                finish();
                return;
            }

            ALint state = AL_STOPPED;
            alGetSourcei(source, AL_SOURCE_STATE, &state);
            if(state != AL_PLAYING && queued > 0) alSourcePlay(source);
        }

        void cleanup(bool clearName = true)
        {
            if(source)
            {
                alSourceStop(source);
                ALint queued = 0;
                alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
                while(queued-- > 0)
                {
                    ALuint buffer = 0;
                    alSourceUnqueueBuffers(source, 1, &buffer);
                }
                alDeleteSources(1, &source);
                source = 0;
            }
            if(buffers[0]) alDeleteBuffers(MusicBuffers, buffers);
            memset(buffers, 0, sizeof(buffers));
            audio.close();
            DELETEA(pcm);
            pcmFrames = 0;
            format = 0;
            active = false;
            looping = false;
            finished = false;
            if(clearName)
            {
                DELETEA(filename);
                DELETEA(donecmd);
            }
        }
    };

    static MusicPlayer music;

    static void setMusicVolume(int vol)
    {
        if(nosound) return;
        music.setVolume(vol);
    }

    void stopMusic()
    {
        if(nosound) return;
        music.cleanup(true);
    }

    #ifdef WIN32
    #define AUDIODEVICE ""
    #else
    #define AUDIODEVICE ""
    #endif

    static bool shouldInitAudio = true;
    SVARF(audiodriver, AUDIODEVICE, { shouldInitAudio = true; initwarning("sound configuration", INIT_RESET, CHANGE_SOUND); });
    VARF(usesound, 0, 1, 1, { shouldInitAudio = true; initwarning("sound configuration", INIT_RESET, CHANGE_SOUND); });
    VARF(soundchans, 1, 32, 128, initwarning("sound configuration", INIT_RESET, CHANGE_SOUND));
    VARF(soundfreq, 0, 44100, 48000, initwarning("sound configuration", INIT_RESET, CHANGE_SOUND));
    VARF(soundbufferlen, 128, 1024, 4096, initwarning("sound configuration", INIT_RESET, CHANGE_SOUND));

    static void destroyEfxReverb()
    {
        if(efxDistanceReverbSlot && alDeleteAuxiliaryEffectSlots_)
        {
            alDeleteAuxiliaryEffectSlots_(1, &efxDistanceReverbSlot);
            efxDistanceReverbSlot = 0;
        }
        if(efxDistanceReverbEffect && alDeleteEffects_)
        {
            alDeleteEffects_(1, &efxDistanceReverbEffect);
            efxDistanceReverbEffect = 0;
        }
        if(efxReverbSlot && alDeleteAuxiliaryEffectSlots_)
        {
            alDeleteAuxiliaryEffectSlots_(1, &efxReverbSlot);
            efxReverbSlot = 0;
        }
        if(efxReverbEffect && alDeleteEffects_)
        {
            alDeleteEffects_(1, &efxReverbEffect);
            efxReverbEffect = 0;
        }
        efxReverb = false;
    }

    static void clearEfx()
    {
        efxFilters = false;
        efxReverb = false;
        alGenFilters_ = NULL;
        alDeleteFilters_ = NULL;
        alFilteri_ = NULL;
        alFilterf_ = NULL;
        alGenEffects_ = NULL;
        alDeleteEffects_ = NULL;
        alEffecti_ = NULL;
        alEffectf_ = NULL;
        alGenAuxiliaryEffectSlots_ = NULL;
        alDeleteAuxiliaryEffectSlots_ = NULL;
        alAuxiliaryEffectSloti_ = NULL;
        alAuxiliaryEffectSlotf_ = NULL;
        efxReverbEffect = efxReverbSlot = efxDistanceReverbEffect = efxDistanceReverbSlot = 0;
        efxMaxAuxiliarySends = 0;
    }

    static void setReverbEffect(ALuint effect, const EFXEAXREVERBPROPERTIES &shape, float gainScale)
    {
        alEffecti_(effect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
        alEffectf_(effect, AL_REVERB_DENSITY, clamp(shape.flDensity, 0.0f, 1.0f));
        alEffectf_(effect, AL_REVERB_DIFFUSION, clamp(shape.flDiffusion, 0.0f, 1.0f));
        alEffectf_(effect, AL_REVERB_GAIN, clamp(shape.flGain*gainScale, 0.0f, 1.0f));
        alEffectf_(effect, AL_REVERB_GAINHF, clamp(shape.flGainHF, 0.0f, 1.0f));
        alEffectf_(effect, AL_REVERB_DECAY_TIME, clamp(shape.flDecayTime, 0.1f, 20.0f));
        alEffectf_(effect, AL_REVERB_DECAY_HFRATIO, clamp(shape.flDecayHFRatio, 0.1f, 2.0f));
        alEffectf_(effect, AL_REVERB_REFLECTIONS_GAIN, clamp(shape.flReflectionsGain*sqrtf(max(gainScale, 0.0f)), 0.0f, 3.16f));
        alEffectf_(effect, AL_REVERB_REFLECTIONS_DELAY, clamp(shape.flReflectionsDelay, 0.0f, 0.3f));
        alEffectf_(effect, AL_REVERB_LATE_REVERB_GAIN, clamp(shape.flLateReverbGain*max(gainScale, 0.0f), 0.0f, 10.0f));
        alEffectf_(effect, AL_REVERB_LATE_REVERB_DELAY, clamp(shape.flLateReverbDelay, 0.0f, 0.1f));
        alEffectf_(effect, AL_REVERB_AIR_ABSORPTION_GAINHF, clamp(shape.flAirAbsorptionGainHF, 0.892f, 1.0f));
        alEffectf_(effect, AL_REVERB_ROOM_ROLLOFF_FACTOR, clamp(shape.flRoomRolloffFactor, 0.0f, 10.0f));
        alEffecti_(effect, AL_REVERB_DECAY_HFLIMIT, shape.iDecayHFLimit ? AL_TRUE : AL_FALSE);
    }

    static bool initEfxReverb()
    {
        if(!alGenEffects_ || !alDeleteEffects_ || !alEffecti_ || !alEffectf_ ||
           !alGenAuxiliaryEffectSlots_ || !alDeleteAuxiliaryEffectSlots_ || !alAuxiliaryEffectSloti_ || !alAuxiliaryEffectSlotf_)
            return false;

        alGenEffects_(1, &efxReverbEffect);
        if(!checkAl("alGenEffects")) { efxReverbEffect = 0; return false; }
        EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
        setReverbEffect(efxReverbEffect, generic, 0.0f);
        if(!checkAl("OpenAL reverb effect setup")) { destroyEfxReverb(); return false; }

        alGenAuxiliaryEffectSlots_(1, &efxReverbSlot);
        if(!checkAl("alGenAuxiliaryEffectSlots")) { efxReverbSlot = 0; destroyEfxReverb(); return false; }
        alAuxiliaryEffectSloti_(efxReverbSlot, AL_EFFECTSLOT_EFFECT, efxReverbEffect);
        alAuxiliaryEffectSlotf_(efxReverbSlot, AL_EFFECTSLOT_GAIN, 1.0f);
        if(!checkAl("OpenAL reverb slot setup")) { destroyEfxReverb(); return false; }

        if(efxMaxAuxiliarySends > 1)
        {
            alGenEffects_(1, &efxDistanceReverbEffect);
            if(!checkAl("alGenEffects"))
            {
                efxDistanceReverbEffect = 0;
                return true;
            }
            EFXEAXREVERBPROPERTIES rolling = EFX_REVERB_PRESET_OUTDOORS_ROLLINGPLAINS;
            setReverbEffect(efxDistanceReverbEffect, rolling, 6.0f);
            if(!checkAl("OpenAL distance reverb effect setup"))
            {
                if(efxDistanceReverbEffect) { alDeleteEffects_(1, &efxDistanceReverbEffect); efxDistanceReverbEffect = 0; }
                return true;
            }
            alGenAuxiliaryEffectSlots_(1, &efxDistanceReverbSlot);
            if(!checkAl("alGenAuxiliaryEffectSlots"))
            {
                efxDistanceReverbSlot = 0;
                if(efxDistanceReverbEffect) { alDeleteEffects_(1, &efxDistanceReverbEffect); efxDistanceReverbEffect = 0; }
                return true;
            }
            alAuxiliaryEffectSloti_(efxDistanceReverbSlot, AL_EFFECTSLOT_EFFECT, efxDistanceReverbEffect);
            alAuxiliaryEffectSlotf_(efxDistanceReverbSlot, AL_EFFECTSLOT_GAIN, 1.0f);
            if(!checkAl("OpenAL distance reverb slot setup"))
            {
                if(efxDistanceReverbSlot) { alDeleteAuxiliaryEffectSlots_(1, &efxDistanceReverbSlot); efxDistanceReverbSlot = 0; }
                if(efxDistanceReverbEffect) { alDeleteEffects_(1, &efxDistanceReverbEffect); efxDistanceReverbEffect = 0; }
            }
        }
        return true;
    }

    static void initEfx()
    {
        clearEfx();
        if(alcIsExtensionPresent(alDevice, ALC_EXT_EFX_NAME) != ALC_TRUE) return;
        alGenFilters_ = (LPALGENFILTERS)alGetProcAddress("alGenFilters");
        alDeleteFilters_ = (LPALDELETEFILTERS)alGetProcAddress("alDeleteFilters");
        alFilteri_ = (LPALFILTERI)alGetProcAddress("alFilteri");
        alFilterf_ = (LPALFILTERF)alGetProcAddress("alFilterf");
        alGenEffects_ = (LPALGENEFFECTS)alGetProcAddress("alGenEffects");
        alDeleteEffects_ = (LPALDELETEEFFECTS)alGetProcAddress("alDeleteEffects");
        alEffecti_ = (LPALEFFECTI)alGetProcAddress("alEffecti");
        alEffectf_ = (LPALEFFECTF)alGetProcAddress("alEffectf");
        alGenAuxiliaryEffectSlots_ = (LPALGENAUXILIARYEFFECTSLOTS)alGetProcAddress("alGenAuxiliaryEffectSlots");
        alDeleteAuxiliaryEffectSlots_ = (LPALDELETEAUXILIARYEFFECTSLOTS)alGetProcAddress("alDeleteAuxiliaryEffectSlots");
        alAuxiliaryEffectSloti_ = (LPALAUXILIARYEFFECTSLOTI)alGetProcAddress("alAuxiliaryEffectSloti");
        alAuxiliaryEffectSlotf_ = (LPALAUXILIARYEFFECTSLOTF)alGetProcAddress("alAuxiliaryEffectSlotf");
        efxFilters = alGenFilters_ && alDeleteFilters_ && alFilteri_ && alFilterf_;
        ALCint sends = 0;
        alcGetIntegerv(alDevice, ALC_MAX_AUXILIARY_SENDS, 1, &sends);
        efxMaxAuxiliarySends = max(int(sends), 0);
        efxReverb = initEfxReverb();
    }

    static bool initDevice()
    {
        const ALCchar *deviceName = audiodriver[0] ? audiodriver : NULL;
        alDevice = alcOpenDevice(deviceName);
        if(!alDevice && deviceName)
        {
            conoutf(CON_WARN, "could not open OpenAL device '%s', trying default device", audiodriver);
            alDevice = alcOpenDevice(NULL);
        }
        if(!alDevice)
        {
            conoutf(CON_ERROR, "sound init failed (OpenAL): could not open device");
            return false;
        }
        ALCint attrs[] = { ALC_FREQUENCY, soundfreq, 0 };
        alContext = alcCreateContext(alDevice, soundfreq > 0 ? attrs : NULL);
        if(!alContext || !alcMakeContextCurrent(alContext))
        {
            conoutf(CON_ERROR, "sound init failed (OpenAL): could not create context");
            if(alContext) { alcDestroyContext(alContext); alContext = NULL; }
            alcCloseDevice(alDevice);
            alDevice = NULL;
            clearEfx();
            return false;
        }
        initEfx();

        alDistanceModel(AL_NONE);
        alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
        alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
        const ALfloat orientation[6] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
        alListenerfv(AL_ORIENTATION, orientation);
        return checkAl("OpenAL listener setup");
    }

    void init()
    {
        if(shouldInitAudio)
        {
            shouldInitAudio = false;
            if(alContext || alDevice)
            {
                destroyEfxReverb();
                alcMakeContextCurrent(NULL);
                if(alContext) { alcDestroyContext(alContext); alContext = NULL; }
                if(alDevice) { alcCloseDevice(alDevice); alDevice = NULL; }
                clearEfx();
            }
            if(!usesound || !initDevice())
            {
                nosound = true;
                return;
            }
        }
        maxChannels = soundchans;
        nosound = false;
    }

    static bool loadSoundFile(const char *name, ALuint &buffer)
    {
        AudioFile audio;
        if(!audio.open(name)) return false;

        if(audio.info.channels <= 0)
        {
            conoutf(CON_ERROR, "unsupported sample channel count: %d (%s)", audio.info.channels, name);
            return false;
        }
        if(audio.info.frames <= 0 || audio.info.frames > sf_count_t(INT_MAX/(audio.info.channels*sizeof(short))) || audio.info.frames > sf_count_t(INT_MAX/sizeof(short)))
        {
            conoutf(CON_ERROR, "invalid sample length: %s", name);
            return false;
        }

        int inputSamples = int(audio.info.frames*audio.info.channels);
        float *pcm = new (false) float[inputSamples];
        if(!pcm)
        {
            conoutf(CON_ERROR, "could not allocate sample: %s", name);
            return false;
        }

        sf_count_t frames = sf_readf_float(audio.handle, pcm, audio.info.frames);
        if(frames != audio.info.frames)
        {
            conoutf(CON_ERROR, "incomplete read while loading sample: %s", name);
            delete[] pcm;
            return false;
        }

        int outputSamples = int(audio.info.frames);
        float *mono = pcm;
        if(audio.info.channels > 1)
        {
            mono = new (false) float[outputSamples];
            if(!mono)
            {
                conoutf(CON_ERROR, "could not allocate mono sample: %s", name);
                delete[] pcm;
                return false;
            }
            loopi(outputSamples)
            {
                const float *frame = &pcm[i*audio.info.channels];
                float sample = 0;
                loopj(audio.info.channels) sample += frame[j];
                mono[i] = sample/audio.info.channels;
            }
        }

        float peak = 0;
        loopi(outputSamples) peak = max(peak, float(fabs(mono[i])));
        float scale = peak > 1.0f ? 32767.0f/peak : 32767.0f;

        short *out = new (false) short[outputSamples];
        if(!out)
        {
            conoutf(CON_ERROR, "could not allocate output sample: %s", name);
            if(mono != pcm) delete[] mono;
            delete[] pcm;
            return false;
        }
        loopi(outputSamples) out[i] = floatToShort(mono[i], scale);

        ALuint newBuffer = 0;
        alGenBuffers(1, &newBuffer);
        if(checkAl("alGenBuffers"))
        {
            alBufferData(newBuffer, AL_FORMAT_MONO16, out, ALsizei(outputSamples*sizeof(short)), ALsizei(audio.info.samplerate));
            if(!checkAl("alBufferData"))
            {
                alDeleteBuffers(1, &newBuffer);
                newBuffer = 0;
            }
        }
        delete[] out;
        if(mono != pcm) delete[] mono;
        delete[] pcm;
        if(!newBuffer) return false;
        buffer = newBuffer;
        return true;
    }

    bool SoundSample::load(const char *dir, bool msg)
    {
        if(buffer) return true;
        if(!name[0]) return false;

        static const char * const exts[] = { "", ".wav", ".ogg" };
        string filename;
        loopi(sizeof(exts)/sizeof(exts[0]))
        {
            formatstring(filename, "packages/sound/%s%s%s", dir, name, exts[i]);
            if(msg && !i) renderprogress(0, filename);
            path(filename);
            if(loadSoundFile(filename, buffer)) return true;
        }

        conoutf(CON_ERROR, "failed to load sample: packages/sound/%s%s", dir, name);
        return false;
    }

    static struct SoundType
    {
        hashnameset<SoundSample> samples;
        vector<SoundSlot> slots;
        vector<SoundConfig> configs;
        const char *dir;

        SoundType(const char *dir) : dir(dir) {}

        int findSound(const char *name, int vol)
        {
            loopv(configs)
            {
                SoundConfig &s = configs[i];
                loopj(s.numslots)
                {
                    SoundSlot &c = slots[s.slots+j];
                    if(!strcmp(c.sample->name, name) && (!vol || c.volume==vol)) return i;
                }
            }
            return -1;
        }

        int addSlot(const char *name, int vol)
        {
            SoundSample *s = samples.access(name);
            if(!s)
            {
                char *n = newstring(name);
                s = &samples[n];
                s->name = n;
                s->buffer = 0;
            }
            SoundSlot *oldslots = slots.getbuf();
            int oldlen = slots.length();
            SoundSlot &slot = slots.add();
            if(slots.getbuf() != oldslots) loopv(channels)
            {
                SoundChannel &chan = channels[i];
                if(chan.inuse && chan.slot >= oldslots && chan.slot < &oldslots[oldlen])
                    chan.slot = &slots[chan.slot - oldslots];
            }
            slot.sample = s;
            slot.volume = vol ? vol : 100;
            return oldlen;
        }

        int addSound(const char *name, int vol, int maxuses = 0)
        {
            SoundConfig &s = configs.add();
            s.slots = addSlot(name, vol);
            s.numslots = 1;
            s.maxuses = maxuses;
            return configs.length()-1;
        }

        void addAlt(const char *name, int vol)
        {
            if(configs.empty()) return;
            addSlot(name, vol);
            configs.last().numslots++;
        }

        void clear()
        {
            slots.setsize(0);
            configs.setsize(0);
        }

        void reset()
        {
            loopv(channels)
            {
                SoundChannel &chan = channels[i];
                if(chan.inuse && slots.inbuf(chan.slot)) haltChannel(i);
            }
            clear();
        }

        void cleanupSamples()
        {
            enumerate(samples, SoundSample, s, s.cleanup());
        }

        void cleanup()
        {
            cleanupSamples();
            slots.setsize(0);
            configs.setsize(0);
            samples.clear();
        }

        void preload(int n)
        {
            if(nosound || !configs.inrange(n)) return;
            SoundConfig &config = configs[n];
            loopk(config.numslots) slots[config.slots+k].sample->load(dir, true);
        }

        bool playing(const SoundChannel &chan, const SoundConfig &config) const
        {
            return chan.inuse && config.hasSlot(chan.slot, slots);
        }
    } gameSounds("game/"), mapSounds("mapsound/");

    int registerSound(const char *name, int vol) { return gameSounds.addSound(name, vol, 0); }
    int registerMapSound(const char *name, int vol, int maxuses) { return mapSounds.addSound(name, vol, maxuses < 0 ? 0 : max(1, maxuses)); }
    void addAltSound(const char *name, int vol) { gameSounds.addAlt(name, vol); }
    void addAltMapSound(const char *name, int vol) { mapSounds.addAlt(name, vol); }
    int numSounds() { return gameSounds.configs.length(); }
    int numMapSounds() { return mapSounds.configs.length(); }
    void soundReset() { gameSounds.reset(); }
    void mapSoundReset() { mapSounds.reset(); }

    void resetChannels()
    {
        loopv(channels) channels[i].cleanup();
        channels.shrink(0);
    }

    void cleanup()
    {
        closemumble();
        music.cleanup(true);
        gameSounds.cleanup();
        mapSounds.cleanup();
        resetChannels();
        if(alContext || alDevice)
        {
            destroyEfxReverb();
            alcMakeContextCurrent(NULL);
            if(alContext) { alcDestroyContext(alContext); alContext = NULL; }
            if(alDevice) { alcCloseDevice(alDevice); alDevice = NULL; }
        }
        clearEfx();
        nosound = true;
    }

    void stopMapSounds()
    {
        loopv(channels) if(channels[i].inuse && channels[i].ent) haltChannel(i);
    }

    void clearMapSounds()
    {
        stopMapSounds();
        mapSounds.clear();
    }

    void stopMapSound(extentity *e)
    {
        loopv(channels)
        {
            SoundChannel &chan = channels[i];
            if(chan.inuse && chan.ent == e) haltChannel(i);
        }
    }

    static float metersToUnits(float meters);
    extern int soundairattenuation;
    extern float maxsounddistance;

    void checkMapSounds()
    {
        const vector<extentity *> &ents = entities::getents();

        loopv(ents)
        {
            extentity &e = *ents[i];
            if(e.type != ET_SOUND) continue;

            float dist = camera1->o.dist(e.o);

            // ET_SOUND attr2 is the mapper-authored radius, it must always be respected for map sounds
            float mapradius = e.attr2 > 0 ? float(e.attr2) : 0.0f;

            // global max sound distance is only a fallback/extra cap, never a replacement for map sound radius
            float globalradius = soundairattenuation && maxsounddistance > 0
                ? metersToUnits(maxsounddistance)
                : 0.0f;

            float maxdist = 0.0f;

            if(mapradius > 0.0f && globalradius > 0.0f) maxdist = min(mapradius, globalradius);
            else if(mapradius > 0.0f) maxdist = mapradius;
            else if(globalradius > 0.0f) maxdist = globalradius;

            bool inrange = maxdist <= 0.0f || dist < maxdist;

            if(inrange)
            {
                if(!(e.flags&EF_SOUND)) play(e.attr1, NULL, &e, SND_MAP, -1);
            }
            else if(e.flags&EF_SOUND) stopMapSound(&e);
        }
    }

    VAR(stereo, 0, 1, 1);
    VAR(maxsoundradius, 1, 340, 0);
    VARP(soundfollowentities, 0, 1, 1);
    VARP(soundpitchrandom, 0, 0, 1);
    FVAR(soundpitchrandomamount, 0.0f, 0.03f, 1.0f);
    VARP(soundairattenuation, 0, 0, 1);
    FVARP(maxsounddistance, 0.0f, 1024.0f, 4096.0f);
    FVARP(sounddistanceattenuationfactor, 0.0f, 2.0f, 4.0f);
    FVARP(soundattenuationneardistance, 0.0f, 8.0f, 100.0f);
    FVARP(soundattenuationhalfdistance, 1.0f, 80.0f, 1000.0f);
    FVARP(soundmufflingfactor, 0.0f, 1.0f, 4.0f);
    FVAR(sounddistancereverb, 0.0f, 0.35f, 2.0f);
    FVAR(sounddistancebasscut, 0.0f, 0.2f, 1.0f);
    VARP(sounddistanceattack, 0, 250, 2000);
    FVARR(airhumidity, 0.0f, 50.0f, 100.0f); // percent
    FVARR(airtemperature, -50.0f, 20.0f, 60.0f); // celsius
    FVARR(airpressure, 0.5f, 1.0f, 2.0f); // atmo

    VARP(soundacoustics, 0, 0, 1);
    VAR(soundacousticsmooth, 0, 150, 2000);
    VAR(debugsoundacoustics, 0, 0, 1);
    FVAR(soundacousticrange, 4.0f, 512.0f, 1024.0f);
    FVAR(soundacousticocclusion, 0.0f, 1.0f, 2.0f);
    FVAR(soundacousticblockgain, 0.05f, 0.7f, 1.0f);
    FVAR(soundacousticmufflegainhf, 0.02f, 0.1f, 1.0f);
    FVARP(soundacousticreverb, 0.0f, 1.2f, 2.0f);
    VARP(soundacousticgrid, 0, 1, 1);
    VARP(soundacousticcellsize, 16, 32, 128);
    VARP(soundacousticbakerays, 16, 64, 256);
    VARP(soundacousticgridradius, 32, 256, 256);

    static const float SoundUnitsPerMeter = 5.0f;
    static const float SoundLoudnessFrequency = 1000.0f;
    static const float SoundMuffleFrequency = 16000.0f;

    static float unitsToMeters(float units)
    {
        return max(units, 0.0f)/SoundUnitsPerMeter;
    }

    static float metersToUnits(float meters)
    {
        return max(meters, 0.0f)*SoundUnitsPerMeter;
    }

    static float randomPitchOffset(const vec *loc, int flags, const extentity *ent)
    {
        if(!soundpitchrandom || soundpitchrandomamount <= 0.0f || ent || (flags&SND_MAP) || (!loc && !(flags&SND_HUD))) return 1.0f;
        float amount = min(soundpitchrandomamount, 0.99f);
        return clamp(1.0f + rndscale(2.0f*amount) - amount, 0.01f, 4.0f);
    }

    static int soundEntityId(const vec *loc, const extentity *ent, int flags)
    {
        return soundfollowentities && loc && !ent && !(flags&SND_MAP) ? game::getsoundentityid(loc) : 0;
    }

    static void updateSoundEntity(SoundChannel &chan)
    {
        if(!soundfollowentities || chan.soundentity <= 0) return;
        vec pos;
        if(game::getsoundentitypos(chan.soundentity, pos)) chan.loc = pos;
        else chan.soundentity = 0;
    }

    enum
    {
        AP_SMALLROOM = 0,
        AP_HALL,
        AP_CORRIDOR,
        AP_CAVE,
        AP_OPENOUTDOOR,
        AP_COURTYARD,
        AP_STREET,
        AP_CANYON,
        AP_NUM
    };

    struct AcousticPreset
    {
        const char *name;
        bool outdoor;
        EFXEAXREVERBPROPERTIES efx;
    };

    static const AcousticPreset acousticPresets[AP_NUM] =
    {
        { "small_room", false, EFX_REVERB_PRESET_CASTLE_SMALLROOM },
        { "hall", false, EFX_REVERB_PRESET_CASTLE_HALL },
        { "corridor", false, EFX_REVERB_PRESET_STONECORRIDOR },
        { "cave", false, EFX_REVERB_PRESET_CAVE },
        { "open_outdoor", true, EFX_REVERB_PRESET_OUTDOORS_ROLLINGPLAINS },
        { "courtyard", true, EFX_REVERB_PRESET_CASTLE_COURTYARD },
        { "street", true, EFX_REVERB_PRESET_CITY_STREETS },
        { "canyon", true, EFX_REVERB_PRESET_OUTDOORS_DEEPCANYON }
    };

    struct AcousticChoice
    {
        int first, second;
        float firstWeight, secondWeight;

        AcousticChoice() : first(0), second(0), firstWeight(1), secondWeight(0) {}
    };

    struct AcousticCell
    {
        ivec coord;
        vec origin;
        float airOccupancy, skyOpenness, openness, hitRatio, nearWallRatio, farWallRatio, nearDistance, medianDistance, farDistance, distanceVariance,
              corridorScore, verticalOpenness, clutterScore, irregularityScore, outdoorConnectivity, presetScores[AP_NUM], presetBlend, confidence,
              reverbGain, reverbDecay, reflection, muffleOpen, outdoorRatio;
        int primaryPreset, secondaryPreset, region, connections;
        bool valid, boundary;
        EFXEAXREVERBPROPERTIES reverbShape;
        AcousticChoice indoorChoice, outdoorChoice;

        AcousticCell() : coord(0, 0, 0), origin(0, 0, 0), airOccupancy(0), skyOpenness(0), openness(0), hitRatio(0), nearWallRatio(0), farWallRatio(0),
            nearDistance(0), medianDistance(0), farDistance(0), distanceVariance(0), corridorScore(0), verticalOpenness(0), clutterScore(0),
            irregularityScore(0), outdoorConnectivity(0), presetBlend(0), confidence(0), reverbGain(0), reverbDecay(0.3f), reflection(0),
            muffleOpen(1), outdoorRatio(1), primaryPreset(AP_OPENOUTDOOR), secondaryPreset(AP_OPENOUTDOOR), region(-1), connections(0),
            valid(false), boundary(false)
        {
            loopi(AP_NUM) presetScores[i] = 0.0f;
            EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
            reverbShape = generic;
            indoorChoice.first = indoorChoice.second = AP_HALL;
            outdoorChoice.first = outdoorChoice.second = AP_OPENOUTDOOR;
        }
    };

    struct AcousticRegion
    {
        int id, firstCell, cellCount, primaryPreset, secondaryPreset;
        vec center;
        float confidence, outdoorRatio, presetBlend;

        AcousticRegion() : id(-1), firstCell(-1), cellCount(0), primaryPreset(AP_OPENOUTDOOR), secondaryPreset(AP_OPENOUTDOOR), center(0, 0, 0),
            confidence(0), outdoorRatio(1), presetBlend(0) {}
    };

    struct AcousticPortal
    {
        int regionA, regionB;
        vec center, normal;
        float apertureSize, openingStrength, acousticCost, highFrequencyLoss, diffractionCost;

        AcousticPortal() : regionA(-1), regionB(-1), center(0, 0, 0), normal(0, 0, 1), apertureSize(0), openingStrength(0), acousticCost(0),
            highFrequencyLoss(0), diffractionCost(0) {}
    };

    struct AcousticPath
    {
        vector<int> regions, portals;
        float acousticDistance, pathCost, diffractionAmount, occlusionAmount;
        int bestPortal;

        AcousticPath() : acousticDistance(0), pathCost(0), diffractionAmount(0), occlusionAmount(0), bestPortal(-1) {}
    };

    static float rampfactor(float x, float low, float high)
    {
        if(high <= low) return x >= high ? 1.0f : 0.0f;
        return clamp((x - low)/(high - low), 0.0f, 1.0f);
    }

    static float smoothramp(float x, float low, float high)
    {
        float t = rampfactor(x, low, high);
        return t*t*(3.0f - 2.0f*t);
    }

    static float acousticPercentile(vector<float> &values, float p, float fallback)
    {
        if(values.empty()) return fallback;
        values.sort();
        float pos = clamp(p, 0.0f, 1.0f)*(values.length() - 1),
              frac = pos - floorf(pos);
        int lo = clamp(int(pos), 0, values.length() - 1),
            hi = min(lo + 1, values.length() - 1);
        return values[lo] + (values[hi] - values[lo])*frac;
    }

    static EFXEAXREVERBPROPERTIES blendEfx(const EFXEAXREVERBPROPERTIES &a, const EFXEAXREVERBPROPERTIES &b, float t)
    {
        t = clamp(t, 0.0f, 1.0f);
        EFXEAXREVERBPROPERTIES out;
        #define BLEND_EFX_FIELD(name) out.name = a.name + (b.name - a.name)*t
        BLEND_EFX_FIELD(flDensity);
        BLEND_EFX_FIELD(flDiffusion);
        BLEND_EFX_FIELD(flGain);
        BLEND_EFX_FIELD(flGainHF);
        BLEND_EFX_FIELD(flGainLF);
        BLEND_EFX_FIELD(flDecayTime);
        BLEND_EFX_FIELD(flDecayHFRatio);
        BLEND_EFX_FIELD(flDecayLFRatio);
        BLEND_EFX_FIELD(flReflectionsGain);
        BLEND_EFX_FIELD(flReflectionsDelay);
        loopi(3) BLEND_EFX_FIELD(flReflectionsPan[i]);
        BLEND_EFX_FIELD(flLateReverbGain);
        BLEND_EFX_FIELD(flLateReverbDelay);
        loopi(3) BLEND_EFX_FIELD(flLateReverbPan[i]);
        BLEND_EFX_FIELD(flEchoTime);
        BLEND_EFX_FIELD(flEchoDepth);
        BLEND_EFX_FIELD(flModulationTime);
        BLEND_EFX_FIELD(flModulationDepth);
        BLEND_EFX_FIELD(flAirAbsorptionGainHF);
        BLEND_EFX_FIELD(flHFReference);
        BLEND_EFX_FIELD(flLFReference);
        BLEND_EFX_FIELD(flRoomRolloffFactor);
        #undef BLEND_EFX_FIELD
        out.iDecayHFLimit = t < 0.5f ? a.iDecayHFLimit : b.iDecayHFLimit;
        return out;
    }

    static AcousticChoice chooseAcousticPresets(const float *scores, bool outdoor, int fallback)
    {
        AcousticChoice choice;
        choice.first = fallback;
        choice.second = fallback;
        float best = -1.0f, next = -1.0f;
        loopi(AP_NUM) if(acousticPresets[i].outdoor == outdoor)
        {
            float score = scores[i];
            if(score > best)
            {
                next = best;
                choice.second = choice.first;
                best = score;
                choice.first = i;
            }
            else if(score > next)
            {
                next = score;
                choice.second = i;
            }
        }
        if(best <= 1e-4f)
        {
            choice.first = choice.second = fallback;
            choice.firstWeight = 1.0f;
            choice.secondWeight = 0.0f;
            return choice;
        }
        if(next <= 1e-4f || choice.second == choice.first)
        {
            choice.second = choice.first;
            choice.firstWeight = 1.0f;
            choice.secondWeight = 0.0f;
            return choice;
        }
        float total = best + next;
        choice.firstWeight = best/total;
        choice.secondWeight = next/total;
        return choice;
    }

    static float airAbsorptionDbPerMeter(float frequency)
    {
        float tempK = airtemperature + 273.15f,
              tempRel = tempK/293.15f,
              pressure = max(airpressure, 0.1f),
              humidity = clamp(airhumidity, 0.0f, 100.0f);
        float saturation = powf(10.0f, -6.8346f*powf(273.16f/tempK, 1.261f) + 4.6151f),
              h = (humidity/100.0f)*saturation/pressure;
        float frO = pressure*(24.0f + 4.04e4f*h*(0.02f + h)/(0.391f + h)),
              frN = pressure*powf(tempRel, -0.5f)*(9.0f + 280.0f*h*expf(-4.17f*(powf(tempRel, -1.0f/3.0f) - 1.0f))),
              f2 = frequency*frequency;
        return 8.686f*f2*(1.84e-11f*sqrtf(tempRel)/pressure +
            powf(tempRel, -2.5f)*(0.01275f*expf(-2239.1f/tempK)/(frO + f2/frO) +
                                  0.1068f*expf(-3352.0f/tempK)/(frN + f2/frN)));
    }

    static float soundDistanceGain(float dist)
    {
        float meters = unitsToMeters(dist);
        if(meters <= 0) return 1.0f;

        float rolloffMeters = max(meters - soundattenuationneardistance, 0.0f),
              halfDistance = max(soundattenuationhalfdistance, 1.0f);
        float attenuationDb = 20.0f*log10f(1.0f + rolloffMeters/halfDistance) +
                              airAbsorptionDbPerMeter(SoundLoudnessFrequency)*meters;
        attenuationDb *= sounddistanceattenuationfactor;
        return clamp(powf(10.0f, -attenuationDb/20.0f), 0.0f, 1.0f);
    }

    static float soundMuffleGainHF(float dist)
    {
        float meters = unitsToMeters(dist);
        if(meters <= 0) return 1.0f;

        float extraDb = max(0.0f, airAbsorptionDbPerMeter(SoundMuffleFrequency) -
                                  airAbsorptionDbPerMeter(SoundLoudnessFrequency))*meters;
        extraDb *= soundmufflingfactor;
        return clamp(powf(10.0f, -extraDb/20.0f), 0.0f, 1.0f);
    }

    static float soundDistanceFarMeters()
    {
        float farMeters = soundattenuationneardistance + max(soundattenuationhalfdistance, 1.0f)*4.0f;
        if(maxsounddistance > soundattenuationneardistance) farMeters = min(farMeters, maxsounddistance);
        return max(farMeters, soundattenuationneardistance + 1.0f);
    }

    static float soundDistanceEffect(float dist)
    {
        float meters = unitsToMeters(dist);
        if(meters <= soundattenuationneardistance) return 0.0f;
        return smoothramp(meters, soundattenuationneardistance, soundDistanceFarMeters());
    }

    static int soundDistanceAttackMillis(float dist)
    {
        float t = rampfactor(unitsToMeters(dist), soundattenuationneardistance, soundDistanceFarMeters());
        return clamp(int(sounddistanceattack*t + 0.5f), 0, sounddistanceattack);
    }

    static float soundDistanceReverbSend(float distanceEffect)
    {
        return clamp(powf(clamp(distanceEffect, 0.0f, 1.0f), 0.60f)*sounddistancereverb, 0.0f, 1.0f);
    }

    static float soundDistanceBassGain(float distanceEffect)
    {
        float cutDb = 42.0f*clamp(distanceEffect, 0.0f, 1.0f)*sounddistancebasscut;
        return clamp(powf(10.0f, -cutDb/20.0f), 0.01f, 1.0f);
    }

    struct AcousticProbe
    {
        vec origin;
        int lastmillis, lastDebugMillis, cell;
        float openness, walldist, reverbGain, reverbDecay, reflection, outdoorRatio, muffleOpen, scores[AP_NUM];
        bool baked;
        EFXEAXREVERBPROPERTIES reverbShape;
        AcousticChoice indoorChoice, outdoorChoice;

        AcousticProbe() : origin(0, 0, 0), lastmillis(0), lastDebugMillis(0), cell(-1), openness(1), walldist(0), reverbGain(0), reverbDecay(0.3f), reflection(0), outdoorRatio(1), muffleOpen(1), baked(false)
        {
            loopi(AP_NUM) scores[i] = 0.0f;
            EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
            reverbShape = generic;
            indoorChoice.first = indoorChoice.second = AP_HALL;
            outdoorChoice.first = outdoorChoice.second = AP_OPENOUTDOOR;
        }
    };

    static AcousticProbe acousticProbe;
    static float currentReverbGain = -1.0f, currentReverbDecay = -1.0f, currentReverbReflection = -1.0f, currentReverbDensity = -1.0f, currentReverbGainHF = -1.0f, currentReverbLateGain = -1.0f;
    static vector<AcousticCell> acousticCells;
    static vector<AcousticRegion> acousticRegions;
    static vector<AcousticPortal> acousticPortals;
    static hashtable<ivec, int> acousticCellLookup(1<<12);

    static ivec acousticCellCoord(const vec &o)
    {
        int size = max(soundacousticcellsize, 16);
        return ivec(int(floorf(o.x/size)), int(floorf(o.y/size)), int(floorf(o.z/size)));
    }

    static vec acousticCellCenter(const ivec &coord)
    {
        float size = max(soundacousticcellsize, 16);
        return vec((coord.x + 0.5f)*size, (coord.y + 0.5f)*size, (coord.z + 0.5f)*size);
    }

    static int acousticCellIndex(const ivec &coord)
    {
        int *idx = acousticCellLookup.access(coord);
        return idx ? *idx : -1;
    }

    static AcousticCell *findAcousticCell(const vec &o)
    {
        if(!soundacousticgrid || acousticCells.empty()) return NULL;
        int idx = acousticCellIndex(acousticCellCoord(o));
        return acousticCells.inrange(idx) && acousticCells[idx].valid ? &acousticCells[idx] : NULL;
    }

    static bool acousticPointIsAir(const vec &p, float clearance)
    {
        if(!insideworld(p)) return false;
        static const vec dirs[6] =
        {
            vec(1, 0, 0), vec(-1, 0, 0), vec(0, 1, 0), vec(0, -1, 0), vec(0, 0, 1), vec(0, 0, -1)
        };
        loopi(6) if(raycube(p, dirs[i], clearance, RAY_CLIPMAT|RAY_POLY) >= clearance*0.95f) return true;
        return false;
    }

    static void scoreAcousticCell(AcousticCell &cell, float hits, float skyOpen, float ceilingDist, float horizontalHitRatio, float horizontalNearRatio,
        float horizontalFarRatio, float horizontalFarHitRatio, float horizontalOpenRatio, float medianHorizontalDistance, float horizontalVariance,
        float nearPercentile, float medianHitDistance, float farPercentile, float corridorScore, float downOpenRatio, float range)
    {
        float varianceScore = clamp(sqrtf(horizontalVariance)/max(metersToUnits(medianHorizontalDistance), 1.0f), 0.0f, 1.0f),
              percentileSpread = clamp((farPercentile - nearPercentile)/max(farPercentile, 1.0f), 0.0f, 1.0f),
              irregularityScore = clamp(varianceScore*0.65f + percentileSpread*0.35f, 0.0f, 1.0f),
              outdoorRaw = smoothramp(skyOpen, 0.35f, 0.70f),
              indoorRaw = 1.0f - outdoorRaw,
              ceilingOpen = skyOpen,
              ceilingBlocked = 1.0f - ceilingOpen,
              fill = clamp(horizontalNearRatio + horizontalHitRatio - horizontalFarHitRatio, 0.0f, 1.0f),
              roomSizeScore = 1.0f - smoothramp(medianHorizontalDistance, 6.0f, 24.0f);

        cell.presetScores[AP_SMALLROOM] = indoorRaw*(0.35f + horizontalHitRatio*0.65f)*roomSizeScore*(1.0f - irregularityScore*0.45f)*(1.0f - horizontalFarHitRatio*0.35f)*0.80f;
        cell.presetScores[AP_HALL] = indoorRaw*(0.30f + horizontalHitRatio*0.70f)*smoothramp(medianHorizontalDistance, 8.0f, 28.0f)*(0.40f + horizontalFarHitRatio*0.60f)*(1.0f - corridorScore*0.75f)*(1.0f - irregularityScore*0.35f)*1.05f;
        cell.presetScores[AP_CORRIDOR] = indoorRaw*(0.25f + horizontalHitRatio*0.75f)*corridorScore*(0.60f + fill*0.40f)*1.55f;
        cell.presetScores[AP_CAVE] = indoorRaw*(0.25f + hits*0.50f)*smoothramp(irregularityScore, 0.35f, 0.85f)*(0.25f + horizontalFarHitRatio*0.45f)*(0.35f + ceilingBlocked*0.35f)*(1.0f - corridorScore*0.70f)*0.85f;
        cell.presetScores[AP_OPENOUTDOOR] = outdoorRaw*skyOpen*(0.45f + horizontalOpenRatio*0.55f)*(1.0f - horizontalNearRatio)*(1.0f - corridorScore*0.50f);
        cell.presetScores[AP_COURTYARD] = outdoorRaw*skyOpen*(0.25f + horizontalNearRatio*0.75f)*(0.25f + horizontalHitRatio*0.75f)*(1.0f - corridorScore*0.40f);
        cell.presetScores[AP_STREET] = outdoorRaw*skyOpen*(0.25f + horizontalNearRatio*0.75f)*corridorScore;
        cell.presetScores[AP_CANYON] = outdoorRaw*skyOpen*(0.25f + horizontalFarHitRatio*0.75f)*(0.35f + irregularityScore*0.65f)*(0.70f + downOpenRatio*0.30f);

        if(indoorRaw > 0.2f && cell.presetScores[AP_SMALLROOM] + cell.presetScores[AP_HALL] + cell.presetScores[AP_CORRIDOR] + cell.presetScores[AP_CAVE] <= 1e-4f)
            cell.presetScores[medianHorizontalDistance < 10.0f ? AP_SMALLROOM : AP_HALL] = indoorRaw;
        if(outdoorRaw > 0.2f && cell.presetScores[AP_OPENOUTDOOR] + cell.presetScores[AP_COURTYARD] + cell.presetScores[AP_STREET] + cell.presetScores[AP_CANYON] <= 1e-4f)
            cell.presetScores[AP_OPENOUTDOOR] = outdoorRaw;

        cell.indoorChoice = chooseAcousticPresets(cell.presetScores, false, AP_HALL);
        cell.outdoorChoice = chooseAcousticPresets(cell.presetScores, true, AP_OPENOUTDOOR);

        EFXEAXREVERBPROPERTIES indoorShape = blendEfx(acousticPresets[cell.indoorChoice.first].efx, acousticPresets[cell.indoorChoice.second].efx, cell.indoorChoice.secondWeight),
                                outdoorShape = blendEfx(acousticPresets[cell.outdoorChoice.first].efx, acousticPresets[cell.outdoorChoice.second].efx, cell.outdoorChoice.secondWeight);
        cell.reverbShape = blendEfx(indoorShape, outdoorShape, outdoorRaw);

        float best = -1.0f, next = -1.0f;
        cell.primaryPreset = AP_OPENOUTDOOR;
        cell.secondaryPreset = AP_OPENOUTDOOR;
        loopi(AP_NUM)
        {
            float score = cell.presetScores[i];
            if(score > best)
            {
                next = best;
                cell.secondaryPreset = cell.primaryPreset;
                best = score;
                cell.primaryPreset = i;
            }
            else if(score > next)
            {
                next = score;
                cell.secondaryPreset = i;
            }
        }
        float total = max(best + max(next, 0.0f), 1e-4f);
        cell.presetBlend = clamp(max(next, 0.0f)/total, 0.0f, 1.0f);

        float indoorStrength = (1.0f - outdoorRaw)*clamp(0.25f + hits*0.35f + horizontalFarHitRatio*0.25f + fill*0.15f, 0.0f, 1.0f),
              outdoorStrength = outdoorRaw*clamp(0.07f + horizontalNearRatio*0.35f + horizontalFarHitRatio*0.30f + corridorScore*0.25f, 0.04f, 0.75f),
              indoorMuffleOpen = clamp(smoothramp(medianHorizontalDistance, 6.0f, 36.0f)*0.70f + horizontalOpenRatio*0.30f, 0.0f, 1.0f),
              outdoorMuffleOpen = clamp(skyOpen*0.55f + horizontalOpenRatio*0.35f + smoothramp(medianHorizontalDistance, 24.0f, 96.0f)*0.10f, 0.0f, 1.0f);

        cell.skyOpenness = skyOpen;
        cell.hitRatio = hits;
        cell.nearWallRatio = horizontalNearRatio;
        cell.farWallRatio = horizontalFarRatio;
        cell.nearDistance = nearPercentile;
        cell.medianDistance = medianHitDistance;
        cell.farDistance = farPercentile;
        cell.distanceVariance = horizontalVariance;
        cell.corridorScore = corridorScore;
        cell.verticalOpenness = clamp(skyOpen*0.7f + downOpenRatio*0.3f, 0.0f, 1.0f);
        cell.clutterScore = fill;
        cell.irregularityScore = irregularityScore;
        cell.outdoorConnectivity = outdoorRaw;
        cell.outdoorRatio = outdoorRaw;
        cell.openness = clamp(1.0f - hits, 0.0f, 1.0f);
        cell.reverbGain = clamp(indoorStrength + outdoorStrength, 0.0f, 1.0f);
        cell.reverbDecay = cell.reverbShape.flDecayTime;
        cell.reflection = clamp(horizontalNearRatio*0.35f + horizontalFarHitRatio*0.25f + corridorScore*0.30f + hits*0.15f, 0.0f, 1.0f);
        cell.muffleOpen = clamp(indoorMuffleOpen*(1.0f - outdoorRaw) + outdoorMuffleOpen*outdoorRaw, 0.0f, 1.0f);
        cell.confidence = clamp(cell.airOccupancy*(1.0f - cell.boundary*0.35f)*(0.45f + hits*0.30f + min(range, ceilingDist + metersToUnits(medianHitDistance))/max(range, 1.0f)*0.25f), 0.05f, 1.0f);
    }

    static void bakeAcousticCellRays(AcousticCell &cell, int rays, float range)
    {
        rays = clamp(rays, 16, 256);
        const float golden = PI*(3.0f - sqrtf(5.0f)),
                    nearDist = metersToUnits(6.0f),
                    farDist = metersToUnits(24.0f),
                    maxHitDist = range*0.98f;
        float open = 0, hits = 0, skyOpen = 1.0f, ceilingDist = 0,
              skyCount = 0, skyOpenCount = 0, ceilingHits = 0,
              skyDiagCount = 0, skyDiagOpen = 0,
              horizontalCount = 0, horizontalHits = 0, horizontalNear = 0, horizontalFar = 0, horizontalFarHits = 0, horizontalDist = 0, horizontalDist2 = 0,
              downCount = 0, downOpen = 0;
        vector<float> hitDistances, horizontalDistances;

        struct AcousticSector
        {
            int count, hits;
            float dist, nearHits;

            AcousticSector() : count(0), hits(0), dist(0), nearHits(0) {}
        } sectors[8];

        loopi(rays)
        {
            float z = 1.0f - (2.0f*(i + 0.5f))/rays,
                  r = sqrtf(max(0.0f, 1.0f - z*z)),
                  a = golden*i;
            vec dir(cosf(a)*r, sinf(a)*r, z);
            float dist = raycube(cell.origin, dir, range, RAY_CLIPMAT|RAY_POLY|RAY_SKIPFIRST);
            dist = clamp(dist, 0.0f, range);
            bool hit = dist < maxHitDist;
            open += dist/range;
            if(hit)
            {
                hits += 1.0f;
                hitDistances.add(unitsToMeters(dist));
            }
            if(dir.z > 0.25f)
            {
                skyDiagCount += 1.0f;
                skyDiagOpen += hit ? smoothramp(dist/range, 0.78f, 0.98f) : 1.0f;
            }
            if(dir.z > 0.65f)
            {
                skyCount += 1.0f;
                if(!hit) skyOpenCount += 1.0f;
                else
                {
                    ceilingHits += 1.0f;
                    ceilingDist += dist;
                }
            }
            else if(fabs(dir.z) < 0.30f)
            {
                horizontalCount += 1.0f;
                horizontalDist += dist;
                horizontalDist2 += dist*dist;
                horizontalDistances.add(unitsToMeters(dist));
                if(hit)
                {
                    horizontalHits += 1.0f;
                    if(dist <= nearDist) horizontalNear += 1.0f;
                    if(dist >= farDist) horizontalFarHits += 1.0f;
                }
                if(dist >= farDist) horizontalFar += 1.0f;

                float yaw = atan2f(dir.y, dir.x);
                int sector = clamp(int(floorf((yaw + PI)*(8.0f/(2.0f*PI)))), 0, 7);
                sectors[sector].count++;
                sectors[sector].dist += dist;
                if(hit)
                {
                    sectors[sector].hits++;
                    if(dist <= nearDist) sectors[sector].nearHits += 1.0f;
                }
            }
            else if(dir.z < -0.35f)
            {
                downCount += 1.0f;
                if(!hit) downOpen += 1.0f;
            }
        }

        open /= rays;
        hits /= rays;
        if(skyCount > 0) skyOpen = skyOpenCount/skyCount;
        float verticalSkyOpen = skyOpen,
              diagonalSkyOpen = skyDiagCount > 0 ? skyDiagOpen/skyDiagCount : verticalSkyOpen;
        skyOpen = clamp(max(verticalSkyOpen*0.65f + diagonalSkyOpen*0.35f, diagonalSkyOpen*0.55f), 0.0f, 1.0f);
        if(ceilingHits > 0) ceilingDist /= ceilingHits;

        float horizontalHitRatio = horizontalCount > 0 ? horizontalHits/horizontalCount : hits,
              horizontalNearRatio = horizontalCount > 0 ? horizontalNear/horizontalCount : 0.0f,
              horizontalFarRatio = horizontalCount > 0 ? horizontalFar/horizontalCount : open,
              horizontalFarHitRatio = horizontalCount > 0 ? horizontalFarHits/horizontalCount : 0.0f,
              horizontalOpenRatio = max(horizontalFarRatio - horizontalFarHitRatio, 0.0f),
              horizontalAvgDist = horizontalCount > 0 ? horizontalDist/horizontalCount : open*range,
              horizontalVariance = 0.0f,
              downOpenRatio = downCount > 0 ? downOpen/downCount : 0.0f;
        if(horizontalCount > 1)
        {
            float mean = horizontalAvgDist;
            horizontalVariance = max(horizontalDist2/horizontalCount - mean*mean, 0.0f);
        }

        float avgHitDistance = hitDistances.empty() ? unitsToMeters(horizontalAvgDist) : 0.0f;
        loopv(hitDistances) avgHitDistance += hitDistances[i];
        if(!hitDistances.empty()) avgHitDistance /= hitDistances.length();
        float nearPercentile = acousticPercentile(hitDistances, 0.25f, avgHitDistance),
              medianHitDistance = acousticPercentile(hitDistances, 0.50f, avgHitDistance),
              farPercentile = acousticPercentile(hitDistances, 0.75f, avgHitDistance),
              medianHorizontalDistance = acousticPercentile(horizontalDistances, 0.50f, unitsToMeters(horizontalAvgDist));

        float sectorOpen[8], sectorNear[8];
        loopi(8)
        {
            sectorOpen[i] = sectors[i].count ? clamp(sectors[i].dist/(sectors[i].count*range), 0.0f, 1.0f) : open;
            sectorNear[i] = sectors[i].count ? clamp(sectors[i].nearHits/sectors[i].count, 0.0f, 1.0f) : horizontalNearRatio;
        }
        float sectorCorridor = 0.0f;
        loopi(4)
        {
            int a = i, b = i + 4, side1 = (i + 2)&7, side2 = (i + 6)&7;
            float alongFar = min(sectorOpen[a], sectorOpen[b]),
                  sideClosed = 1.0f - 0.5f*(sectorOpen[side1] + sectorOpen[side2]),
                  sideNear = 0.5f*(sectorNear[side1] + sectorNear[side2]);
            sectorCorridor = max(sectorCorridor, clamp(alongFar*(sideClosed*0.55f + sideNear*0.45f), 0.0f, 1.0f));
        }
        float varianceScore = clamp(sqrtf(horizontalVariance)/max(horizontalAvgDist, 1.0f), 0.0f, 1.0f),
              corridorScore = clamp(max(min(horizontalNearRatio, horizontalFarRatio)*max(varianceScore, 0.25f), smoothramp(sectorCorridor, 0.06f, 0.36f)), 0.0f, 1.0f);

        scoreAcousticCell(cell, hits, skyOpen, ceilingDist, horizontalHitRatio, horizontalNearRatio, horizontalFarRatio, horizontalFarHitRatio,
            horizontalOpenRatio, medianHorizontalDistance, horizontalVariance, nearPercentile, medianHitDistance, farPercentile, corridorScore, downOpenRatio, range);
    }

    static bool bakeAcousticCell(AcousticCell &cell, const ivec &coord, int rays, float range)
    {
        int cellsize = max(soundacousticcellsize, 16);
        float half = cellsize*0.5f,
              sampleHalf = half*0.92f,
              clearance = clamp(cellsize*0.08f, 1.5f, 4.0f);
        vec center = acousticCellCenter(coord), centroid(0, 0, 0), nearest(center);
        float nearestdist = 1e16f;
        int samples = 0, air = 0;

        loopi(8)
        {
            vec p(center.x + (i&1 ? sampleHalf : -sampleHalf), center.y + (i&2 ? sampleHalf : -sampleHalf), center.z + (i&4 ? sampleHalf : -sampleHalf));
            samples++;
            if(acousticPointIsAir(p, clearance))
            {
                air++;
                centroid.add(p);
                float dist = p.squaredist(center);
                if(dist < nearestdist) { nearestdist = dist; nearest = p; }
            }
        }

        static const vec faces[6] =
        {
            vec(1, 0, 0), vec(-1, 0, 0), vec(0, 1, 0), vec(0, -1, 0), vec(0, 0, 1), vec(0, 0, -1)
        };
        samples++;
        if(acousticPointIsAir(center, clearance))
        {
            air++;
            centroid.add(center);
            nearest = center;
            nearestdist = 0;
        }
        loopi(6)
        {
            vec p = vec(faces[i]).mul(sampleHalf).add(center);
            samples++;
            if(acousticPointIsAir(p, clearance))
            {
                air++;
                centroid.add(p);
                float dist = p.squaredist(center);
                if(dist < nearestdist) { nearestdist = dist; nearest = p; }
            }
        }

        if(!air) return false;

        cell = AcousticCell();
        cell.coord = coord;
        cell.airOccupancy = air/float(samples);
        cell.boundary = air < samples;
        cell.origin = air > 1 ? centroid.div(float(air)) : nearest;
        cell.valid = true;
        bakeAcousticCellRays(cell, rays, range);
        return true;
    }

    static bool acousticCellsCompatible(const AcousticCell &a, const AcousticCell &b)
    {
        if(!a.valid || !b.valid) return false;
        if(a.primaryPreset == b.primaryPreset) return true;
        if(fabs(a.outdoorRatio - b.outdoorRatio) > 0.35f) return false;
        return max(a.presetScores[b.primaryPreset], b.presetScores[a.primaryPreset]) > 0.20f;
    }

    static void finalizeAcousticGrid()
    {
        static const ivec dirs[6] =
        {
            ivec(1, 0, 0), ivec(-1, 0, 0), ivec(0, 1, 0), ivec(0, -1, 0), ivec(0, 0, 1), ivec(0, 0, -1)
        };

        loopv(acousticCells)
        {
            AcousticCell &cell = acousticCells[i];
            if(!cell.valid) continue;
            cell.connections = 0;
            loopj(6)
            {
                int idx = acousticCellIndex(ivec(cell.coord).add(dirs[j]));
                if(acousticCells.inrange(idx) && acousticCells[idx].valid) cell.connections++;
            }
            if(!cell.connections && cell.airOccupancy <= 0.0f) cell.valid = false;
            if(cell.boundary) cell.confidence *= 0.80f;
        }

        acousticRegions.setsize(0);
        acousticPortals.setsize(0);
        vector<int> pending;
        loopv(acousticCells) acousticCells[i].region = -1;
        loopv(acousticCells)
        {
            AcousticCell &start = acousticCells[i];
            if(!start.valid || start.region >= 0) continue;
            AcousticRegion &region = acousticRegions.add();
            region.id = acousticRegions.length() - 1;
            region.firstCell = i;
            region.primaryPreset = start.primaryPreset;
            region.secondaryPreset = start.secondaryPreset;
            pending.add(i);
            start.region = region.id;
            while(!pending.empty())
            {
                int curidx = pending.pop();
                AcousticCell &cur = acousticCells[curidx];
                region.cellCount++;
                region.center.add(cur.origin);
                region.confidence += cur.confidence;
                region.outdoorRatio += cur.outdoorRatio;
                loopj(6)
                {
                    int nextidx = acousticCellIndex(ivec(cur.coord).add(dirs[j]));
                    if(!acousticCells.inrange(nextidx)) continue;
                    AcousticCell &next = acousticCells[nextidx];
                    if(!next.valid || next.region >= 0 || !acousticCellsCompatible(start, next)) continue;
                    next.region = region.id;
                    pending.add(nextidx);
                }
            }
            if(region.cellCount > 0)
            {
                region.center.div(float(region.cellCount));
                region.confidence = clamp(region.confidence/region.cellCount, 0.0f, 1.0f);
                region.outdoorRatio = clamp(region.outdoorRatio/region.cellCount, 0.0f, 1.0f);
            }
        }

        loopv(acousticCells)
        {
            AcousticCell &cell = acousticCells[i];
            if(!cell.valid || cell.region < 0) continue;
            loopj(6)
            {
                int nextidx = acousticCellIndex(ivec(cell.coord).add(dirs[j]));
                if(!acousticCells.inrange(nextidx)) continue;
                AcousticCell &next = acousticCells[nextidx];
                if(!next.valid || next.region <= cell.region) continue;
                AcousticPortal &portal = acousticPortals.add();
                portal.regionA = cell.region;
                portal.regionB = next.region;
                portal.center = vec(cell.origin).add(next.origin).mul(0.5f);
                portal.normal = vec(next.origin).sub(cell.origin);
                if(!portal.normal.iszero()) portal.normal.normalize();
                portal.apertureSize = max(soundacousticcellsize, 16)*min(cell.airOccupancy, next.airOccupancy);
                portal.openingStrength = clamp((cell.airOccupancy + next.airOccupancy)*0.5f, 0.0f, 1.0f);
                portal.acousticCost = 1.0f - portal.openingStrength;
                portal.highFrequencyLoss = clamp((cell.boundary || next.boundary ? 0.35f : 0.10f) + portal.acousticCost*0.40f, 0.0f, 1.0f);
                portal.diffractionCost = clamp((1.0f - portal.openingStrength)*0.75f + max(cell.nearWallRatio, next.nearWallRatio)*0.25f, 0.0f, 1.0f);
            }
        }
    }

    void clearAcousticGrid()
    {
        acousticCells.setsize(0);
        acousticRegions.setsize(0);
        acousticPortals.setsize(0);
        acousticCellLookup.clear();
        acousticProbe.baked = false;
        acousticProbe.cell = -1;
    }

    int numAcousticCells() { return acousticCells.length(); }
    int numAcousticRegions() { return acousticRegions.length(); }
    int numAcousticPortals() { return acousticPortals.length(); }

    void bakeAcousticGrid(int cellsize, int rays)
    {
        if(worldsize <= 0)
        {
            conoutf(CON_WARN, "sound acoustics: no world for acoustic bake");
            return;
        }
        if(cellsize > 0) soundacousticcellsize = clamp(cellsize, 16, 128);
        rays = clamp(rays > 0 ? rays : soundacousticbakerays, 16, 256);
        soundacousticbakerays = rays;

        clearAcousticGrid();
        int csize = max(soundacousticcellsize, 16),
            cellsperaxis = (worldsize + csize - 1)/csize;
        float range = metersToUnits(soundacousticrange);
        loop(x, cellsperaxis) loop(y, cellsperaxis) loop(z, cellsperaxis)
        {
            ivec coord(x, y, z);
            AcousticCell cell;
            if(!bakeAcousticCell(cell, coord, rays, range)) continue;
            int idx = acousticCells.length();
            acousticCells.add(cell);
            acousticCellLookup[coord] = idx;
        }
        finalizeAcousticGrid();
        conoutf(CON_DEBUG, "sound acoustics: baked whole map: %d cells, %d regions, %d portals (%d unit cells, %d rays)",
            acousticCells.length(), acousticRegions.length(), acousticPortals.length(), csize, rays);
    }

    static float smoothstepfactor(int elapsed)
    {
        if(soundacousticsmooth <= 0) return 1.0f;
        return clamp(elapsed/float(soundacousticsmooth), 0.0f, 1.0f);
    }

    static void applyAcousticCell(const AcousticCell &cell, int elapsed)
    {
        float k = acousticProbe.walldist <= 0 ? 1.0f : smoothstepfactor(elapsed);
        acousticProbe.baked = true;
        acousticProbe.cell = acousticCellIndex(cell.coord);
        loopi(AP_NUM) acousticProbe.scores[i] += (cell.presetScores[i] - acousticProbe.scores[i])*k;
        acousticProbe.indoorChoice = cell.indoorChoice;
        acousticProbe.outdoorChoice = cell.outdoorChoice;
        acousticProbe.outdoorRatio += (cell.outdoorRatio - acousticProbe.outdoorRatio)*k;
        acousticProbe.openness += (cell.openness - acousticProbe.openness)*k;
        acousticProbe.walldist += (metersToUnits(cell.medianDistance) - acousticProbe.walldist)*k;
        acousticProbe.reverbGain += (cell.reverbGain - acousticProbe.reverbGain)*k;
        acousticProbe.reverbDecay += (cell.reverbDecay - acousticProbe.reverbDecay)*k;
        acousticProbe.reflection += (cell.reflection - acousticProbe.reflection)*k;
        acousticProbe.muffleOpen += (cell.muffleOpen - acousticProbe.muffleOpen)*k;
        acousticProbe.reverbShape = cell.reverbShape;

        if(debugsoundacoustics && totalmillis - acousticProbe.lastDebugMillis >= 1000)
        {
            acousticProbe.lastDebugMillis = totalmillis;
            conoutf(CON_DEBUG, "sound acoustics baked: region %d, %s %d%% / %s %d%%, occupancy %d%%, confidence %d%%, sky %d%%, median %.1fm, irregular %d%%, corridor %d%%",
                cell.region, acousticPresets[cell.primaryPreset].name, int((1.0f - cell.presetBlend)*100.0f + 0.5f),
                acousticPresets[cell.secondaryPreset].name, int(cell.presetBlend*100.0f + 0.5f),
                int(cell.airOccupancy*100.0f + 0.5f), int(cell.confidence*100.0f + 0.5f), int(cell.skyOpenness*100.0f + 0.5f),
                cell.medianDistance, int(cell.irregularityScore*100.0f + 0.5f), int(cell.corridorScore*100.0f + 0.5f));
        }
    }

    static void updateEfxReverb()
    {
        if(!efxReverb || !efxReverbEffect || !efxReverbSlot) return;
        bool usebaked = soundacoustics && acousticProbe.baked;
        EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
        const EFXEAXREVERBPROPERTIES &shape = usebaked ? acousticProbe.reverbShape : generic;
        float gain = usebaked ? clamp(shape.flGain*acousticProbe.reverbGain*soundacousticreverb, 0.0f, 1.0f) : clamp(0.65f*sounddistancereverb, 0.0f, 1.0f),
              decay = clamp(usebaked ? acousticProbe.reverbDecay : shape.flDecayTime, 0.12f, 4.0f),
              reflection = clamp(shape.flReflectionsGain*(usebaked ? 0.35f + acousticProbe.reflection*0.65f : 1.0f), 0.0f, 3.16f),
              density = clamp(shape.flDensity, 0.0f, 1.0f),
              gainhf = clamp(shape.flGainHF, 0.0f, 1.0f),
              lateGain = clamp(shape.flLateReverbGain, 0.0f, 10.0f);
        if(fabs(gain - currentReverbGain) < 0.015f && fabs(decay - currentReverbDecay) < 0.05f && fabs(reflection - currentReverbReflection) < 0.03f &&
           fabs(density - currentReverbDensity) < 0.03f && fabs(gainhf - currentReverbGainHF) < 0.03f && fabs(lateGain - currentReverbLateGain) < 0.05f) return;

        currentReverbGain = gain;
        currentReverbDecay = decay;
        currentReverbReflection = reflection;
        currentReverbDensity = density;
        currentReverbGainHF = gainhf;
        currentReverbLateGain = lateGain;
        alEffectf_(efxReverbEffect, AL_REVERB_DENSITY, density);
        alEffectf_(efxReverbEffect, AL_REVERB_DIFFUSION, clamp(shape.flDiffusion, 0.0f, 1.0f));
        alEffectf_(efxReverbEffect, AL_REVERB_GAIN, gain);
        alEffectf_(efxReverbEffect, AL_REVERB_GAINHF, gainhf);
        alEffectf_(efxReverbEffect, AL_REVERB_DECAY_TIME, decay);
        alEffectf_(efxReverbEffect, AL_REVERB_DECAY_HFRATIO, clamp(shape.flDecayHFRatio, 0.1f, 2.0f));
        alEffectf_(efxReverbEffect, AL_REVERB_REFLECTIONS_GAIN, reflection);
        alEffectf_(efxReverbEffect, AL_REVERB_REFLECTIONS_DELAY, clamp(shape.flReflectionsDelay, 0.0f, 0.3f));
        alEffectf_(efxReverbEffect, AL_REVERB_LATE_REVERB_GAIN, lateGain);
        alEffectf_(efxReverbEffect, AL_REVERB_LATE_REVERB_DELAY, clamp(shape.flLateReverbDelay, 0.0f, 0.1f));
        alEffectf_(efxReverbEffect, AL_REVERB_AIR_ABSORPTION_GAINHF, clamp(shape.flAirAbsorptionGainHF, 0.892f, 1.0f));
        alEffectf_(efxReverbEffect, AL_REVERB_ROOM_ROLLOFF_FACTOR, clamp(shape.flRoomRolloffFactor, 0.0f, 10.0f));
        alEffecti_(efxReverbEffect, AL_REVERB_DECAY_HFLIMIT, shape.iDecayHFLimit ? AL_TRUE : AL_FALSE);
        alAuxiliaryEffectSloti_(efxReverbSlot, AL_EFFECTSLOT_EFFECT, efxReverbEffect);
        checkAl("OpenAL acoustic reverb update");
    }

    static void updateAcoustics()
    {
        if(!soundacoustics || !camera1)
        {
            acousticProbe.baked = false;
            acousticProbe.cell = -1;
            updateEfxReverb();
            return;
        }

        int now = totalmillis;
        if(!acousticProbe.lastmillis) acousticProbe.lastmillis = now;
        int elapsed = max(now - acousticProbe.lastmillis, 1);
        acousticProbe.lastmillis = now;
        acousticProbe.origin = camera1->o;
        AcousticCell *cell = findAcousticCell(acousticProbe.origin);
        if(cell)
        {
            applyAcousticCell(*cell, elapsed);
            updateEfxReverb();
            return;
        }

        acousticProbe.baked = false;
        acousticProbe.cell = -1;
        updateEfxReverb();
    }

    static void acousticSource(const vec &loc, float dist, float &volf, float &gainhf, float &reverbSend)
    {
        if(!soundacoustics || !acousticProbe.baked || dist <= 1.0f) return;
        vec dir = vec(loc).sub(acousticProbe.origin);
        if(dir.iszero()) return;
        dir.normalize();
        float clear = raycube(acousticProbe.origin, dir, dist, RAY_CLIPMAT|RAY_POLY|RAY_SKIPFIRST),
              pathOpen = clear + 2.0f < dist ? clamp(clear/max(dist, 1.0f), 0.0f, 1.0f) : 1.0f,
              occlusion = clamp(powf(1.0f - pathOpen, 0.75f)*soundacousticocclusion, 0.0f, 1.0f),
              muffleOpen = clamp(pathOpen*0.65f + acousticProbe.muffleOpen*0.35f, 0.0f, 1.0f),
              closedGainHF = clamp(soundacousticmufflegainhf, 0.02f, 1.0f),
              openGainHF = max(closedGainHF, 0.5f),
              effectiveMuffleGainHF = closedGainHF + (openGainHF - closedGainHF)*muffleOpen;
        volf *= 1.0f - occlusion*(1.0f - soundacousticblockgain);
        gainhf *= 1.0f - occlusion*(1.0f - effectiveMuffleGainHF);
        reverbSend = max(reverbSend, clamp((acousticProbe.reverbGain*(0.35f + 0.65f*occlusion))*soundacousticreverb, 0.0f, 1.0f));
    }

    static void acousticHudSource(float &reverbSend)
    {
        if(!soundacoustics || !acousticProbe.baked) return;
        reverbSend = max(reverbSend, clamp(acousticProbe.reverbGain*soundacousticreverb, 0.0f, 1.0f));
    }

    static bvec acousticCellDebugColor(const AcousticCell &cell)
    {
        int r = int((cell.boundary ? 255.0f : 80.0f) + cell.nearWallRatio*80.0f),
            g = int(90.0f + cell.outdoorRatio*150.0f),
            b = int(220.0f - cell.outdoorRatio*130.0f);
        return bvec(clamp(r, 0, 255), clamp(g, 0, 255), clamp(b, 0, 255));
    }

    static void drawAcousticCellLine(const vec *v, int a, int b, const bvec &color, uchar alpha)
    {
        gle::attrib(v[a]); gle::attrib(color, alpha);
        gle::attrib(v[b]); gle::attrib(color, alpha);
    }

    static void drawAcousticsDebug()
    {
        if(!soundacoustics || !debugsoundacoustics || acousticCells.empty()) return;
        ldrnotextureshader->set();
        GLboolean cull = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        gle::defvertex();
        gle::defcolor(4, GL_UNSIGNED_BYTE);

        if(camera1 && soundacousticgrid && !acousticCells.empty())
        {
            int visible = 0,
                maxradius = min(soundacousticgridradius, 256);
            loopv(acousticCells) if(acousticCells[i].valid && acousticCells[i].origin.dist(camera1->o) <= maxradius) visible++;
            if(visible > 0)
            {
                float half = max(soundacousticcellsize, 16)*0.5f;
                gle::begin(GL_LINES, visible*24);
                loopv(acousticCells)
                {
                    const AcousticCell &cell = acousticCells[i];
                    if(!cell.valid || cell.origin.dist(camera1->o) > maxradius) continue;
                    vec c = acousticCellCenter(cell.coord),
                        v[8] =
                        {
                            vec(c.x - half, c.y - half, c.z - half),
                            vec(c.x + half, c.y - half, c.z - half),
                            vec(c.x + half, c.y + half, c.z - half),
                            vec(c.x - half, c.y + half, c.z - half),
                            vec(c.x - half, c.y - half, c.z + half),
                            vec(c.x + half, c.y - half, c.z + half),
                            vec(c.x + half, c.y + half, c.z + half),
                            vec(c.x - half, c.y + half, c.z + half)
                        };
                    bvec color = acousticCellDebugColor(cell);
                    uchar alpha = uchar(clamp(45 + int(cell.confidence*150.0f), 45, 195));
                    drawAcousticCellLine(v, 0, 1, color, alpha);
                    drawAcousticCellLine(v, 1, 2, color, alpha);
                    drawAcousticCellLine(v, 2, 3, color, alpha);
                    drawAcousticCellLine(v, 3, 0, color, alpha);
                    drawAcousticCellLine(v, 4, 5, color, alpha);
                    drawAcousticCellLine(v, 5, 6, color, alpha);
                    drawAcousticCellLine(v, 6, 7, color, alpha);
                    drawAcousticCellLine(v, 7, 4, color, alpha);
                    drawAcousticCellLine(v, 0, 4, color, alpha);
                    drawAcousticCellLine(v, 1, 5, color, alpha);
                    drawAcousticCellLine(v, 2, 6, color, alpha);
                    drawAcousticCellLine(v, 3, 7, color, alpha);
                }
                xtraverts += gle::end();
            }
        }
        glDepthMask(GL_TRUE);
        if(cull) glEnable(GL_CULL_FACE);
    }

    static bool soundInRange(const vec &loc)
    {
        if(soundairattenuation)
        {
            if(maxsounddistance <= 0) return true;
            return camera1->o.dist(loc) <= metersToUnits(maxsounddistance);
        }
        return true;
    }

    static bool updateChannel(SoundChannel &chan)
    {
        if(!chan.slot) return false;
        float volf = 1.0f, panf = 0.5f, gainhf = 1.0f, gainlf = 1.0f, reverbSend = 0.0f, distanceReverbSend = 0.0f;
        updateSoundEntity(chan);
        if(chan.hasLoc())
        {
            vec v;
            float dist = chan.loc.dist(camera1->o, v);
            int rad = 0;
            if(chan.ent)
            {
                rad = chan.ent->attr2;
                if(chan.ent->attr3)
                {
                    rad -= chan.ent->attr3;
                    dist -= chan.ent->attr3;
                }
            }
            else if(chan.radius > 0) rad = chan.radius;
            if(soundairattenuation)
            {
                float distanceEffect = soundDistanceEffect(dist),
                      distanceReverb = soundDistanceReverbSend(distanceEffect);
                volf *= soundDistanceGain(dist);
                gainhf = soundMuffleGainHF(dist);
                gainlf = soundDistanceBassGain(distanceEffect);
                reverbSend = max(reverbSend, distanceReverb);
                distanceReverbSend = distanceReverb;
            }
            else if(rad > 0) volf -= clamp(dist/rad, 0.0f, 1.0f);
            acousticSource(chan.loc, dist, volf, gainhf, reverbSend);
            if(stereo && (v.x != 0 || v.y != 0) && dist>0)
            {
                v.rotate_around_z(-camera1->yaw*RAD);
                panf = 0.5f - 0.5f*v.x/v.magnitude2();
            }
        }
        else if(chan.flags&SND_HUD) acousticHudSource(reverbSend);
        if(!efxReverb) reverbSend = distanceReverbSend = 0.0f;
        else if(!efxDistanceReverbSlot) reverbSend = max(reverbSend, distanceReverbSend);
        int vol = clamp(int(volf*soundvol*chan.slot->volume*(MaxVolume/float(255*255)) + 0.5f), 0, MaxVolume);
        int pan = clamp(int(panf*255.9f), 0, 255);
        if(vol == chan.targetVolume && pan == chan.pan && fabs(gainhf - chan.targetGainHF) < 1e-3f && fabs(gainlf - chan.targetGainLF) < 1e-3f &&
           fabs(reverbSend - chan.targetReverbSend) < 1e-3f && fabs(distanceReverbSend - chan.targetDistanceReverbSend) < 1e-3f) return false;
        chan.targetVolume = vol;
        chan.pan = pan;
        chan.targetGainHF = gainhf;
        chan.targetGainLF = gainlf;
        chan.targetReverbSend = reverbSend;
        chan.targetDistanceReverbSend = distanceReverbSend;
        chan.dirty = true;
        return true;
    }

    static bool sourcePlaying(const SoundChannel &chan)
    {
        if(!chan.source) return false;
        ALint state = AL_STOPPED;
        alGetSourcei(chan.source, AL_SOURCE_STATE, &state);
        return state == AL_PLAYING;
    }

    static void reclaimChannels()
    {
        loopv(channels)
        {
            SoundChannel &chan = channels[i];
            if(!chan.inuse) continue;
            if(chan.expire >= 0 && totalmillis >= chan.expire)
            {
                haltChannel(i);
                continue;
            }
            if(chan.stopping && totalmillis >= chan.fadeEnd)
            {
                haltChannel(i);
                continue;
            }
            if(!sourcePlaying(chan)) freeChannel(i);
        }
    }

    static void syncChannels()
    {
        loopv(channels)
        {
            SoundChannel &chan = channels[i];
            if(!chan.inuse) continue;
            if(chan.hasLoc() || chan.flags&SND_HUD) updateChannel(chan);
            if(chan.fadeEnd > chan.fadeStart && totalmillis < chan.fadeEnd) chan.dirty = true;
            syncChannel(chan);
        }
    }

    VARP(minimizedsounds, 0, 0, 1);

    void update()
    {
        updatemumble();
        if(nosound) return;
        if(minimized && !minimizedsounds) stopAll();
        else
        {
            reclaimChannels();
            if(mainmenu) stopMapSounds();
            else checkMapSounds();
            updateAcoustics();
            syncChannels();
        }
        music.update();
    }

    VARP(maxsoundsatonce, 0, 7, 100);
    VAR(dbgsound, 0, 0, 1);

    void preload(int n)
    {
        gameSounds.preload(n);
    }

    void preloadMap(int n)
    {
        mapSounds.preload(n);
    }

    void preloadMapSounds()
    {
        const vector<extentity *> &ents = entities::getents();
        loopv(ents)
        {
            extentity &e = *ents[i];
            if(e.type==ET_SOUND) mapSounds.preload(e.attr1);
        }
    }

    int play(int n, const vec *loc, extentity *ent, int flags, int loops, int fade, int chanid, int radius, int expire)
    {
        if(nosound || !soundvol || (minimized && !minimizedsounds)) return -1;

        SoundType &sounds = ent || flags&SND_MAP ? mapSounds : gameSounds;
        if(!sounds.configs.inrange(n)) { conoutf(CON_WARN, "unregistered sound: %d", n); return -1; }
        SoundConfig &config = sounds.configs[n];

        if(loc)
        {
            bool outofrange = false;
            if(soundairattenuation) outofrange = !soundInRange(*loc);
            else
            {
                int maxrad = game::maxsoundradius(n);
                if(radius <= 0 || maxrad < radius) radius = maxrad;
                outofrange = camera1->o.dist(*loc) > 1.5f*radius;
            }
            if(outofrange)
            {
                if(channels.inrange(chanid) && sounds.playing(channels[chanid], config)) haltChannel(chanid);
                return -1;
            }
        }

        if(chanid < 0)
        {
            if(config.maxuses)
            {
                int uses = 0;
                loopv(channels) if(sounds.playing(channels[i], config) && ++uses >= config.maxuses) return -1;
            }

            static int soundsatonce = 0, lastsoundmillis = 0;
            if(totalmillis == lastsoundmillis) soundsatonce++; else soundsatonce = 1;
            lastsoundmillis = totalmillis;
            if(maxsoundsatonce && soundsatonce > maxsoundsatonce) return -1;
        }

        if(channels.inrange(chanid))
        {
            SoundChannel &chan = channels[chanid];
            if(sounds.playing(chan, config))
            {
                chan.soundentity = soundEntityId(loc, ent, flags);
                if(loc) chan.loc = *loc;
                else if(chan.hasLoc()) chan.clearLoc();
                chan.flags = flags;
                chan.radius = radius;
                return chanid;
            }
        }
        if(fade < 0) return -1;

        SoundSlot &slot = sounds.slots[config.chooseSlot(flags)];
        if(!slot.sample->buffer && !slot.sample->load(sounds.dir)) return -1;

        if(dbgsound) conoutf(CON_DEBUG, "sound: %s%s", sounds.dir, slot.sample->name);

        chanid = -1;
        loopv(channels) if(!channels[i].inuse) { chanid = i; break; }
        if(chanid < 0 && channels.length() < maxChannels) chanid = channels.length();
        if(chanid < 0) loopv(channels) if(!channels[i].targetVolume) { haltChannel(i); chanid = i; break; }
        if(chanid < 0) return -1;

        SoundChannel &chan = newChannel(chanid, &slot, loc, ent, flags, radius, soundEntityId(loc, ent, flags));
        chan.pitch = randomPitchOffset(loc, flags, ent);
        if(!chan.ensureSource())
        {
            freeChannel(chanid);
            return -1;
        }

        updateChannel(chan);
        chan.expire = expire >= 0 ? totalmillis + expire : -1;
        if(soundairattenuation && chan.hasLoc()) fade = max(fade, soundDistanceAttackMillis(chan.loc.dist(camera1->o)));
        chan.fadeStart = totalmillis;
        chan.fadeEnd = fade > 0 ? totalmillis + fade : totalmillis;
        chan.fadeFrom = fade > 0 ? 0 : chan.targetVolume;
        chan.volume = -1;
        chan.dirty = true;

        alSourceStop(chan.source);
        alSourcei(chan.source, AL_BUFFER, 0);
        alSourcei(chan.source, AL_BUFFER, slot.sample->buffer);
        alSourcei(chan.source, AL_LOOPING, loops ? AL_TRUE : AL_FALSE);
        alSourcef(chan.source, AL_PITCH, chan.pitch);
        syncChannel(chan);
        alSourcePlay(chan.source);
        if(!checkAl("alSourcePlay"))
        {
            freeChannel(chanid);
            return -1;
        }
        return chanid;
    }

    void stopAll()
    {
        loopv(channels) if(channels[i].inuse) haltChannel(i);
    }

    void clearEntities()
    {
        loopv(channels) channels[i].soundentity = 0;
    }

    bool stop(int n, int chanid, int fade)
    {
        if(!gameSounds.configs.inrange(n) || !channels.inrange(chanid) || !gameSounds.playing(channels[chanid], gameSounds.configs[n])) return false;
        SoundChannel &chan = channels[chanid];
        if(dbgsound) conoutf(CON_DEBUG, "stopsound: %s%s", gameSounds.dir, chan.slot->sample->name);
        if(fade > 0)
        {
            chan.fadeStart = totalmillis;
            chan.fadeEnd = totalmillis + fade;
            chan.fadeFrom = effectiveVolume(chan);
            chan.targetVolume = 0;
            chan.stopping = true;
            chan.dirty = true;
            syncChannel(chan);
        }
        else haltChannel(chanid);
        return true;
    }

    int playName(const char *s, const vec *loc, int vol, int flags, int loops, int fade, int chanid, int radius, int expire)
    {
        if(!vol) vol = 100;
        int id = gameSounds.findSound(s, vol);
        if(id < 0) id = gameSounds.addSound(s, vol);
        return play(id, loc, NULL, flags, loops, fade, chanid, radius, expire);
    }

    void reset()
    {
        clearchanges(CHANGE_SOUND);
        char *oldfile = music.filename ? newstring(music.filename) : NULL;
        char *oldcmd = music.donecmd ? newstring(music.donecmd) : NULL;

        music.cleanup(true);
        gameSounds.cleanupSamples();
        mapSounds.cleanupSamples();
        resetChannels();

        if(alContext || alDevice)
        {
            alcMakeContextCurrent(NULL);
            if(alContext) { alcDestroyContext(alContext); alContext = NULL; }
            if(alDevice) { alcCloseDevice(alDevice); alDevice = NULL; }
        }
        clearEfx();
        shouldInitAudio = true;
        init();

        if(nosound)
        {
            DELETEA(oldfile);
            DELETEA(oldcmd);
            gameSounds.cleanupSamples();
            mapSounds.cleanupSamples();
            return;
        }

        if(oldfile)
        {
            if(!music.startFile(oldfile, oldcmd ? oldcmd : ""))
            {
                DELETEA(music.filename);
                DELETEA(music.donecmd);
            }
            DELETEA(oldfile);
            DELETEA(oldcmd);
        }
    }

    void startMusic(char *name, char *cmd)
    {
        if(nosound) return;
        music.cleanup(true);
        if(soundvol && musicvol && *name)
        {
            if(music.startPackage(name, cmd)) intret(1);
            else
            {
                conoutf(CON_ERROR, "could not play music: packages/%s", name);
                intret(0);
            }
        }
    }
}

void registersound(char *name, int *vol) { intret(sound::registerSound(name, *vol)); }
COMMAND(registersound, "si");

void mapsound(char *name, int *vol, int *maxuses) { intret(sound::registerMapSound(name, *vol, *maxuses)); }
COMMAND(mapsound, "sii");

void altsound(char *name, int *vol) { sound::addAltSound(name, *vol); }
COMMAND(altsound, "si");

void altmapsound(char *name, int *vol) { sound::addAltMapSound(name, *vol); }
COMMAND(altmapsound, "si");

ICOMMAND(numsounds, "", (), intret(sound::numSounds()));
ICOMMAND(nummapsounds, "", (), intret(sound::numMapSounds()));
ICOMMAND(soundacousticcells, "", (), intret(sound::numAcousticCells()));
ICOMMAND(soundacousticregions, "", (), intret(sound::numAcousticRegions()));
ICOMMAND(soundacousticportals, "", (), intret(sound::numAcousticPortals()));

void bakesoundacoustics(int *cellsize, int *rays) { sound::bakeAcousticGrid(*cellsize, *rays); }
COMMAND(bakesoundacoustics, "ii");

void clearsoundacousticgrid() { sound::clearAcousticGrid(); }
COMMAND(clearsoundacousticgrid, "");

void soundreset() { sound::soundReset(); }
COMMAND(soundreset, "");

void mapsoundreset() { sound::mapSoundReset(); }
COMMAND(mapsoundreset, "");

void clear_sound() { sound::cleanup(); }
void clearmapsounds() { sound::clearMapSounds(); }
void checkmapsounds() { sound::checkMapSounds(); }
void updatesounds() { sound::update(); }
void rendersounddebug() { sound::drawAcousticsDebug(); }
void preloadsound(int n) { sound::preload(n); }
void preloadmapsound(int n) { sound::preloadMap(n); }
void preloadmapsounds() { sound::preloadMapSounds(); }
void clearsoundentities() { sound::clearEntities(); }
int playsound(int n, const vec *loc, extentity *ent, int flags, int loops, int fade, int chanid, int radius, int expire) { return sound::play(n, loc, ent, flags, loops, fade, chanid, radius, expire); }
void stopsounds() { sound::stopAll(); }
bool stopsound(int n, int chanid, int fade) { return sound::stop(n, chanid, fade); }
int playsoundname(const char *s, const vec *loc, int vol, int flags, int loops, int fade, int chanid, int radius, int expire) { return sound::playName(s, loc, vol, flags, loops, fade, chanid, radius, expire); }

ICOMMAND(playsound, "i", (int *n), sound::play(*n));
ICOMMAND(sound, "i", (int *n), sound::play(*n)); // sauerract | kept for compatibility with sauer

void initsound() { sound::init(); }
void resetsound() { sound::reset(); }
COMMAND(resetsound, "");
void startmusic(char *name, char *cmd) { sound::startMusic(name, cmd); }
COMMANDN(music, startmusic, "ss");

#ifdef WIN32

#include <wchar.h>

#else

#include <unistd.h>

#ifdef _POSIX_SHARED_MEMORY_OBJECTS
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <wchar.h>
#endif

#endif

#if defined(WIN32) || defined(_POSIX_SHARED_MEMORY_OBJECTS)
struct MumbleInfo
{
    int version, timestamp;
    vec pos, front, top;
    wchar_t name[256];
};
#endif

#ifdef WIN32
static HANDLE mumblelink = NULL;
static MumbleInfo *mumbleinfo = NULL;
#define VALID_MUMBLELINK (mumblelink && mumbleinfo)
#elif defined(_POSIX_SHARED_MEMORY_OBJECTS)
static int mumblelink = -1;
static MumbleInfo *mumbleinfo = (MumbleInfo *)-1;
#define VALID_MUMBLELINK (mumblelink >= 0 && mumbleinfo != (MumbleInfo *)-1)
#endif

#ifdef VALID_MUMBLELINK
VARFP(mumble, 0, 1, 1, { if(mumble) initmumble(); else closemumble(); });
#else
VARFP(mumble, 0, 0, 1, { if(mumble) initmumble(); else closemumble(); });
#endif

void initmumble()
{
    if(!mumble) return;
#ifdef VALID_MUMBLELINK
    if(VALID_MUMBLELINK) return;

    #ifdef WIN32
        mumblelink = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, "MumbleLink");
        if(mumblelink)
        {
            mumbleinfo = (MumbleInfo *)MapViewOfFile(mumblelink, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MumbleInfo));
            if(mumbleinfo) wcsncpy(mumbleinfo->name, L"Sauerract", 256);
        }
    #elif defined(_POSIX_SHARED_MEMORY_OBJECTS)
        defformatstring(shmname, "/MumbleLink.%d", getuid());
        mumblelink = shm_open(shmname, O_RDWR, 0);
        if(mumblelink >= 0)
        {
            mumbleinfo = (MumbleInfo *)mmap(NULL, sizeof(MumbleInfo), PROT_READ|PROT_WRITE, MAP_SHARED, mumblelink, 0);
            if(mumbleinfo != (MumbleInfo *)-1) wcsncpy(mumbleinfo->name, L"Sauerract", 256);
        }
    #endif
    if(!VALID_MUMBLELINK) closemumble();
#else
    conoutf(CON_ERROR, "Mumble positional audio is not available on this platform.");
#endif
}

void closemumble()
{
#ifdef WIN32
    if(mumbleinfo) { UnmapViewOfFile(mumbleinfo); mumbleinfo = NULL; }
    if(mumblelink) { CloseHandle(mumblelink); mumblelink = NULL; }
#elif defined(_POSIX_SHARED_MEMORY_OBJECTS)
    if(mumbleinfo != (MumbleInfo *)-1) { munmap(mumbleinfo, sizeof(MumbleInfo)); mumbleinfo = (MumbleInfo *)-1; }
    if(mumblelink >= 0) { close(mumblelink); mumblelink = -1; }
#endif
}

static inline vec mumblevec(const vec &v, bool pos = false)
{
    // change from X left, Z up, Y forward to X right, Y up, Z forward
    // 8 cube units = 1 meter
    vec m(-v.x, v.z, v.y);
    if(pos) m.div(8);
    return m;
}

void updatemumble()
{
#ifdef VALID_MUMBLELINK
    if(!VALID_MUMBLELINK) return;

    static int timestamp = 0;

    mumbleinfo->version = 1;
    mumbleinfo->timestamp = ++timestamp;

    mumbleinfo->pos = mumblevec(player->o, true);
    mumbleinfo->front = mumblevec(vec(player->yaw*RAD, player->pitch*RAD));
    mumbleinfo->top = mumblevec(vec(player->yaw*RAD, (player->pitch+90)*RAD));
#endif
}
