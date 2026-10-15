/*
 * The controllers onto the cabinet; see c1943_input.h. After pico-galagino's
 * mapInputs().
 */
#include "c1943_input.h"

#include "c1943.h"

/* A coin switch is a pulse: held this many frames. */
#define COIN_FRAMES 6

void c1943_pads_reset(c1943_pads_t *s)
{
    s->prev = s->prev2 = ~0u;
    s->select_used = 1;
    s->coin = 0;
    s->wait_release = 1;
}

static uint32_t player(uint32_t pad, int rapid_a, int rapid_b, int rapid_phase)
{
    uint32_t in = 0;
    if (pad & C1943_PAD_RIGHT)
        in |= C1943_IN_RIGHT;
    if (pad & C1943_PAD_LEFT)
        in |= C1943_IN_LEFT;
    if (pad & C1943_PAD_DOWN)
        in |= C1943_IN_DOWN;
    if (pad & C1943_PAD_UP)
        in |= C1943_IN_UP;
    if ((pad & (C1943_PAD_A | C1943_PAD_X)) && (!rapid_a || rapid_phase))
        in |= C1943_IN_B1;
    if ((pad & (C1943_PAD_B | C1943_PAD_Y)) && (!rapid_b || rapid_phase))
        in |= C1943_IN_B2;
    return in;
}

uint32_t c1943_pads_map(c1943_pads_t *s, uint32_t p1, uint32_t p2, int rapid_a, int rapid_b, int *settings, int *fps)
{
    const uint32_t combo = C1943_PAD_SELECT | C1943_PAD_START;
    const uint32_t pushed = p1 & ~s->prev;
    s->frame++;
    *settings = (p1 & combo) == combo && (s->prev & combo) != combo;
    *fps = (p1 & C1943_PAD_START) && (pushed & C1943_PAD_A);

    if (p1 & C1943_PAD_SELECT)
    {
        if (p1 & ~(uint32_t)C1943_PAD_SELECT)
            s->select_used = 1;
    }
    else if (s->prev & C1943_PAD_SELECT)
    {
        if (!s->select_used)
            s->coin = COIN_FRAMES;
        s->select_used = 0;
    }
    if (pushed & C1943_PAD_COIN)
        s->coin = COIN_FRAMES;
    s->prev = p1;
    s->prev2 = p2;

    if (s->wait_release)
    {
        s->wait_release = (p1 | p2) != 0;
        s->coin = 0;
        return 0;
    }

    uint32_t in = 0;
    if (s->coin > 0)
    {
        s->coin--;
        in |= C1943_IN_COIN1;
    }
    if (p1 & C1943_PAD_SELECT)
        return in; /* a modifier combination, not game input */

    const int rapid_phase = (s->frame & 4) != 0;
    if ((p1 & C1943_PAD_START) && !(p1 & C1943_PAD_A))
        in |= C1943_IN_START1;
    if ((p2 & C1943_PAD_START) || (p1 & C1943_PAD_START2))
        in |= C1943_IN_START2;
    /* while START is held, A toggles the frame rate rather than fire */
    in |= player((p1 & C1943_PAD_START) ? (p1 & ~(uint32_t)C1943_PAD_A) : p1, rapid_a, rapid_b, rapid_phase);
    in |= C1943_IN_P2(player(p2, rapid_a, rapid_b, rapid_phase));
    return in;
}
