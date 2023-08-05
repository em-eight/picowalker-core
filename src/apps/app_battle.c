#include <stdint.h>
#include <stddef.h>

#include "app_battle.h"
#include "../states.h"
#include "../screen.h"
#include "../eeprom_map.h"
#include "../globals.h"
#include "../buttons.h"
#include "../rand.h"
#include "../utils.h"

/** @file apps/app_battle.c
 *
 * ```
 *  reg_a = chosen_pokemon (0..3)
 *  reg_b = [0]=?, [1..2]=our_action, [3..4]=their_action, [5..7]=?
 *  reg_c = anim_frame
 *  reg_d = [0..3]=our_hp, [4..7]=their_hp
 *  reg_x = substate_queue index + 1
 *  reg_y = substate_queue_len
 * ```
 *
 */

#define OUR_HP_OFFSET       0
#define THEIR_HP_OFFSET     4
#define OUR_HP_MASK         (0xf<<OUR_HP_OFFSET)
#define THEIR_HP_MASK       (0xf<<THEIR_HP_OFFSET)
#define OUR_ACTION_OFFSET   1
#define OUR_ACTION_MASK     (0x3<<OUR_ACTION_OFFSET)
#define THEIR_ACTION_OFFSET 3
#define THEIR_ACTION_MASK   (0x3<<THEIR_ACTION_OFFSET)
#define CHOICE_INDEX_OFFSET 5
#define CHOICE_INDEX_MASK   (0x7<<CHOICE_INDEX_OFFSET)

#define CURRENT_WOBBLE_MASK (0x0f)
#define MAX_WOBBLE_OFFSET   4

#define ATTACK_ANIM_LENGTH 9
#define STAREDOWN_ANIM_LENGTH 5
#define THREW_BALL_ANIM_LENGTH 6
#define CLOUD_ANIM_LENGTH 2
#define WOBBLE_ANIM_LENGTH 5
#define MESSAGE_DISPLAY_ANIM_LENGTH 4
#define CATCH_ANIM_LENGTH 4

enum {
    ACTION_ATTACK,
    ACTION_EVADE,
    ACTION_SPECIAL,
    N_ACTIONS,
};

static uint8_t substate_queue[8];

// what happens to `cur` hp given both actions
// valid for both us vs. them and them vs. us
static const uint8_t HP_MATRIX[3][3] = {
    //                foe attack, foe evade,  foe crit
    /* cur attack */ {         1,         1,         2},
    /* cur evade  */ {         0,         0,         0},
    /* cur crit   */ {         1,         1,         2},
};

static const uint8_t WOBBLE_CHANCES[4] = { 97, 79, 66, 56 };

/*
 *  Note: same animations for attack and evade
 *  just flip ours/theirs on evade
 */
const screen_pos_t OUR_ATTACK_XS[2][ATTACK_ANIM_LENGTH] = {
    /* us   */ {56, 56, 54, 52, 53, 54, 55, 56, 56}, // copied from walker FLASH:0xbb12
    /* them */ {8,   8,  8,  8,  0,  0,  4,  8,  8}
};
const screen_pos_t THEIR_ATTACK_XS[2][ATTACK_ANIM_LENGTH] = {
    /* us   */ {56, 56, 56, 56, 64, 64, 64, 60, 56}, // copied from walker FLASH:0xbb12
    /* them */ {8,   8, 10, 12, 12, 11, 10,  9,  8}
};

const screen_pos_t POKEBALL_THROW_XS[6] = { 44, 40, 36, 32, 28, 24 };
const screen_pos_t POKEBALL_THROW_YS[6] = { 20, 14,  9,  6,  4,  6 };
const int8_t POKEMON_ENTER_XS[4] = { -16, -4, 8, 8 };


#define WOBBLE_INITIAL_X 16
#define WOBBLE_INITIAL_Y 16

#define OUR_NORMAL_X    56
#define OUR_NORMAL_Y    8
#define THEIR_NORMAL_X  8
#define THEIR_NORMAL_Y  0

const uint8_t ACTION_CHANCES[5][3] = {
    // atk, evade, crit/flee
    {   45,    35,        20},
    {   40,    30,        30},
    {   50,    40,        10},
    {   60,    30,        10},
    {   20,    30,        50},
};

/**
 * Lazy way of switching substate, recording the last one and then requesting a redraw
*/
static void pw_battle_switch_substate(pw_state_t *s, uint8_t sid) {
    s->battle.previous_substate = s->battle.current_substate;
    s->battle.current_substate = sid;
    PW_SET_REQUEST(s->requests, PW_REQUEST_REDRAW);
}

/**
 *  Initialises `app_battle_t` struct.
 *
 *  @param s Pointer to current state.
 *  @param sf Pointer to current screen flags.
 *
 */
void pw_battle_init(pw_state_t *s, const screen_flags_t *sf) {
    s->battle.current_substate = BATTLE_OPENING;
    s->battle.previous_substate = BATTLE_OPENING;
    s->battle.actions = 0;
    s->battle.anim_frame = 4;
    s->battle.current_hp = (4<<OUR_HP_OFFSET) | (4<<THEIR_HP_OFFSET);
    s->battle.switch_cursor = 0;
    s->battle.prev_switch_cursor = 0;
}

/**
 * Main event loop for battle state.
 *
 * @param s Pointer to current state, always interpreted as `app_battle_t`.
 * @param p Pointer to pending state. Switches to this state if `p->sid != s->sid`.
 * @param sf Pointer to current screen flags.
 *
 */
