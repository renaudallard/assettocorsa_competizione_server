#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "entrylist.h"

static void check(const struct CarEntry *c, int kg, float restrictor)
{
    assert(c->ballast_kg == kg);
    assert(fabsf(c->restrictor - restrictor) < .000001f);
}

int main(void)
{
    struct Server *s = calloc(1, sizeof(*s));
    struct CarEntry car = {0};
    assert(s);
    strcpy(s->track, "red_bull_ring");
    s->bop_count = 3;
    strcpy(s->bop[0].track, "red_bull_ring");
    s->bop[0].car_model = 35;
    s->bop[0].ballast_kg = 23;
    s->bop[0].restrictor_pct = 4;
    strcpy(s->bop[1].track, "spa");
    s->bop[1].car_model = 35;
    s->bop[1].ballast_kg = -7;
    s->bop[1].restrictor_pct = 2;
    strcpy(s->bop[2].track, "red_bull_ring");
    s->bop[2].car_model = 32;
    s->bop[2].ballast_kg = -21;
    car.car_model = 35;
    bop_apply(s, &car); check(&car, 23, .04f);
    bop_apply(s, &car); check(&car, 23, .04f);
    car.car_model = 32;
    bop_apply(s, &car); check(&car, -21, 0);
    car.car_model = 35;
    strcpy(s->track, "spa");
    bop_apply(s, &car); check(&car, -7, .02f);
    memset(&car, 0, sizeof(car));
    strcpy(s->track, "red_bull_ring");
    car.car_model = 35; car.ballast_kg = 5; car.restrictor = .02f;
    bop_apply(s, &car); check(&car, 28, .06f);
    bop_apply(s, &car); check(&car, 28, .06f);
    memset(&car, 0, sizeof(car));
    car.car_model = 35; car.ballast_kg = 35; car.restrictor = .19f;
    bop_apply(s, &car); check(&car, 40, .20f);
    bop_apply(s, &car); check(&car, 40, .20f);
    car.car_model = 36;
    bop_apply(s, &car); check(&car, 35, .19f);
    free(s);
    puts("PASS: track/model matching, signed ballast, normalized restrictor, reconnect idempotence, entry penalties and clamps");
    return 0;
}
