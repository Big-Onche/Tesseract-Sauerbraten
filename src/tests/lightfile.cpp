// g++ -DSTANDALONE -I src/shared -I src/enet/include -I src/include src/tests/lightfile.cpp -o lightfile-test.exe
#include "cube.h"
#include "../engine/lightfile.h"

static int failures = 0;
#define CHECK(condition) do { if(!(condition)) { printf("line %d: %s\n", __LINE__, #condition); ++failures; } } while(0)

static bool parse(const char *text, lightfile::parser &parser)
{
    while(*text)
    {
        const char *end = strchr(text, '\n');
        size_t length = end ? size_t(end-text) : strlen(text);
        if(length >= 4096) return false;
        char line[4096];
        memcpy(line, text, length);
        line[length] = 0;
        if(!parser.line(line)) return false;
        text += length + (end ? 1 : 0);
    }
    return parser.finish();
}

int main()
{
    using namespace lightfile;
    parser legacy;
    CHECK(parse("# legacy\n1.25 2.5 3.75 200 255 128 64 17\n4 5 6 -1 -2 300 1 -32768\n", legacy));
    CHECK(legacy.records.length() == 2);
    CHECK(legacy.records[0].values[POSITION] == 1.25f && legacy.records[0].values[RANGE] == 200);
    CHECK(legacy.records[0].values[SHAPE] == 0 && legacy.records[0].values[PCSS_ENABLED] == -1);
    vector<char> output;
    serialize(legacy.records, output);
    CHECK(!strncmp(output.getbuf(), "sauerlights 1", 13));
    parser upgraded;
    CHECK(parse(output.getbuf(), upgraded));
    CHECK(upgraded.records.length() == legacy.records.length());
    CHECK(!memcmp(upgraded.records[1].values, legacy.records[1].values, sizeof(record::values)));

    parser defaults;
    CHECK(parse("\xEF\xBB\xBFsauerlights 1\r\n[light]\nfuture.material = blue noise [1 2]\nposition = 1 2 3 // comment\n", defaults));
    CHECK(defaults.records[0].values[RANGE] == 128 && defaults.records[0].values[COLOR] == 255);
    CHECK(defaults.records[0].values[WIDTH] == 0 && defaults.records[0].values[PCSS_DISTANCE] == -1);
    parser empty;
    CHECK(parse("sauerlights 1\n", empty) && empty.records.empty());

    for(int shape = 0; shape < 5; ++shape)
    {
        char source[1024];
        snprintf(source, sizeof(source),
                 "sauerlights 1\n[metadata]\nfuture stuff\n[light]\nshape = %s\nposition = 1.23456789 2 3\n"
                 "source_radius = 2.75\nwidth = 42.5\nheight = 17.25\norientation = 90 -45 12\n"
                 "pcss.enabled = 1\npcss.quality = 2\npcss.blockers = 8\npcss.samples = 32\npcss.max_penumbra = 24\n"
                 "pcss.distance = 768\npcss.min_pixels = 12\nshadow.enabled = 0\n", shapes[shape]);
        parser first, second;
        CHECK(parse(source, first));
        serialize(first.records, output);
        CHECK(parse(output.getbuf(), second));
        CHECK(second.records.length() == 1);
        CHECK(!memcmp(first.records[0].values, second.records[0].values, sizeof(record::values)));
        CHECK(second.records[0].values[SHAPE] == shape && second.records[0].values[PCSS_SAMPLES] == 32);
    }
    parser aliases;
    CHECK(parse("sauerlights 1\n[light]\nposition=0 0 0\nshape=line\nradius=64\nyaw=20\nwidth=4\nwidth=8", aliases));
    CHECK(aliases.records[0].values[SHAPE] == 3 && aliases.records[0].values[WIDTH] == 8);

    const char *invalid[] =
    {
        "", "# comment only", "sauerlights 2", "1 2 3 4 5 6 7", "1 2 3 4 5 6 7 8 9", "1 2 3 4.5 5 6 7 8",
        "sauerlights 1\n[light]\ncolor=1 2 3", "sauerlights 1\n[light]\nposition=nan 0 0",
        "sauerlights 1\n[light]\nposition=-nan 0 0", "sauerlights 1\n[light]\nposition=1e999 0 0",
        "sauerlights 1\n[light]\nposition=0 0 0\npcss.samples=0", "sauerlights 1\n[light]\nposition=0 0 0\npcss.samples=65",
        "sauerlights 1\n[light]\nposition=0 0 0\npcss.distance=-0.5", "sauerlights 1\n[light]\nposition=0 0 0\nshape=triangle",
        "sauerlights 1\n[light]\nposition=0 0 0\nwidth=4 junk", "sauerlights 1\n[light]\nposition=0 0 0\nwidth=-1"
    };
    loopi(sizeof(invalid)/sizeof(invalid[0]))
    {
        parser bad;
        CHECK(!parse(invalid[i], bad));
    }
    parser limit;
    CHECK(limit.line("sauerlights 1"));
    loopi(10000) { CHECK(limit.line("[light]")); CHECK(limit.line("position=0 0 0")); }
    CHECK(limit.line("[light]"));
    CHECK(limit.line("position=0 0 0"));
    CHECK(!limit.finish());
    printf("lightfile: %s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