void pw_battle_event_loop(pw_state_t *s, pw_state_t *p, const screen_flags_t *sf) {
    switch(s->battle.current_substate) {
    case BATTLE_OPENING: {
        if(s->battle.anim_frame <= 0) {
            pw_battle_switch_substate(s, BATTLE_APPEARED);
        }
        break;
    }
    case BATTLE_APPEARED: {
        break;
    }
    case BATTLE_CHOOSING: {
        if(s->battle.current_substate != s->battle.previous_substate) s->battle.substate_queue_index = 0;
        if(s->battle.substate_queue_index == 1) {
            uint8_t our_action   = (s->battle.actions&OUR_ACTION_MASK)>>OUR_ACTION_OFFSET;
            uint8_t their_action = (s->battle.actions&THEIR_ACTION_MASK)>>THEIR_ACTION_OFFSET;
            uint8_t choice_index = (s->battle.actions&CHOICE_INDEX_MASK)>>CHOICE_INDEX_OFFSET;

            // decide their action
            uint8_t rnd = pw_rand()%100;
            if(rnd < ACTION_CHANCES[choice_index][2]) {
                their_action = ACTION_SPECIAL;
            } else if(rnd < (ACTION_CHANCES[choice_index][2]+ACTION_CHANCES[choice_index][1])) {
                their_action = ACTION_EVADE;
            } else {
                their_action = ACTION_ATTACK;
            }

            s->battle.actions &= ~THEIR_ACTION_MASK;
            s->battle.actions |= (their_action<<THEIR_ACTION_OFFSET)&THEIR_ACTION_MASK;

            uint8_t our_hp   = (s->battle.current_hp&OUR_HP_MASK)>>OUR_HP_OFFSET;
            uint8_t their_hp = (s->battle.current_hp&THEIR_HP_MASK)>>THEIR_HP_OFFSET;

            /*
             * big matrix on what happens based on both actions
             * we can't "crit" since user input is only attack/evade.
             * so they choose to "crit" for us.
             * if we evade and they "crit" then they flee
             * if we attack and they "crit" then we have a crit hit
             *
             */
            if(our_action == ACTION_EVADE) {
                switch(their_action) {
                case ACTION_ATTACK: {
                    their_hp -= 1;
                    substate_queue[0] = BATTLE_THEIR_ACTION;
                    substate_queue[1] = BATTLE_OUR_ACTION;
                    substate_queue[2] = BATTLE_CHOOSING;
                    break;
                }
                case ACTION_EVADE: {
                    substate_queue[0] = BATTLE_STAREDOWN;
                    substate_queue[1] = BATTLE_CHOOSING;
                    break;
                }
                case ACTION_SPECIAL: {
                    substate_queue[0] = BATTLE_THEY_FLED;
                    break;
                }
                }
            } else {
                substate_queue[0] = BATTLE_OUR_ACTION;
                substate_queue[1] = BATTLE_THEIR_ACTION;
                substate_queue[2] = BATTLE_CHOOSING;

                switch(their_action) {
                case ACTION_ATTACK: {
                    our_hp -= 1;
                    their_hp -= 1;
                    s->battle.actions &= ~CHOICE_INDEX_MASK;
                    s->battle.actions |= 1<<CHOICE_INDEX_OFFSET; // taken from walker
                    break;
                }
                case ACTION_EVADE: {
                    our_hp -= 1;
                    s->battle.actions &= ~CHOICE_INDEX_MASK;
                    s->battle.actions |= 3<<CHOICE_INDEX_OFFSET; // taken from walker
                    break;
                }
                case ACTION_SPECIAL: {
                    our_hp -= 1;
                    their_hp -= 2;
                    s->battle.actions &= ~CHOICE_INDEX_MASK;
                    s->battle.actions |= 2<<CHOICE_INDEX_OFFSET; // taken from walker
                    break;
                }
                }
            }

            s->battle.current_hp = our_hp<<OUR_HP_OFFSET | their_hp<<THEIR_HP_OFFSET;

            pw_battle_switch_substate(s, substate_queue[s->battle.substate_queue_index-1]);
            s->battle.anim_frame = 0;
        }
        break;
    }
    case BATTLE_THEIR_ACTION: {
        if(s->battle.anim_frame == ATTACK_ANIM_LENGTH) {
            uint8_t our_hp = (s->battle.current_hp&OUR_HP_MASK)>>OUR_HP_OFFSET;
            if(our_hp == 0 || our_hp > 4) {
                s->battle.anim_frame = 0;
                pw_battle_switch_substate(s, BATTLE_WE_LOST);
                return;
            }

            s->battle.substate_queue_index++;
            s->battle.anim_frame = 0;
            pw_battle_switch_substate(s, substate_queue[s->battle.substate_queue_index-1]);
        }
        break;
    }
    case BATTLE_OUR_ACTION: {
        if(s->battle.anim_frame == ATTACK_ANIM_LENGTH) {
            uint8_t their_hp = (s->battle.current_hp&THEIR_HP_MASK)>>THEIR_HP_OFFSET;
            if(their_hp == 0 || their_hp > 4) {
                s->battle.anim_frame = 0;
                pw_battle_switch_substate(s, BATTLE_THEY_FLED);
                return;
            }

            s->battle.substate_queue_index++;
            s->battle.anim_frame = 0;
            pw_battle_switch_substate(s, substate_queue[s->battle.substate_queue_index-1]);
        }
        break;
    }
    case BATTLE_STAREDOWN: {
        if(s->battle.anim_frame == STAREDOWN_ANIM_LENGTH) {
            s->battle.substate_queue_index++;
            s->battle.anim_frame = 0;
            pw_battle_switch_substate(s, substate_queue[s->battle.substate_queue_index-1]);
        }
        break;
    }
    case BATTLE_THEY_FLED: {
        if(s->battle.anim_frame >= MESSAGE_DISPLAY_ANIM_LENGTH) {
            p->sid = STATE_SPLASH;
        }
        break;
    }
    case BATTLE_WE_LOST: {
        if(s->battle.anim_frame >= MESSAGE_DISPLAY_ANIM_LENGTH) {
            p->sid = STATE_SPLASH;
        }
        break;
    }
    case BATTLE_CATCH_SETUP: {

        substate_queue[0] = BATTLE_THREW_BALL;
        substate_queue[1] = BATTLE_CLOUD_ANIM;

        int8_t health = (s->battle.current_hp&THEIR_HP_MASK) >> THEIR_HP_OFFSET;
        uint8_t wobble_chance = WOBBLE_CHANCES[health-1];
        if(health <= 0) {
            substate_queue[0] = BATTLE_THEY_FLED;
            s->battle.substate_queue_len = 1;
            return;
        }

        // 1-3 wobbles, can flee at three wobbles
        bool caught = true;
        uint8_t n_wobbles = 0;
        while(n_wobbles < 3 && caught) {
            n_wobbles++;
            uint8_t pct = pw_rand()%100;
            if(pct >= wobble_chance) {
                caught = false;
            }
        }

        s->battle.actions = n_wobbles<<MAX_WOBBLE_OFFSET;  // reuse reg_b for wobble count

        if(caught) {
            substate_queue[2] = BATTLE_BALL_WOBBLE;
            substate_queue[3] = BATTLE_CLOUD_ANIM;
            substate_queue[4] = BATTLE_ALMOST_HAD_IT;
            substate_queue[5] = BATTLE_THEY_FLED;
            s->battle.substate_queue_len = 6;
        } else {
            substate_queue[2] = BATTLE_BALL_WOBBLE;
            substate_queue[3] = BATTLE_CATCH_STARS;
            substate_queue[4] = BATTLE_POKEMON_CAUGHT;
            s->battle.substate_queue_len = 5;
        }

        s->battle.substate_queue_index = 1;  // 1-indexed
        s->battle.anim_frame = 0;  // reset anim frame count
        pw_battle_switch_substate(s, substate_queue[s->battle.substate_queue_index-1]);

        break;
    }
    case BATTLE_THREW_BALL: {
        if(s->battle.anim_frame >= THREW_BALL_ANIM_LENGTH) {
            s->battle.substate_queue_index++;
            s->battle.anim_frame = 0;
            pw_battle_switch_substate(s, substate_queue[s->battle.substate_queue_index-1]);
        }
        break;
    }
    case BATTLE_CLOUD_ANIM: {
        if(s->battle.anim_frame >= CLOUD_ANIM_LENGTH) {
            s->battle.substate_queue_index++;
            s->battle.anim_frame = 0;
            pw_battle_switch_substate(s, substate_queue[s->battle.substate_queue_index-1]);

        }
        break;
    }
    case BATTLE_BALL_WOBBLE: {
        if(s->battle.anim_frame >= WOBBLE_ANIM_LENGTH) {
            uint8_t current_wobble = s->battle.actions & CURRENT_WOBBLE_MASK+1;
            uint8_t max_wobble = s->battle.actions >> MAX_WOBBLE_OFFSET;

            if(current_wobble < max_wobble) {
                current_wobble++;
                s->battle.actions &= ~CURRENT_WOBBLE_MASK;
                s->battle.actions |= current_wobble;
                s->battle.anim_frame = 0;
            } else {
                s->battle.substate_queue_index++;
                s->battle.anim_frame = 0;
                pw_battle_switch_substate(s, substate_queue[s->battle.substate_queue_index-1]);

            }
        }
        break;
    }
    case BATTLE_ALMOST_HAD_IT: {
        if(s->battle.anim_frame >= MESSAGE_DISPLAY_ANIM_LENGTH) {
            s->battle.substate_queue_index++;
            s->battle.anim_frame = 0;
            pw_battle_switch_substate(s, substate_queue[s->battle.substate_queue_index-1]);

        }
        break;
    }
    case BATTLE_POKEMON_CAUGHT: {
        if(s->battle.anim_frame >= MESSAGE_DISPLAY_ANIM_LENGTH) {
            /*
             *  TODO:
             *  - check if switch screen needed
             *  - actually store pokemon caught
             */
        }
        break;
    }
    case BATTLE_PROCESS_CAUGHT_POKEMON: {

        if(s->battle.chosen_pokemon >= 3) {
            // event mon
            pokemon_summary_t *caught_poke = (pokemon_summary_t*)eeprom_buf;
            pw_eeprom_read(
                PW_EEPROM_ADDR_EVENT_POKEMON_BASIC_DATA,
                (uint8_t*)(caught_poke),
                sizeof(*caught_poke)
            );
            if(caught_poke->le_species == 0x0000 || caught_poke->le_species == 0xffff) {

                // basic data
                pw_eeprom_read(
                    PW_EEPROM_ADDR_SPECIAL_POKEMON_BASIC_DATA,
                    (uint8_t*)(caught_poke),
                    sizeof(*caught_poke)
                );
                pw_eeprom_write(
                    PW_EEPROM_ADDR_EVENT_POKEMON_BASIC_DATA,
                    (uint8_t*)(caught_poke),
                    sizeof(*caught_poke)
                );

                // extra data
                pw_eeprom_read(
                    PW_EEPROM_ADDR_SPECIAL_POKEMON_EXTRA_DATA,
                    eeprom_buf,
                    PW_EEPROM_SIZE_SPECIAL_POKEMON_EXTRA_DATA
                );
                pw_eeprom_write(
                    PW_EEPROM_ADDR_EVENT_POKEMON_EXTRA_DATA,
                    eeprom_buf,
                    PW_EEPROM_SIZE_EVENT_POKEMON_EXTRA_DATA
                );

                // small sprite
                pw_eeprom_read(
                    PW_EEPROM_ADDR_IMG_SPECIAL_POKEMON_SMALL_ANIMATED,
                    eeprom_buf,
                    PW_EEPROM_SIZE_IMG_SPECIAL_POKEMON_SMALL_ANIMATED
                );
                pw_eeprom_write(
                    PW_EEPROM_ADDR_IMG_EVENT_POKEMON_SMALL_ANIMATED,
                    eeprom_buf,
                    PW_EEPROM_SIZE_IMG_EVENT_POKEMON_SMALL_ANIMATED
                );

                // name text
                pw_eeprom_read(
                    PW_EEPROM_ADDR_TEXT_SPECIAL_POKEMON_NAME,
                    eeprom_buf,
                    PW_EEPROM_SIZE_TEXT_SPECIAL_POKEMON_NAME
                );
                pw_eeprom_write(
                    PW_EEPROM_ADDR_TEXT_EVENT_POKEMON_NAME,
                    eeprom_buf,
                    PW_EEPROM_SIZE_TEXT_EVENT_POKEMON_NAME
                );


                p->sid = STATE_SPLASH;
            } else {
                // TODO: idk? silently overwrite?
                p->sid = STATE_SPLASH;
            }

        } else {
            // normal mon
            pokemon_summary_t caught_pokes[3];
            pw_eeprom_read(
                PW_EEPROM_ADDR_CAUGHT_POKEMON_SUMMARY,
                (uint8_t*)caught_pokes,
                sizeof(caught_pokes)
            );

            size_t i = 0;
            for(i = 0; i < 3; i++) {
                if(caught_pokes[i].le_species == 0x0000 || caught_pokes[i].le_species == 0xffff) break;
            }

            if(i == 3) {
                // no space
                s->battle.prev_switch_cursor = 0xff;
                pw_battle_switch_substate(s, BATTLE_SWITCH);
            } else {
                route_info_t ri;
                pw_eeprom_read(
                    PW_EEPROM_ADDR_ROUTE_INFO,
                    (uint8_t*)(&ri),
                    sizeof(ri)
                );
                caught_pokes[i] = ri.route_pokemon[s->battle.chosen_pokemon];
                pw_eeprom_write(
                    PW_EEPROM_ADDR_CAUGHT_POKEMON_SUMMARY,
                    (uint8_t*)caught_pokes,
                    sizeof(caught_pokes)
                );
                p->sid = STATE_SPLASH;
            }
        }
        break;
    }
    case BATTLE_CATCH_STARS: {
        if(s->battle.anim_frame >= CATCH_ANIM_LENGTH) {
            s->battle.substate_queue_index++;
            s->battle.anim_frame = 0;
            pw_battle_switch_substate(s, substate_queue[s->battle.substate_queue_index-1]);
        }
        break;
    }
    case BATTLE_SWITCH: {
        break;
    }
    case BATTLE_GO_TO_SPLASH: {
        p->sid = STATE_SPLASH;
        break;
    }
    default: {
        //printf("[ERROR] Unhandled substate init: 0x%02x\n", s->battle.current_substate);
        break;
    }

    }
}

