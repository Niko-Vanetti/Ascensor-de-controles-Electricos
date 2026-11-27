#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "elevator.h"
#include "rules.h"
#include "driver/gpio.h"
static int pins[64]; static int64_t clock_us;
int64_t esp_timer_get_time(void) {return clock_us;}
int gpio_config(const gpio_config_t *c){return 0;}
int gpio_get_level(int g){assert(g>=0 && g<64); return pins[g];}
int gpio_set_level(int g,int v){pins[g]=v;return 0;}
int gpio_reset_pin(int g){return 0;}
static elevator_config_t cfg;
static void step(int ms){clock_us+=ms*1000; elevator_tick(); rules_tick();}
static void floor_at(int f){for(int i=0;i<4;i++) pins[cfg.floor_gpio[i]]=i+1==f; step(100);step(100);}
static elevator_state_t state(void){elevator_state_t s;elevator_get_state(&s);return s;}
static void init(void){memset(&cfg,0,sizeof(cfg));cfg.floors=4;cfg.demo_mode=true;for(int i=0;i<4;i++){cfg.floor_gpio[i]=i+2;cfg.floor_level[i]=1;}memset(pins,0,sizeof(pins)); elevator_init(&cfg);floor_at(3);}

static void test_door_cycle(void)
{
    init();
    step(10000);
    assert(state().doors_open && !state().doors_closing && !state().moving);
    assert(elevator_call(1, ELEV_DIR_IDLE) == ESP_OK);
    step(2200);
    assert(state().doors_open && !state().doors_closing && !state().moving);
    assert(elevator_call(2, ELEV_DIR_IDLE) == ESP_OK);
    step(100);
    assert(state().doors_closing && !state().moving);
    floor_at(0);
    assert(!state().position_known && !state().moving);
    step(499);
    assert(!state().moving);
    step(1);
    assert(state().moving && !state().doors_open);

    floor_at(2);
    assert(state().doors_open && !state().moving);
    step(2299);
    assert(!state().doors_closing && !state().moving);
    step(1);
    assert(state().doors_closing && !state().moving);
    step(699);
    assert(!state().moving);
    step(1);
    assert(state().moving && !state().doors_open);

    floor_at(1);
    step(10000);
    assert(state().doors_open && !state().doors_closing && !state().moving);
}

int main(void){
 test_door_cycle();
 init(); assert(state().observed_dir == ELEV_DIR_IDLE);
 floor_at(0); floor_at(3); assert(state().observed_dir == ELEV_DIR_IDLE);
 floor_at(2); assert(state().observed_dir == ELEV_DIR_DOWN);
 floor_at(3); assert(state().observed_dir == ELEV_DIR_UP);
 pins[2]=1; step(100); step(100);
 assert(state().fault && state().observed_dir == ELEV_DIR_IDLE);
 init(); assert(!state().demo_mode); assert(state().position_known);
 elevator_call(1,ELEV_DIR_IDLE); step(3100); assert(state().moving);
 elevator_call(2,ELEV_DIR_DOWN); floor_at(2);
 assert(!state().moving && state().doors_open); assert(state().cabin_calls==1);
 step(3100);assert(state().moving);floor_at(1);assert(state().doors_open);
 init();elevator_call(1,ELEV_DIR_IDLE);elevator_call(2,ELEV_DIR_IDLE);step(3100);floor_at(2);assert(state().doors_open);
 init();elevator_call(1,ELEV_DIR_IDLE);elevator_call(2,ELEV_DIR_UP);step(3100);floor_at(2);assert(state().moving);floor_at(1);step(3100);floor_at(2);assert(state().doors_open);
 init();rule_block_t b[2];for(int i=0;i<2;i++){rules_block_defaults(&b[i]);b[i].source=RULE_BUTTON_DOWN;b[i].floor=i+2;b[i].out_gpio=25;}
 assert(rules_start(b,2)==ESP_OK);elevator_call(2,ELEV_DIR_DOWN);rules_tick();assert(pins[25]==1);rules_stop();assert(pins[25]==0);
 b[0].out_gpio=2;char err[160];assert(rules_validate(b,1,err,sizeof(err))!=ESP_OK);
 rules_stop();init();rules_block_defaults(&b[0]);b[0].in_gpio=27;b[0].action=1;b[0].action_floor=2;
 assert(rules_start(b,1)==ESP_OK);pins[27]=1;rules_tick();clock_us+=40000;rules_tick();assert(state().cabin_calls & 2);rules_stop();
 init();rules_block_defaults(&b[0]);b[0].source=RULE_BUTTON_CABIN;b[0].floor=3;b[0].out_gpio=25;
 assert(rules_start(b,1)==ESP_OK);elevator_call(3,ELEV_DIR_IDLE);rules_tick();assert(pins[25]==1);clock_us+=301000;rules_tick();assert(pins[25]==0);
 elevator_call(3,ELEV_DIR_IDLE);rules_tick();assert(pins[25]==1);pins[2]=1;step(100);step(100);assert(state().fault);assert(!rules_enabled());assert(pins[25]==0);rules_stop();
 init();elevator_call(1,ELEV_DIR_IDLE);elevator_call(2,ELEV_DIR_IDLE);elevator_call(2,ELEV_DIR_UP);step(3100);floor_at(2);assert(state().doors_open);assert(state().up_calls & 2);step(3100);assert(state().moving && state().dir==ELEV_DIR_DOWN);floor_at(1);step(3100);floor_at(2);assert(state().doors_open && !state().up_calls);
 assert(elevator_call(1,ELEV_DIR_DOWN)!=ESP_OK);assert(elevator_call(4,ELEV_DIR_UP)!=ESP_OK);
 puts("PASS: GPIO real, llamadas intermedias interiores/exteriores, sentido opuesto, OR de bloques, reposo y conflictos de sensores");
}
