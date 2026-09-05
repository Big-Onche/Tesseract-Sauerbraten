#ifndef SAUERRACT_LIGHTFILE_H
#define SAUERRACT_LIGHTFILE_H

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>

// Data-only sidecar codec: no CubeScript execution and no persistent entity layout changes.
namespace lightfile
{
    enum
    {
        POSITION = 0, COLOR = 3, RANGE = 6, FLAGS, SHAPE, SOURCE_RADIUS, WIDTH, HEIGHT, ORIENTATION,
        PCSS_ENABLED = 15, PCSS_QUALITY, PCSS_BLOCKERS, PCSS_SAMPLES, PCSS_PENUMBRA, PCSS_DISTANCE, PCSS_MINPIXELS,
        SHADOW_ENABLED, NUMVALUES
    };

    struct record
    {
        float values[NUMVALUES];
        bool positioned;

        record() : positioned(false)
        {
            memset(values, 0, sizeof(values));
            values[COLOR] = values[COLOR+1] = values[COLOR+2] = 255;
            values[RANGE] = 128;
            for(int i = PCSS_ENABLED; i < NUMVALUES; ++i) values[i] = -1;
        }
    };

    struct property
    {
        const char *name;
        int index, count;
        float minimum, maximum;
        bool integral, canonical;
    };

    // Add optional numeric properties here; the record parser and writer remain unchanged.
    static const property properties[] =
    {
        { "position", POSITION, 3, -1e8f, 1e8f, false, true },
        { "color", COLOR, 3, -32768, 32767, true, true },
        { "range", RANGE, 1, -32768, 32767, true, true },
        { "radius", RANGE, 1, -32768, 32767, true, false },
        { "flags", FLAGS, 1, -32768, 32767, true, true },
        { "shape", SHAPE, 1, 0, 4, true, true },
        { "source_radius", SOURCE_RADIUS, 1, 0, 4096, false, true },
        { "width", WIDTH, 1, 0, 8192, false, true },
        { "height", HEIGHT, 1, 0, 8192, false, true },
        { "orientation", ORIENTATION, 3, -360, 360, false, true },
        { "yaw", ORIENTATION, 1, -360, 360, false, false },
        { "pitch", ORIENTATION+1, 1, -360, 360, false, false },
        { "roll", ORIENTATION+2, 1, -360, 360, false, false },
        { "pcss.enabled", PCSS_ENABLED, 1, -1, 1, true, true },
        { "pcss.quality", PCSS_QUALITY, 1, -1, 2, true, true },
        { "pcss.blockers", PCSS_BLOCKERS, 1, -1, 32, true, true },
        { "pcss.samples", PCSS_SAMPLES, 1, -1, 64, true, true },
        { "pcss.max_penumbra", PCSS_PENUMBRA, 1, -1, 128, false, true },
        { "pcss.distance", PCSS_DISTANCE, 1, -1, 16384, false, true },
        { "pcss.min_pixels", PCSS_MINPIXELS, 1, -1, 1024, false, true },
        { "shadow.enabled", SHADOW_ENABLED, 1, -1, 1, true, true }
    };
    static const char *const shapes[] = { "point", "sphere", "disk", "capsule", "rectangle" };

    inline char *trim(char *text)
    {
        while(std::isspace(static_cast<unsigned char>(*text))) ++text;
        char *end = text + strlen(text);
        while(end > text && std::isspace(static_cast<unsigned char>(end[-1]))) --end;
        *end = 0;
        return text;
    }