void pw_battle_init_display(pw_state_t *s, const screen_flags_t *sf) {

    pw_img_t our_sprite   = {.width=32, .height=24, .size=192, .data=eeprom_buf};
    pw_img_t their_sprite = {.width=32, .height=24, .size=192, .data=decompression_buf};

    pw_pokemon_index_to_small_sprite(s->battle.chosen_pokemon+1, their_sprite.data, sf->frame);

    pw_pokemon_index_to_small_sprite(PIDX_WALKING, our_sprite.data, sf->frame);

    switch(s->battle.current_substate) {
    case BATTLE_OPENING: {
        pw_screen_fill_area(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_BLACK);
        break;
    }
    case BATTLE_APPEARED: {
        pw_screen_draw_from_eeprom(
            0, SCREEN_HEIGHT-16,
            96, 16,
            PW_EEPROM_ADDR_TEXT_APPEARED,
            PW_EEPROM_SIZE_TEXT_APPEARED
        );
        pw_screen_draw_from_eeprom(
            0, SCREEN_HEIGHT-32,
            80, 16,
            PW_EEPROM_ADDR_TEXT_POKEMON_NAMES + s->battle.chosen_pokemon*PW_EEPROM_SIZE_TEXT_POKEMON_NAME,
            PW_EEPROM_SIZE_TEXT_POKEMON_NAME
        );
        pw_screen_draw_text_box(0, SCREEN_HEIGHT-32, SCREEN_WIDTH-1, SCREEN_HEIGHT-1, SCREEN_BLACK);

        pw_img_t health_bar = {.width=8, .height=8, .data=eeprom_buf, .size=16};
        pw_eeprom_read(PW_EEPROM_ADDR_IMG_RADAR_HP_BLIP, eeprom_buf, PW_EEPROM_SIZE_IMG_RADAR_HP_BLIP);

        int8_t health = (s->battle.current_hp&THEIR_HP_MASK) >> THEIR_HP_OFFSET;
        for(int8_t i = 0; i < health; i++) {
            pw_screen_draw_img(&health_bar, 8*(i+1), 24);
        }

        health = (s->battle.current_hp&OUR_HP_MASK) >> OUR_HP_OFFSET;
        for(int8_t i = 0; i < health; i++) {
            pw_screen_draw_img(&health_bar, SCREEN_WIDTH/2 + 8*(i+1), 0);
        }

        pw_screen_draw_img(&their_sprite, THEIR_NORMAL_X, THEIR_NORMAL_Y);
        pw_screen_draw_img(&our_sprite, OUR_NORMAL_X, OUR_NORMAL_Y);

        break;
    }
    case BATTLE_CHOOSING: {
        pw_screen_draw_from_eeprom(
            0, SCREEN_HEIGHT-32,
            96, 32,
            PW_EEPROM_ADDR_TEXT_RADAR_ACTION,
            PW_EEPROM_SIZE_TEXT_RADAR_ACTION
        );
        break;
    }
    case BATTLE_OUR_ACTION: {
        uint8_t our_action = (s->battle.actions&OUR_ACTION_MASK)>>OUR_ACTION_OFFSET;
        uint8_t their_action = (s->battle.actions&THEIR_ACTION_MASK)>>THEIR_ACTION_OFFSET;

        pw_screen_clear_area(80, SCREEN_HEIGHT-32, SCREEN_WIDTH-80, 16);
        switch(their_action) {
        case ACTION_ATTACK: {
            pw_screen_draw_from_eeprom(
                0, SCREEN_HEIGHT-32,
                80, 16,
                PW_EEPROM_ADDR_TEXT_POKEMON_NAME,
                PW_EEPROM_SIZE_TEXT_POKEMON_NAME
            );
            pw_screen_draw_from_eeprom(
                0, SCREEN_HEIGHT-16,
                SCREEN_WIDTH, 16,
                PW_EEPROM_ADDR_TEXT_ATTACKED,
                PW_EEPROM_SIZE_TEXT_ATTACKED
            );
            break;
        }
        case ACTION_EVADE: {
            pw_screen_draw_from_eeprom(
                0, SCREEN_HEIGHT-32,
                80, 16,
                PW_EEPROM_ADDR_TEXT_POKEMON_NAMES + s->battle.chosen_pokemon*PW_EEPROM_SIZE_TEXT_POKEMON_NAME,
                PW_EEPROM_SIZE_TEXT_POKEMON_NAME
            );
            pw_screen_draw_from_eeprom(
                0, SCREEN_HEIGHT-16,
                SCREEN_WIDTH, 16,
                PW_EEPROM_ADDR_TEXT_EVADED,
                PW_EEPROM_SIZE_TEXT_EVADED
            );

            break;
        }
        case ACTION_SPECIAL: {
            pw_screen_draw_from_eeprom(
                0, SCREEN_HEIGHT-32,
                SCREEN_WIDTH, 16,
                PW_EEPROM_ADDR_TEXT_CRITICAL_HIT,
                PW_EEPROM_SIZE_TEXT_CRITICAL_HIT
            );
            pw_screen_clear_area(0, SCREEN_HEIGHT-16, SCREEN_WIDTH, 16);
            break;
        }
        }
        pw_screen_draw_text_box(0, SCREEN_HEIGHT-32, SCREEN_WIDTH-1, SCREEN_HEIGHT-1, SCREEN_BLACK);
        break;
    }
    case BATTLE_THEIR_ACTION: {
        uint8_t our_action = (s->battle.actions&OUR_ACTION_MASK)>>OUR_ACTION_OFFSET;
        uint8_t their_action = (s->battle.actions&THEIR_ACTION_MASK)>>THEIR_ACTION_OFFSET;

        pw_screen_clear_area(80, SCREEN_HEIGHT-32, SCREEN_WIDTH-80, 16);
        if(our_action == ACTION_EVADE) {
            pw_screen_draw_from_eeprom(
                0, SCREEN_HEIGHT-32,
                80, 16,
                PW_EEPROM_ADDR_TEXT_POKEMON_NAME,
                PW_EEPROM_SIZE_TEXT_POKEMON_NAME
            );
            pw_screen_draw_from_eeprom(
                0, SCREEN_HEIGHT-16,
                SCREEN_WIDTH, 16,
                PW_EEPROM_ADDR_TEXT_EVADED,
                PW_EEPROM_SIZE_TEXT_EVADED
            );

        } else {
            pw_screen_draw_from_eeprom(
                0, SCREEN_HEIGHT-32,
                80, 16,
                PW_EEPROM_ADDR_TEXT_POKEMON_NAMES + s->battle.chosen_pokemon*PW_EEPROM_SIZE_TEXT_POKEMON_NAME,
                PW_EEPROM_SIZE_TEXT_POKEMON_NAME
            );
            pw_screen_draw_from_eeprom(
                0, SCREEN_HEIGHT-16,
                SCREEN_WIDTH, 16,
                PW_EEPROM_ADDR_TEXT_ATTACKED,
                PW_EEPROM_SIZE_TEXT_ATTACKED
            );
        }
        pw_screen_draw_text_box(0, SCREEN_HEIGHT-32, SCREEN_WIDTH-1, SCREEN_HEIGHT-1, SCREEN_BLACK);
        break;
    }
    case BATTLE_THEY_FLED: {
        pw_screen_clear_area(80, SCREEN_HEIGHT-32, SCREEN_WIDTH-80, 16);
        pw_screen_draw_from_eeprom(
            0, SCREEN_HEIGHT-32,
            80, 16,
            PW_EEPROM_ADDR_TEXT_POKEMON_NAMES + s->battle.chosen_pokemon*PW_EEPROM_SIZE_TEXT_POKEMON_NAME,
            PW_EEPROM_SIZE_TEXT_POKEMON_NAME
        );
        pw_screen_draw_message(SCREEN_HEIGHT-16, 33, 16); // "fled..."
        pw_screen_draw_text_box(0, SCREEN_HEIGHT-32, SCREEN_WIDTH-1, SCREEN_HEIGHT-1, SCREEN_BLACK);
        break;
    }
    case BATTLE_WE_LOST: {
        pw_screen_draw_from_eeprom(
            0, SCREEN_HEIGHT-32,
            80, 16,
            PW_EEPROM_ADDR_TEXT_POKEMON_NAMES + s->battle.chosen_pokemon*PW_EEPROM_SIZE_TEXT_POKEMON_NAME,
            PW_EEPROM_SIZE_TEXT_POKEMON_NAME
        );
        pw_screen_draw_message(SCREEN_HEIGHT-16, 34, 16); // "was too strong..."
        pw_screen_draw_text_box(0, SCREEN_HEIGHT-32, SCREEN_WIDTH-1, SCREEN_HEIGHT-1, SCREEN_BLACK);
        break;
    }
    case BATTLE_STAREDOWN: {
        pw_screen_clear_area(0, SCREEN_HEIGHT-32, SCREEN_WIDTH, 16);
        pw_screen_draw_message(SCREEN_HEIGHT-16, 41, 16);
        break;
    }
    case BATTLE_THREW_BALL: {
        pw_screen_clear_area(0, SCREEN_HEIGHT-32, SCREEN_WIDTH, 16);
        pw_screen_draw_message(SCREEN_HEIGHT-16, 39, 16); // "threw a ball"
        break;
    }
    case BATTLE_CLOUD_ANIM: {
        pw_screen_clear_area(0, SCREEN_HEIGHT-32, SCREEN_WIDTH, 16);
        pw_screen_draw_from_eeprom(
            THEIR_NORMAL_X, THEIR_NORMAL_Y,
            32, 24,
            PW_EEPROM_ADDR_IMG_RADAR_APPEAR_CLOUD,
            PW_EEPROM_SIZE_IMG_RADAR_APPEAR_CLOUD
        );
        break;
    }
    case BATTLE_BALL_WOBBLE: {
        pw_screen_clear_area(0, SCREEN_HEIGHT-32, SCREEN_WIDTH, 16);
        pw_screen_clear_area(
            THEIR_NORMAL_X, THEIR_NORMAL_Y,
            32, 24
        );
        pw_screen_draw_from_eeprom(
            WOBBLE_INITIAL_Y, WOBBLE_INITIAL_Y,
            8, 8,
            PW_EEPROM_ADDR_IMG_BALL,
            PW_EEPROM_SIZE_IMG_BALL
        );
        break;
    }
    case BATTLE_ALMOST_HAD_IT: {
        pw_screen_draw_img(&their_sprite, THEIR_NORMAL_X, THEIR_NORMAL_Y);
        pw_screen_draw_message(SCREEN_HEIGHT-16, 40, 16); // "almost had it"
        break;
    }
    case BATTLE_CATCH_STARS: {
        pw_screen_clear_area(0, SCREEN_HEIGHT-32, SCREEN_WIDTH, 16);
        break;
    }
    case BATTLE_POKEMON_CAUGHT: {
        pw_screen_draw_from_eeprom(
            0, SCREEN_HEIGHT-32,
            80, 16,
            PW_EEPROM_ADDR_TEXT_POKEMON_NAMES + s->battle.chosen_pokemon*PW_EEPROM_SIZE_TEXT_POKEMON_NAME,
            PW_EEPROM_SIZE_TEXT_POKEMON_NAME
        );
        pw_screen_draw_message(SCREEN_HEIGHT-16, 32, 16); // "was caught!"
        break;
    }
    case BATTLE_SWITCH: {
        pw_screen_clear();
        pw_screen_draw_from_eeprom(
            0, 0,
            8, 16,
            PW_EEPROM_ADDR_IMG_MENU_ARROW_RETURN,
            PW_EEPROM_SIZE_IMG_MENU_ARROW_RETURN
        );

        pw_screen_draw_from_eeprom(
            8, 0,
            80, 16,
            PW_EEPROM_ADDR_TEXT_SWITCH,
            PW_EEPROM_SIZE_TEXT_SWITCH
        );
        for(uint8_t i = 0; i < 3; i++) {
            pw_screen_draw_from_eeprom(
                20+i*(16+8), SCREEN_HEIGHT-32-8,
                8, 8,
                PW_EEPROM_ADDR_IMG_BALL,
                PW_EEPROM_SIZE_IMG_BALL
            );
        }

        pw_screen_draw_from_eeprom(
            20+s->battle.switch_cursor*(16+8), SCREEN_HEIGHT-32,
            8, 8,
            PW_EEPROM_ADDR_IMG_ARROW_UP_NORMAL,
            PW_EEPROM_SIZE_IMG_ARROW
        );
        break;
    }
    default: {
        //printf("[ERROR] Unhandled substate draw: 0x%02x\n", s->battle.current_substate);
        break;
    }
    }
}

