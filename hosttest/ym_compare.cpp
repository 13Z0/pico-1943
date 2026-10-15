/*
 * Renders a log of YM2203 writes (c1943_host --ymlog) through ymfm, Aaron
 * Giles' emulation of the Yamaha chips that MAME uses, into a WAV with MAME's
 * 1943 mix: per chip the FM at 0.10 and each SSG channel at 0.15. Compare it
 * with c1943_host's own WAV of the same run (hosttest/wavcompare.py) to check
 * ym2203.c and ym_ssg.c.
 *
 *   ./hosttest/ym_compare <ymlog> <out.wav> [fm|ssg]
 *
 * With fm or ssg only that half of the chips is heard.
 *
 * ymfm runs at its highest fidelity (clock / 4 = 375 kHz) and is brought to
 * 44.1 kHz by averaging, the SSG's DC removed as c1943 does. Built by
 * hosttest/build.sh when ymfm's sources are found (YMFM_DIR).
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ymfm_opn.h"

#define SAMPLERATE 44100
#define CLOCK 1500000

class intf : public ymfm::ymfm_interface
{
};

struct ev
{
    uint32_t sample;
    int chip, a0, value;
};

static void put16(FILE *f, uint16_t v)
{
    fputc(v & 0xff, f);
    fputc(v >> 8, f);
}

static void put32(FILE *f, uint32_t v)
{
    put16(f, v & 0xffff);
    put16(f, v >> 16);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4)
    {
        fprintf(stderr, "usage: %s <ymlog> <out.wav> [fm|ssg]\n", argv[0]);
        return 1;
    }
    const double fm_gain = (argc == 4 && !strcmp(argv[3], "ssg")) ? 0 : 0.10;
    const double ssg_gain = (argc == 4 && !strcmp(argv[3], "fm")) ? 0 : 0.15;
    FILE *in = fopen(argv[1], "r");
    if (!in)
    {
        perror(argv[1]);
        return 1;
    }
    std::vector<ev> evs;
    ev e;
    while (fscanf(in, "%u %d %d %d", &e.sample, &e.chip, &e.a0, &e.value) == 4)
        evs.push_back(e);
    fclose(in);
    if (evs.empty())
        return 1;
    const uint32_t total = evs.back().sample + SAMPLERATE; /* a second after the last write */

    intf i0, i1;
    ymfm::ym2203 chips[2] = {ymfm::ym2203(i0), ymfm::ym2203(i1)};
    for (auto &c : chips)
    {
        c.set_fidelity(ymfm::OPN_FIDELITY_MAX);
        c.reset();
    }
    const double rate = chips[0].sample_rate(CLOCK);
    printf("ymfm at %.0f Hz, %zu writes, %u samples\n", rate, evs.size(), total);

    std::vector<int16_t> out(total);
    size_t next = 0;
    double pos = 0; /* ymfm samples made */
    int32_t dc_x = 0, dc_y = 0;
    for (uint32_t n = 0; n < total; n++)
    {
        while (next < evs.size() && evs[next].sample <= n)
        {
            chips[evs[next].chip].write(evs[next].a0, (uint8_t)evs[next].value);
            next++;
        }
        /* the ymfm samples of this output sample, averaged */
        const double upto = (n + 1) * rate / SAMPLERATE;
        double fm = 0, ssg = 0;
        int cnt = 0;
        while (pos < upto)
        {
            for (auto &c : chips)
            {
                ymfm::ym2203::output_data o;
                c.generate(&o, 1);
                fm += o.data[0];
                ssg += o.data[1] + o.data[2] + o.data[3];
            }
            cnt++;
            pos += 1;
        }
        if (cnt)
        {
            fm /= cnt;
            ssg /= cnt;
        }
        /* the SSG's DC removed as in ym_ssg.c */
        const int32_t x = (int32_t)(ssg * ssg_gain);
        dc_y += ((x - dc_x) << 8) - (dc_y >> 8);
        dc_x = x;
        double v = fm * fm_gain + (dc_y >> 8);
        if (v > 32767)
            v = 32767;
        else if (v < -32768)
            v = -32768;
        out[n] = (int16_t)lrint(v);
    }

    FILE *f = fopen(argv[2], "wb");
    if (!f)
    {
        perror(argv[2]);
        return 1;
    }
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + total * 2);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32(f, SAMPLERATE);
    put32(f, SAMPLERATE * 2);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, total * 2);
    fwrite(out.data(), 2, total, f);
    fclose(f);
    return 0;
}
