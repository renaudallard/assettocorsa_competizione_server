#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "results.c"
int main(void) {
 struct CarEntry a={0},b={0};
 a.used=0;b.used=1;a.race.best_lap_ms=90000;b.race.best_lap_ms=100000;
 assert(results_practice_compare(&a,&b)<0);
 a.race.best_lap_ms=100000;a.race.best_lap_set_at_ms=10;b.race.best_lap_set_at_ms=20;
 assert(results_practice_compare(&a,&b)<0);
 a.race.disqualified=1;assert(results_practice_compare(&a,&b)>0);a.race.disqualified=0;
 a.race.car_field=0x40;assert(results_practice_compare(&a,&b)>0);a.race.car_field=0;
 a.race.best_lap_ms=0;assert(results_practice_compare(&a,&b)>0);
 b.race.best_lap_ms=0;assert(results_practice_compare(&a,&b)==0);
 puts("PASS: disconnected, equal-time, disqualified, retired and no-time classification");
 return 0;
}