/*
 * coords:
 *   - i attack, they attack: me attack + they hit ("i attacked")-> me hit + they attack ("they attacked")
 *   - i evade, they attack: me evade + they hit ("I evaded")-> me attack + they hit ("I attacked")
 *   - i attack, they evade: me attack + they evade ("they evaded")-> me hit + they attack ("they attacked")
 *   - i evade, they evade: staredown ("staredown")
 *
 *   00 - me -> them
 *   01 - me -> them
 *   10 - them -> me
 *   11 - none - none
 */
void pw_battle_update_display(pw_state_t *s, const screen_flags_t *sf) {
    if(s->battle.current_substate != s->battle.previous_substate) {
        s->battle.previous_substate = s->battle.current_substate;
        pw_battle_init_display(s, sf);
        return;
    }

    pw_img_t our_sprite   = {.width=32, .height=24, .size=192, .data=eeprom_buf};
    pw_img_t their_sprite = {.width=32, .height=24, .size=192, .data=decompression_buf};

    pw_pokemon_index_to_small_sprite(s->battle.chosen_pokemon+1, their_sprite.data, sf->frame);

    pw_pokemon_index_to_small_sprite(PIDX_WALKING, our_sprite.data, sf->frame);

    switch(s->battle.current_substate) {
    case BATTLE_OPENING: {
        if(s->battle.anim_frame > 0) s->battle.anim_frame--;
        pw_screen_fill_area(0, s->battle.anim_frame*8, SCREEN_WIDTH, (4-s->battle.anim_frame)*16, SCREEN_WHITE);
        break;
    }
    case BATTLE_APPEARED: {
        pw_screen_draw_img(&their_sprite, 8, 0);
        pw_screen_draw_img(&our_sprite, SCREEN_WIDTH/2+8, 8);
        break;
    }
    case BATTLE_CHOOSING: {
        pw_screen_draw_img(&their_sprite, 8, 0);
        pw_screen_draw_img(&our_sprite, SCREEN_WIDTH/2+8, 8);

        break;
    }
    case BATTLE_OUR_ACTION: {
        uint8_t our_action = (s->battle.actions&OUR_ACTION_MASK)>>OUR_ACTION_OFFSET;
        uint8_t their_action = (s->battle.actions&THEIR_ACTION_MASK)>>THEIR_ACTION_OFFSET;
        pw_screen_draw_img(&our_sprite, OUR_ATTACK_XS[0][s->battle.anim_frame], 8);
        pw_screen_draw_img(&their_sprite, OUR_ATTACK_XS[1][s->battle.anim_frame], 0);

        if(s->battle.anim_frame == (ATTACK_ANIM_LENGTH+1)/2) {
            if(their_action == ACTION_SPECIAL) {
                pw_screen_draw_from_eeprom(
                    (SCREEN_WIDTH-16)/2, 0,
                    16, 32,
                    PW_EEPROM_ADDR_IMG_RADAR_CRITICAL_HIT,
                    PW_EEPROM_SIZE_IMG_RADAR_CRITICAL_HIT
                );
            } else if(their_action != ACTION_EVADE) {
                pw_screen_draw_from_eeprom(
                    (SCREEN_WIDTH-16)/2, 0,
                    16, 32,
                    PW_EEPROM_ADDR_IMG_RADAR_ATTACK_HIT,
                    PW_EEPROM_SIZE_IMG_RADAR_ATTACK_HIT
                );
            }

            uint8_t hp = (s->battle.current_hp&THEIR_HP_MASK)>>THEIR_HP_OFFSET;
            pw_screen_clear_area(8*(hp+1), 24, 8*(4-hp), 8);
        } else {
            pw_screen_clear_area(
                (SCREEN_WIDTH-16)/2, 0,
                16, 32
            );
        }

        s->battle.anim_frame++;
        break;
    }
    case BATTLE_THEIR_ACTION: {
        uint8_t our_action = (s->battle.actions&OUR_ACTION_MASK)>>OUR_ACTION_OFFSET;
        uint8_t their_action = (s->battle.actions&THEIR_ACTION_MASK)>>THEIR_ACTION_OFFSET;
        pw_screen_draw_img(&our_sprite,   THEIR_ATTACK_XS[0][s->battle.anim_frame], 8);
        pw_screen_draw_img(&their_sprite, THEIR_ATTACK_XS[1][s->battle.anim_frame], 0);

        if(s->battle.anim_frame == (ATTACK_ANIM_LENGTH+1)/2) {
            if(our_action != ACTION_EVADE) {
                pw_screen_draw_from_eeprom(
                    (SCREEN_WIDTH-16)/2, 0,
                    16, 32,
                    PW_EEPROM_ADDR_IMG_RADAR_ATTACK_HIT,
                    PW_EEPROM_SIZE_IMG_RADAR_ATTACK_HIT
                );
            }

            uint8_t hp = (s->battle.current_hp&OUR_HP_MASK)>>OUR_HP_OFFSET;
            pw_screen_clear_area(SCREEN_WIDTH/2+8*(hp+1), 0, 8*(4-hp), 8);

        } else {
            pw_screen_clear_area(
                (SCREEN_WIDTH-16)/2, 0,
                16, 32
            );
        }

        s->battle.anim_frame++;
        break;
    }
    case BATTLE_WE_LOST: {
        break;
    }
    case BATTLE_THEY_FLED: {
        // TODO: animation
        break;
    }
    case BATTLE_STAREDOWN: {
        pw_screen_draw_img(&our_sprite,   THEIR_ATTACK_XS[0][0], 8);
        pw_screen_draw_img(&their_sprite, THEIR_ATTACK_XS[1][0], 0);
        s->battle.anim_frame++;
        break;
    }
    case BATTLE_THREW_BALL: {
        if(s->battle.anim_frame > 0) {
            pw_screen_clear_area(
                POKEBALL_THROW_XS[s->battle.anim_frame-1], POKEBALL_THROW_YS[s->battle.anim_frame-1],
                8, 8
            );
        }
        pw_screen_draw_from_eeprom(
            POKEBALL_THROW_XS[s->battle.anim_frame], POKEBALL_THROW_YS[s->battle.anim_frame],
            8, 8,
            PW_EEPROM_ADDR_IMG_BALL,
            PW_EEPROM_SIZE_IMG_BALL
        );
        s->battle.anim_frame++;
        break;
    }
    case BATTLE_CLOUD_ANIM: {
        s->battle.anim_frame++;
        break;
    }
    case BATTLE_BALL_WOBBLE: {

        screen_pos_t middle = THEIR_NORMAL_X+8;
        screen_pos_t left   = middle-2;
        screen_pos_t right  = middle+2;

        screen_pos_t x = middle;
        switch(s->battle.anim_frame%4) {
        case 0:
        case 2: {
            x=middle;
            break;
        }
        case 1: {
            x=left;
            break;
        }
        case 3: {
            x=right;
            break;
        }
        }

        pw_screen_clear_area(left, 16, right-left+8, 8);
        pw_screen_draw_from_eeprom(
            x, 16,
            8, 8,
            PW_EEPROM_ADDR_IMG_BALL,
            PW_EEPROM_SIZE_IMG_BALL
        );
        s->battle.anim_frame++;
        break;
    }
    case BATTLE_ALMOST_HAD_IT: {
        pw_screen_draw_img(&their_sprite, THEIR_NORMAL_X, THEIR_NORMAL_Y);
        s->battle.anim_frame++;
        break;
    }
    case BATTLE_CATCH_STARS: {
        pw_screen_draw_from_eeprom(
            THEIR_NORMAL_X, THEIR_NORMAL_Y+8-s->battle.anim_frame,
            8, 8,
            PW_EEPROM_ADDR_IMG_RADAR_CATCH_EFFECT,
            PW_EEPROM_SIZE_IMG_RADAR_CATCH_EFFECT
        );
        s->battle.anim_frame++;
        break;
    }
    case BATTLE_POKEMON_CAUGHT: {
        break;
    }
    case BATTLE_SWITCH: {
        for(uint8_t i = 0; i < 3; i++) {
            pw_screen_clear_area(20+i*(8+16), SCREEN_HEIGHT-32, 8, 8);
        }
        if(sf->frame) {
            pw_screen_draw_from_eeprom(
                20+s->battle.switch_cursor*(8+16), SCREEN_HEIGHT-32,
                8, 8,
                PW_EEPROM_ADDR_IMG_ARROW_UP_NORMAL,
                PW_EEPROM_SIZE_IMG_ARROW
            );
        } else {
            pw_screen_draw_from_eeprom(
                20+s->battle.switch_cursor*(8+16), SCREEN_HEIGHT-32,
                8, 8,
                PW_EEPROM_ADDR_IMG_ARROW_UP_OFFSET,
                PW_EEPROM_SIZE_IMG_ARROW
            );
        }

        if(s->battle.switch_cursor != s->battle.prev_switch_cursor) {
            pw_screen_draw_from_eeprom(
                0, SCREEN_HEIGHT-16,
                80, 16,
                PW_EEPROM_ADDR_TEXT_POKEMON_NAMES + s->battle.chosen_pokemon*PW_EEPROM_SIZE_TEXT_POKEMON_NAME,
                PW_EEPROM_SIZE_TEXT_POKEMON_NAME
            );
            s->battle.prev_switch_cursor = s->battle.switch_cursor;
        }

        break;
    }
    default: {
        //printf("[ERROR] Unhandled substate draw update: 0x%02x\n", s->battle.current_substate);
        break;
    }

    }

}

