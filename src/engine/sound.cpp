// sound.cpp: basic positional sound using OpenAL Soft and libsndfile

#include "engine.h"
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/efx.h"
#include "AL/efx-presets.h"
#include "sndfile.h"
#include "acoustics.h"

extern vec hitsurface;

namespace acoustics { extern int soundacoustics; }

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

        int chooseSlot(int flags, uint seed = 0) const
        {
            if(flags&SND_NO_ALT || numslots <= 1) return slots;
            if(flags&SND_USE_ALT) return slots + 1 + (seed ? detrnd(seed, numslots - 1) : rnd(numslots - 1));
            return slots + (seed ? detrnd(seed, numslots) : rnd(numslots));
        }
    };

    struct SoundChannel
    {
        int id;
        ALuint source, filter, sendFilter, distanceSendFilter, acousticSource, acousticFilter;
        bool inuse;
        vec loc;
        SoundSlot *slot;
        extentity *ent;
        int radius, volume, targetVolume, pan, acousticPan, targetAcousticPan, flags, expire, soundentity;
        uint eventseed;
        int fadeStart, fadeEnd, fadeFrom;
        float pitch, gainhf, targetGainHF, gainlf, targetGainLF, reverbSend, targetReverbSend, distanceReverbSend, targetDistanceReverbSend,
              acousticGain, targetAcousticGain, acousticGainHF, targetAcousticGainHF;
        vec acousticCacheLoc, acousticCacheListener;
        acoustics::AcousticSourceInfo acousticCacheInfo;
        int acousticCacheMillis;
        float acousticCacheVol, acousticCacheGainHF, acousticCacheReverb;
        bool dirty, stopping, looping, acousticCacheValid;

        SoundChannel(int id) : id(id), source(0), filter(0), sendFilter(0), distanceSendFilter(0), acousticSource(0), acousticFilter(0) { reset(); }
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
            acousticPan = targetAcousticPan = 128;
            flags = 0;
            expire = -1;
            soundentity = 0;
            eventseed = 0;
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
            acousticGain = targetAcousticGain = 0.0f;
            acousticGainHF = targetAcousticGainHF = 1.0f;
            acousticCacheLoc = acousticCacheListener = vec(0, 0, 0);
            acousticCacheInfo = acoustics::AcousticSourceInfo();
            acousticCacheMillis = -1;
            acousticCacheVol = acousticCacheGainHF = 1.0f;
            acousticCacheReverb = 0.0f;
            dirty = false;
            stopping = false;
            looping = false;
            acousticCacheValid = false;
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

        bool ensureAcousticSource()
        {
            if(acousticSource) return true;
            alGenSources(1, &acousticSource);
            if(!checkAl("alGenSources acoustic voice")) { acousticSource = 0; return false; }
            alSourcei(acousticSource, AL_SOURCE_RELATIVE, AL_TRUE);
            alSourcef(acousticSource, AL_ROLLOFF_FACTOR, 0.0f);
            if(efxFilters) alSourcei(acousticSource, AL_DIRECT_FILTER, AL_FILTER_NULL);
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
            if(acousticSource)
            {
                alSourceStop(acousticSource);
                alSourcei(acousticSource, AL_BUFFER, 0);
                alDeleteSources(1, &acousticSource);
                acousticSource = 0;
            }
            if(acousticFilter)
            {
                if(alDeleteFilters_) alDeleteFilters_(1, &acousticFilter);
                acousticFilter = 0;
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
        ALuint acousticSource = chan.acousticSource;
        ALuint acousticFilter = chan.acousticFilter;
        chan.reset();
        chan.source = source;
        chan.filter = filter;
        chan.sendFilter = sendFilter;
        chan.distanceSendFilter = distanceSendFilter;
        chan.acousticSource = acousticSource;
        chan.acousticFilter = acousticFilter;
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
        if(chan.acousticSource)
        {
            alSourceStop(chan.acousticSource);
            alSourcei(chan.acousticSource, AL_BUFFER, 0);
        }
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
        if(chan.acousticSource)
        {
            alSourceStop(chan.acousticSource);
            alSourcei(chan.acousticSource, AL_BUFFER, 0);
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

    extern int soundacousticdualvoice;

    static bool acousticPropagatedSound(const SoundChannel &chan)
    {
        return chan.hasLoc() && !(chan.flags&SND_HUD) && (!chan.ent || (chan.flags&SND_MAP));
    }

    static bool acousticDualVoiceImportant(const SoundChannel &chan)
    {
        return soundacousticdualvoice && acousticPropagatedSound(chan);
    }

    static void syncAcousticVoice(SoundChannel &chan, int volume)
    {
        if(!chan.acousticSource) return;
        float targetGain = acoustics::soundacoustics && acousticDualVoiceImportant(chan) ? chan.targetAcousticGain : 0.0f;
        if(chan.acousticGain < 0.0f) chan.acousticGain = targetGain;
        float fade = curtime > 0 ? min(curtime/120.0f, 1.0f) : 1.0f;
        chan.acousticGain += (targetGain - chan.acousticGain)*fade;
        if(fabs(chan.acousticGain - targetGain) < 1e-3f) chan.acousticGain = targetGain;

        float gain = clamp(volume/float(MaxVolume)*chan.acousticGain, 0.0f, 1.0f),
              pan = clamp(chan.targetAcousticPan/127.5f - 1.0f, -1.0f, 1.0f);
        alSourcef(chan.acousticSource, AL_GAIN, gain);
        alSource3f(chan.acousticSource, AL_POSITION, pan, 0.0f, -1.0f);
        if(efxReverb && efxReverbSlot && gain > 0.001f && chan.targetReverbSend > 0.001f)
            alSource3i(chan.acousticSource, AL_AUXILIARY_SEND_FILTER, efxReverbSlot, 0, AL_FILTER_NULL);
        else if(efxReverb)
            alSource3i(chan.acousticSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        if(efxFilters && chan.targetAcousticGainHF < 0.999f)
        {
            if(chan.ensureFilter(chan.acousticFilter, AL_FILTER_LOWPASS))
            {
                alFilterf_(chan.acousticFilter, AL_LOWPASS_GAINHF, chan.targetAcousticGainHF);
                alSourcei(chan.acousticSource, AL_DIRECT_FILTER, chan.acousticFilter);
            }
            else alSourcei(chan.acousticSource, AL_DIRECT_FILTER, AL_FILTER_NULL);
        }
        else if(efxFilters) alSourcei(chan.acousticSource, AL_DIRECT_FILTER, AL_FILTER_NULL);
        chan.acousticGainHF = chan.targetAcousticGainHF;
        chan.acousticPan = chan.targetAcousticPan;
    }

    static void syncChannel(SoundChannel &chan)
    {
        if(!chan.source) return;
        int volume = effectiveVolume(chan);
        if(!chan.dirty && volume == chan.volume && fabs(chan.targetGainHF - chan.gainhf) < 1e-3f && fabs(chan.targetGainLF - chan.gainlf) < 1e-3f &&
           fabs(chan.targetReverbSend - chan.reverbSend) < 1e-3f && fabs(chan.targetDistanceReverbSend - chan.distanceReverbSend) < 1e-3f &&
           fabs(chan.targetAcousticGain - chan.acousticGain) < 1e-3f && fabs(chan.targetAcousticGainHF - chan.acousticGainHF) < 1e-3f &&
           chan.targetAcousticPan == chan.acousticPan) return;
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
        syncAcousticVoice(chan, volume);
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

    void stopMapSound(extentity *e, int fade = 0)
    {
        loopv(channels)
        {
            SoundChannel &chan = channels[i];
            if(chan.inuse && chan.ent == e)
            {
                if(fade > 0 && !chan.stopping)
                {
                    chan.fadeStart = totalmillis;
                    chan.fadeEnd = totalmillis + fade;
                    chan.fadeFrom = effectiveVolume(chan);
                    chan.targetVolume = 0;
                    chan.stopping = true;
                    chan.dirty = true;
                    syncChannel(chan);
                }
                else if(fade <= 0) haltChannel(i);
            }
        }
    }

    static float metersToUnits(float meters);
    static float mapSoundFadeDistance(float radius);
    static int mapSoundFadeMillis(float radius);
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

            float fadedist = mapSoundFadeDistance(maxdist);
            bool inrange = maxdist <= 0.0f || dist < maxdist + fadedist;

            if(inrange)
            {
                play(e.attr1, NULL, &e, SND_MAP, -1, mapSoundFadeMillis(maxdist), -1, 0, -1);
            }
            else if(e.flags&EF_SOUND) stopMapSound(&e, mapSoundFadeMillis(maxdist));
        }
    }

    VAR(stereo, 0, 1, 1);
    VAR(maxsoundradius, 1, 340, 0);
    VARP(soundfollowentities, 0, 1, 1);
    VARP(soundpitchrandom, 0, 0, 1);
    FVAR(soundpitchrandomamount, 0.0f, 0.03f, 1.0f);
    VARP(soundacousticdualvoice, 0, 1, 1);
    VARP(soundacousticlooprefresh, 0, 100, 5000);
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

    static uint soundEventSerial = 0;

    static uint mixSoundSeed(uint seed, uint value)
    {
        seed ^= value + 0x9E3779B9U + (seed << 6) + (seed >> 2);
        return seed ? seed : 0xA511E9B3U;
    }

    static uint quantizeSoundCoord(float v)
    {
        return uint(int(v*16.0f + (v >= 0 ? 0.5f : -0.5f)));
    }

    static uint soundEventSeed(int n, const vec *loc, const extentity *ent, int flags, int soundentity)
    {
        uint seed = mixSoundSeed(0x6D2B79F5U, uint(n));
        seed = mixSoundSeed(seed, uint(flags));
        seed = mixSoundSeed(seed, uint(totalmillis));
        seed = mixSoundSeed(seed, ++soundEventSerial);
        seed = mixSoundSeed(seed, uint(soundentity));
        if(ent) seed = mixSoundSeed(seed, uint(size_t(ent)));
        if(loc)
        {
            seed = mixSoundSeed(seed, quantizeSoundCoord(loc->x));
            seed = mixSoundSeed(seed, quantizeSoundCoord(loc->y));
            seed = mixSoundSeed(seed, quantizeSoundCoord(loc->z));
        }
        return seed;
    }

    static float randomPitchOffset(const vec *loc, int flags, const extentity *ent, uint seed)
    {
        if(!soundpitchrandom || soundpitchrandomamount <= 0.0f || ent || (flags&SND_MAP) || (!loc && !(flags&SND_HUD))) return 1.0f;
        float amount = min(soundpitchrandomamount, 0.99f);
        float r = float(detrnd(seed, 0x10000))/float(0xFFFF);
        return clamp(1.0f + r*(2.0f*amount) - amount, 0.01f, 4.0f);
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

    static float mapSoundFadeDistance(float radius)
    {
        return radius > 0.0f ? min(max(radius*0.25f, 8.0f), 128.0f) : 0.0f;
    }

    static int mapSoundFadeMillis(float radius)
    {
        if(radius <= 0.0f) return 350;
        return clamp(int(300.0f + sqrtf(radius)*20.0f + 0.5f), 250, 900);
    }

    static float mapSoundRadiusGain(float dist, float radius, float inner)
    {
        if(radius <= 0.0f) return 1.0f;
        inner = clamp(inner, 0.0f, max(radius - 1.0f, 0.0f));
        if(dist <= inner) return 1.0f;

        float edge = mapSoundFadeDistance(radius),
              audible = radius + edge;
        if(dist >= audible) return 0.0f;

        float edgegain = 0.18f,
              body = 1.0f - (1.0f - edgegain)*smoothramp(dist, inner, radius);
        if(dist <= radius) return body;

        return edgegain*(1.0f - smoothramp(dist, radius, audible));
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


    static float currentReverbGain = -1.0f, currentReverbDecay = -1.0f, currentReverbReflection = -1.0f, currentReverbDensity = -1.0f, currentReverbGainHF = -1.0f, currentReverbLateGain = -1.0f;

    void updateAcousticReverb(const EFXEAXREVERBPROPERTIES *acousticShape, float reverbGain, float reverbDecay, float reflectionAmount)
    {
        if(!efxReverb || !efxReverbEffect || !efxReverbSlot) return;
        bool usebaked = acousticShape != NULL;
        EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;
        const EFXEAXREVERBPROPERTIES &shape = usebaked ? *acousticShape : generic;
        float gain = usebaked ? clamp(shape.flGain*reverbGain, 0.0f, 1.0f) : clamp(0.65f*sounddistancereverb, 0.0f, 1.0f),
              decay = clamp(usebaked ? reverbDecay : shape.flDecayTime, 0.12f, 4.0f),
              reflection = clamp(shape.flReflectionsGain*(usebaked ? 0.35f + reflectionAmount*0.65f : 1.0f), 0.0f, 3.16f),
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

    static bool soundInRange(const vec &loc)
    {
        if(soundairattenuation)
        {
            if(maxsounddistance <= 0) return true;
            return camera1->o.dist(loc) <= metersToUnits(maxsounddistance);
        }
        return true;
    }

    static bool acousticLoopCacheMoved(const SoundChannel &chan)
    {
        static const float moveThreshold = 1.0f;
        return !chan.acousticCacheValid ||
               chan.loc.squaredist(chan.acousticCacheLoc) > moveThreshold*moveThreshold ||
               camera1->o.squaredist(chan.acousticCacheListener) > moveThreshold*moveThreshold;
    }

    static void acousticSourceForChannel(SoundChannel &chan, float dist, float &volf, float &gainhf, float &reverbSend, acoustics::AcousticSourceInfo &info)
    {
        if(!chan.looping)
        {
            acoustics::acousticSource(chan.loc, dist, volf, gainhf, reverbSend, &info);
            return;
        }

        bool moved = acousticLoopCacheMoved(chan),
             refresh = soundacousticlooprefresh <= 0 || chan.acousticCacheMillis < 0 || totalmillis - chan.acousticCacheMillis >= soundacousticlooprefresh;
        if(!chan.acousticCacheValid || (moved && refresh))
        {
            float cacheVol = 1.0f, cacheGainHF = 1.0f, cacheReverb = 0.0f;
            acoustics::AcousticSourceInfo cacheInfo;
            acoustics::acousticSource(chan.loc, dist, cacheVol, cacheGainHF, cacheReverb, &cacheInfo);
            chan.acousticCacheLoc = chan.loc;
            chan.acousticCacheListener = camera1->o;
            chan.acousticCacheInfo = cacheInfo;
            chan.acousticCacheMillis = totalmillis;
            chan.acousticCacheVol = cacheVol;
            chan.acousticCacheGainHF = cacheGainHF;
            chan.acousticCacheReverb = cacheReverb;
            chan.acousticCacheValid = true;
        }

        if(!chan.acousticCacheValid) return;
        volf *= chan.acousticCacheVol;
        gainhf *= chan.acousticCacheGainHF;
        reverbSend = max(reverbSend, chan.acousticCacheReverb);
        info = chan.acousticCacheInfo;
    }

    static bool updateChannel(SoundChannel &chan)
    {
        if(!chan.slot) return false;
        float volf = 1.0f, panf = 0.5f, gainhf = 1.0f, gainlf = 1.0f, reverbSend = 0.0f, distanceReverbSend = 0.0f;
        float acousticGain = 0.0f, acousticGainHF = 1.0f;
        int acousticPan = 128;
        updateSoundEntity(chan);
        if(chan.hasLoc())
        {
            vec v;
            float dist = chan.loc.dist(camera1->o, v), attenDist = dist;
            int rad = 0;
            float inner = 0.0f;
            if(chan.ent)
            {
                rad = chan.ent->attr2;
                if(chan.ent->attr3)
                {
                    inner = max(float(chan.ent->attr3), 0.0f);
                    attenDist = max(dist - inner, 0.0f);
                }
            }
            else if(chan.radius > 0) rad = chan.radius;
            if(soundairattenuation)
            {
                float distanceEffect = soundDistanceEffect(attenDist),
                      distanceReverb = soundDistanceReverbSend(distanceEffect);
                volf *= soundDistanceGain(attenDist);
                gainhf = soundMuffleGainHF(attenDist);
                gainlf = soundDistanceBassGain(distanceEffect);
                reverbSend = max(reverbSend, distanceReverb);
                distanceReverbSend = distanceReverb;
            }
            bool mapSoundInRadius = !(chan.flags&SND_MAP) || rad <= 0 || dist <= rad;
            if((chan.flags&SND_MAP) && rad > 0) volf *= mapSoundRadiusGain(dist, float(rad), inner);
            else if(!soundairattenuation && rad > 0) volf -= clamp(attenDist/rad, 0.0f, 1.0f);
            acoustics::AcousticSourceInfo acousticInfo;
            if(acoustics::soundacoustics && acousticPropagatedSound(chan) && mapSoundInRadius) acousticSourceForChannel(chan, dist, volf, gainhf, reverbSend, acousticInfo);
            if(acousticInfo.path)
            {
                acousticGain = acousticInfo.virtualGain;
                acousticGainHF = acousticInfo.virtualGainHF;
            }
            if(stereo && (v.x != 0 || v.y != 0) && dist>0)
            {
                v.rotate_around_z(-camera1->yaw*RAD);
                panf = 0.5f - 0.5f*v.x/v.magnitude2();
                if(acousticInfo.path)
                {
                    vec av;
                    float adist = acousticInfo.apparent.dist(camera1->o, av);
                    if(adist > 0 && (av.x != 0 || av.y != 0))
                    {
                        av.rotate_around_z(-camera1->yaw*RAD);
                        acousticPan = clamp(int((0.5f - 0.5f*av.x/av.magnitude2())*255.9f), 0, 255);
                        if(!acousticDualVoiceImportant(chan)) panf = acousticPan/255.9f;
                    }
                }
            }
        }
        else if(chan.flags&SND_HUD) acoustics::acousticHudSource(reverbSend);
        if(!efxReverb) reverbSend = distanceReverbSend = 0.0f;
        else if(!efxDistanceReverbSlot) reverbSend = max(reverbSend, distanceReverbSend);
        int vol = clamp(int(volf*soundvol*chan.slot->volume*(MaxVolume/float(255*255)) + 0.5f), 0, MaxVolume);
        int pan = clamp(int(panf*255.9f), 0, 255);
        if(vol == chan.targetVolume && pan == chan.pan && fabs(gainhf - chan.targetGainHF) < 1e-3f && fabs(gainlf - chan.targetGainLF) < 1e-3f &&
           fabs(reverbSend - chan.targetReverbSend) < 1e-3f && fabs(distanceReverbSend - chan.targetDistanceReverbSend) < 1e-3f &&
           fabs(acousticGain - chan.targetAcousticGain) < 1e-3f && fabs(acousticGainHF - chan.targetAcousticGainHF) < 1e-3f &&
           acousticPan == chan.targetAcousticPan) return false;
        chan.targetVolume = vol;
        chan.pan = pan;
        chan.targetGainHF = gainhf;
        chan.targetGainLF = gainlf;
        chan.targetReverbSend = reverbSend;
        chan.targetDistanceReverbSend = distanceReverbSend;
        chan.targetAcousticGain = acousticGain;
        chan.targetAcousticGainHF = acousticGainHF;
        chan.targetAcousticPan = acousticPan;
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
            acoustics::updateAcoustics();
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
                chan.looping = loops != 0;
                chan.acousticCacheValid = false;
                return chanid;
            }
        }
        if(fade < 0) return -1;

        int soundentity = soundEntityId(loc, ent, flags);
        uint eventseed = soundEventSeed(n, loc, ent, flags, soundentity);
        SoundSlot &slot = sounds.slots[config.chooseSlot(flags, eventseed)];
        if(!slot.sample->buffer && !slot.sample->load(sounds.dir)) return -1;

        if(dbgsound) conoutf(CON_DEBUG, "sound: %s%s", sounds.dir, slot.sample->name);

        chanid = -1;
        loopv(channels) if(!channels[i].inuse) { chanid = i; break; }
        if(chanid < 0 && channels.length() < maxChannels) chanid = channels.length();
        if(chanid < 0) loopv(channels) if(!channels[i].targetVolume) { haltChannel(i); chanid = i; break; }
        if(chanid < 0) return -1;

        SoundChannel &chan = newChannel(chanid, &slot, loc, ent, flags, radius, soundentity);
        chan.eventseed = eventseed;
        chan.pitch = randomPitchOffset(loc, flags, ent, eventseed);
        chan.looping = loops != 0;
        if(!chan.ensureSource())
        {
            freeChannel(chanid);
            return -1;
        }
        bool useAcousticVoice = acousticDualVoiceImportant(chan) && chan.ensureAcousticSource();

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
        if(useAcousticVoice && chan.acousticSource)
        {
            alSourceStop(chan.acousticSource);
            alSourcei(chan.acousticSource, AL_BUFFER, 0);
            alSourcei(chan.acousticSource, AL_BUFFER, slot.sample->buffer);
            alSourcei(chan.acousticSource, AL_LOOPING, loops ? AL_TRUE : AL_FALSE);
            alSourcef(chan.acousticSource, AL_PITCH, chan.pitch);
            alSourcef(chan.acousticSource, AL_GAIN, 0.0f);
        }
        else if(chan.acousticSource)
        {
            alSourceStop(chan.acousticSource);
            alSourcei(chan.acousticSource, AL_BUFFER, 0);
        }
        syncChannel(chan);
        alSourcePlay(chan.source);
        if(useAcousticVoice && chan.acousticSource) alSourcePlay(chan.acousticSource);
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
ICOMMAND(soundacousticcells, "", (), intret(acoustics::numAcousticCells()));
ICOMMAND(soundacousticregions, "", (), intret(acoustics::numAcousticRegions()));
ICOMMAND(soundacousticportals, "", (), intret(acoustics::numAcousticPortals()));

void bakesoundacoustics(int *cellsize, int *rays) { acoustics::bakeAcousticGrid(*cellsize, *rays); }
COMMAND(bakesoundacoustics, "ii");

void soundacousticbakecorner(int *corner, float *x, float *y, float *z)
{
    if(*corner < 1 || *corner > 2)
    {
        conoutf(CON_WARN, "soundacousticbakecorner: corner must be 1 or 2");
        return;
    }
    acoustics::setAcousticBakeCorner(*corner - 1, vec(*x, *y, *z));
}
COMMAND(soundacousticbakecorner, "ifff");

void getacousticbounds(int *corner)
{
    if(*corner < 1 || *corner > 2)
    {
        conoutf(CON_WARN, "getacousticbounds: corner must be 1 or 2");
        return;
    }
    if(!camera1)
    {
        conoutf(CON_WARN, "getacousticbounds: no camera");
        return;
    }
    acoustics::setAcousticBakeCorner(*corner - 1, camera1->o);
}
COMMAND(getacousticbounds, "i");

void clearsoundacousticgrid() { acoustics::clearAcousticGrid(); }
COMMAND(clearsoundacousticgrid, "");

void soundreset() { sound::soundReset(); }
COMMAND(soundreset, "");

void mapsoundreset() { sound::mapSoundReset(); }
COMMAND(mapsoundreset, "");

void clear_sound() { sound::cleanup(); }
void clearmapsounds() { sound::clearMapSounds(); }
void checkmapsounds() { sound::checkMapSounds(); }
void updatesounds() { sound::update(); }
void rendersounddebug() { acoustics::drawAcousticsDebug(); }
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
