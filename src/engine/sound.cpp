// sound.cpp: basic positional sound using OpenAL Soft and libsndfile

#include "engine.h"
#include "AL/al.h"
#include "AL/alc.h"
#include "sndfile.h"

namespace sound
{
    static const int MaxVolume = 128;
    static const int MusicBuffers = 4;
    static const int MusicBufferFrames = 32768;

    bool nosound = true;

    static ALCdevice *alDevice = NULL;
    static ALCcontext *alContext = NULL;
    static int maxChannels = 0;

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
        ALuint source;
        bool inuse;
        vec loc;
        SoundSlot *slot;
        extentity *ent;
        int radius, volume, targetVolume, pan, flags, expire;
        int fadeStart, fadeEnd, fadeFrom;
        bool dirty, stopping;

        SoundChannel(int id) : id(id), source(0) { reset(); }
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
            fadeStart = fadeEnd = fadeFrom = 0;
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
            reset();
        }
    };

    static vector<SoundChannel> channels;

    static SoundChannel &newChannel(int n, SoundSlot *slot, const vec *loc = NULL, extentity *ent = NULL, int flags = 0, int radius = 0)
    {
        if(ent)
        {
            loc = &ent->o;
            ent->flags |= EF_SOUND;
        }
        while(!channels.inrange(n)) channels.add(channels.length());
        SoundChannel &chan = channels[n];
        ALuint source = chan.source;
        chan.reset();
        chan.source = source;
        chan.inuse = true;
        if(loc) chan.loc = *loc;
        chan.slot = slot;
        chan.ent = ent;
        chan.flags = flags;
        chan.radius = radius;
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

    static void syncChannel(SoundChannel &chan)
    {
        if(!chan.source || (!chan.dirty && effectiveVolume(chan) == chan.volume)) return;
        chan.volume = effectiveVolume(chan);
        alSourcef(chan.source, AL_GAIN, clamp(chan.volume/float(MaxVolume), 0.0f, 1.0f));
        float pan = clamp(chan.pan/127.5f - 1.0f, -1.0f, 1.0f);
        alSource3f(chan.source, AL_POSITION, pan, 0.0f, -1.0f);
        chan.dirty = false;
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
            return false;
        }

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
                alcMakeContextCurrent(NULL);
                if(alContext) { alcDestroyContext(alContext); alContext = NULL; }
                if(alDevice) { alcCloseDevice(alDevice); alDevice = NULL; }
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
            alcMakeContextCurrent(NULL);
            if(alContext) { alcDestroyContext(alContext); alContext = NULL; }
            if(alDevice) { alcCloseDevice(alDevice); alDevice = NULL; }
        }
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

    void checkMapSounds()
    {
        const vector<extentity *> &ents = entities::getents();
        loopv(ents)
        {
            extentity &e = *ents[i];
            if(e.type!=ET_SOUND) continue;
            if(camera1->o.dist(e.o) < e.attr2)
            {
                if(!(e.flags&EF_SOUND)) play(e.attr1, NULL, &e, SND_MAP, -1);
            }
            else if(e.flags&EF_SOUND) stopMapSound(&e);
        }
    }

    VAR(stereo, 0, 1, 1);
    VAR(maxsoundradius, 1, 340, 0);

    static bool updateChannel(SoundChannel &chan)
    {
        if(!chan.slot) return false;
        float volf = 1.0f, panf = 0.5f;
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
            if(rad > 0) volf -= clamp(dist/rad, 0.0f, 1.0f);
            if(stereo && (v.x != 0 || v.y != 0) && dist>0)
            {
                v.rotate_around_z(-camera1->yaw*RAD);
                panf = 0.5f - 0.5f*v.x/v.magnitude2();
            }
        }
        int vol = clamp(int(volf*soundvol*chan.slot->volume*(MaxVolume/float(255*255)) + 0.5f), 0, MaxVolume);
        int pan = clamp(int(panf*255.9f), 0, 255);
        if(vol == chan.targetVolume && pan == chan.pan) return false;
        chan.targetVolume = vol;
        chan.pan = pan;
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
            if(chan.hasLoc()) updateChannel(chan);
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
            int maxrad = game::maxsoundradius(n);
            if(radius <= 0 || maxrad < radius) radius = maxrad;
            if(camera1->o.dist(*loc) > 1.5f*radius)
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
                if(loc) chan.loc = *loc;
                else if(chan.hasLoc()) chan.clearLoc();
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

        SoundChannel &chan = newChannel(chanid, &slot, loc, ent, flags, radius);
        if(!chan.ensureSource())
        {
            freeChannel(chanid);
            return -1;
        }

        updateChannel(chan);
        chan.expire = expire >= 0 ? totalmillis + expire : -1;
        chan.fadeStart = totalmillis;
        chan.fadeEnd = fade > 0 ? totalmillis + fade : totalmillis;
        chan.fadeFrom = fade > 0 ? 0 : chan.targetVolume;
        chan.volume = -1;
        chan.dirty = true;

        alSourceStop(chan.source);
        alSourcei(chan.source, AL_BUFFER, 0);
        alSourcei(chan.source, AL_BUFFER, slot.sample->buffer);
        alSourcei(chan.source, AL_LOOPING, loops ? AL_TRUE : AL_FALSE);
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

void soundreset() { sound::soundReset(); }
COMMAND(soundreset, "");

void mapsoundreset() { sound::mapSoundReset(); }
COMMAND(mapsoundreset, "");

void clear_sound() { sound::cleanup(); }
void clearmapsounds() { sound::clearMapSounds(); }
void checkmapsounds() { sound::checkMapSounds(); }
void updatesounds() { sound::update(); }
void preloadsound(int n) { sound::preload(n); }
void preloadmapsound(int n) { sound::preloadMap(n); }
void preloadmapsounds() { sound::preloadMapSounds(); }
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