/**
 * Battle state input handler
 *
 */
void pw_battle_handle_input(pw_state_t *s, const screen_flags_t *sf, uint8_t b) {
    switch(s->battle.current_substate) {
    case BATTLE_APPEARED: {
        pw_battle_switch_substate(s, BATTLE_CHOOSING);
        break;
    }
    case BATTLE_CHOOSING: {
        switch(b) {
        case BUTTON_L: {
            s->battle.actions &= ~OUR_ACTION_MASK;
            s->battle.actions |= ACTION_ATTACK << OUR_ACTION_OFFSET;
            s->battle.substate_queue_index = 1;
            break;
        }
        case BUTTON_R: {
            s->battle.actions &= ~OUR_ACTION_MASK;
            s->battle.actions |= ACTION_EVADE << OUR_ACTION_OFFSET;
            s->battle.substate_queue_index = 1;
            break;
        }
        case BUTTON_M: {
            pw_battle_switch_substate(s, BATTLE_CATCH_SETUP);
            break;
        }
        }
        break;
    }
    case BATTLE_POKEMON_CAUGHT: {
        pw_battle_switch_substate(s, BATTLE_PROCESS_CAUGHT_POKEMON);
        break;
    }
    case BATTLE_THEY_FLED:
    case BATTLE_WE_LOST: {
        s->battle.current_substate = BATTLE_GO_TO_SPLASH;
        break;
    }
    case BATTLE_SWITCH: {
        switch(b) {
        case BUTTON_L: {
            if(s->battle.switch_cursor == 0) {
                s->battle.current_substate = BATTLE_GO_TO_SPLASH;
            }
            s->battle.switch_cursor = (s->battle.switch_cursor-1)%3;
            break;
        }
        case BUTTON_R: {
            if(s->battle.switch_cursor >= 2) break;
            s->battle.switch_cursor = (s->battle.switch_cursor+1)%3;
            break;
        }
        case BUTTON_M: {
            pokemon_summary_t poke;
            route_info_t ri;
            pw_eeprom_read(
                PW_EEPROM_ADDR_ROUTE_INFO,
                (uint8_t*)(&ri),
                sizeof(ri)
            );
            poke = ri.route_pokemon[s->battle.chosen_pokemon];
            pw_eeprom_write(
                PW_EEPROM_ADDR_CAUGHT_POKEMON_SUMMARY,
                (uint8_t*)(&poke),
                sizeof(poke)
            );
            s->battle.current_substate = BATTLE_GO_TO_SPLASH;
            break;
        }
        }
    }
    default:
        break;
    }

}