    inline bool numbers(const char *text, float *values, int count, double minimum, double maximum, bool integral)
    {
        const char *cursor = text;
        for(int i = 0; i < count; ++i)
        {
            char *end;
            double value = std::strtod(cursor, &end);
            unsigned long long bits = 0;
            memcpy(&bits, &value, sizeof(value));
            if((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) return false;
            if(end == cursor || !(value >= minimum && value <= maximum) || (integral && std::floor(value) != value)) return false;
            if(*end && !std::isspace(static_cast<unsigned char>(*end))) return false;
            values[i] = static_cast<float>(value);
            cursor = end;
        }
        while(std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
        return !*cursor;
    }

    class parser
    {
        int mode;
        bool inlight, ignoredsection;
        record current;

        bool finishrecord()
        {
            if(!inlight) return true;
            if(!current.positioned || records.length() >= 10000) return false;
            records.add(current);
            inlight = false;
            return true;
        }

    public:
        vector<record> records;

        parser() : mode(0), inlight(false), ignoredsection(false) {}

        bool line(const char *input)
        {
            if(strlen(input) > 4095) return false;
            char buffer[4096];
            strcpy(buffer, input);
            char *text = buffer;
            if(strncmp(text, "\xEF\xBB\xBF", 3) == 0) text += 3;
            char *comment = strpbrk(text, "#;"), *slash = strstr(text, "//");
            if(slash && (!comment || slash < comment)) comment = slash;
            if(comment) *comment = 0;
            text = trim(text);
            if(!*text) return true;
            if(!mode)
            {
                if(!strcmp(text, "sauerlights 1")) { mode = 1; return true; }
                if(!strncmp(text, "sauerlights", 10)) return false;
                mode = 2;
            }
            if(mode == 2)
            {
                float old[8];
                if(!numbers(text, old, 8, -1e8, 1e8, false) || records.length() >= 10000) return false;
                record r;
                for(int i = 0; i < 3; ++i) r.values[POSITION+i] = old[i];
                for(int i = 3; i < 8; ++i)
                {
                    if(old[i] < -32768 || old[i] > 32767 || std::floor(old[i]) != old[i]) return false;
                }
                r.values[RANGE] = old[3];
                for(int i = 0; i < 3; ++i) r.values[COLOR+i] = old[4+i];
                r.values[FLAGS] = old[7];
                r.positioned = true;
                records.add(r);
                return true;
            }
            if(text[0] == '[')
            {
                if(text[strlen(text)-1] != ']' || !finishrecord()) return false;
                inlight = !strcmp(text, "[light]");
                ignoredsection = !inlight;
                current = record();
                return true;
            }
            if(ignoredsection) return true;
            if(!inlight) return false;
            char *equals = strchr(text, '=');
            if(!equals) return false;
            *equals = 0;
            char *key = trim(text), *value = trim(equals+1);
            for(size_t i = 0; i < sizeof(properties)/sizeof(properties[0]); ++i)
            {
                const property &p = properties[i];
                if(strcmp(key, p.name)) continue;
                if(p.index == SHAPE)
                {
                    if(!strcmp(value, "line")) { current.values[SHAPE] = 3; return true; }
                    for(int j = 0; j < 5; ++j) if(!strcmp(value, shapes[j])) { current.values[SHAPE] = float(j); return true; }
                    return false;
                }
                if(!numbers(value, &current.values[p.index], p.count, p.minimum, p.maximum, p.integral)) return false;
                if(p.minimum == -1 && current.values[p.index] < 0 && current.values[p.index] != -1) return false;
                if((p.index == PCSS_BLOCKERS || p.index == PCSS_SAMPLES) && current.values[p.index] == 0) return false;
                if(p.index == POSITION) current.positioned = true;
                return true;
            }
            return true; // Unknown optional fields are intentionally ignored.
        }

        bool finish()
        {
            return mode != 0 && finishrecord();
        }
    };

    inline void append(vector<char> &text, const char *value)
    {
        text.put(value, strlen(value));
    }

    inline void serialize(const vector<record> &records, vector<char> &text)
    {
        text.setsize(0);
        append(text, "sauerlights 1\n# PCSS values of -1 inherit the renderer settings.\n");
        for(int i = 0; i < records.length(); ++i)
        {
            append(text, "\n[light]\n");
            for(size_t j = 0; j < sizeof(properties)/sizeof(properties[0]); ++j)
            {
                const property &p = properties[j];
                if(!p.canonical) continue;
                append(text, p.name);
                append(text, " =");
                for(int k = 0; k < p.count; ++k)
                {
                    char value[64];
                    if(p.index == SHAPE) std::snprintf(value, sizeof(value), " %s", shapes[int(records[i].values[SHAPE])]);
                    else std::snprintf(value, sizeof(value), " %.9g", records[i].values[p.index+k]);
                    append(text, value);
                }
                append(text, "\n");
            }
        }
        text.add(0);
    }
}
#endif
