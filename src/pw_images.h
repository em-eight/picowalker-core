#ifndef PW_IMAGES_H
#define PW_IMAGES_H

#include <stddef.h>
#include <stdint.h>
#include "pwroms.h"

typedef struct {
    size_t height, width;
    uint8_t *data;
    size_t size;
} pw_img_t;


extern pw_img_t text_mm_dowsing;
extern pw_img_t text_mm_pokeradar;
extern pw_img_t text_mm_connect;
extern pw_img_t text_mm_trainer;
extern pw_img_t text_mm_inventory;
extern pw_img_t text_mm_settings;

extern pw_img_t icon_mm_pokeradar;
extern pw_img_t icon_mm_dowsing;
extern pw_img_t icon_mm_connect;
extern pw_img_t icon_mm_trainer;
extern pw_img_t icon_mm_inventory;
extern pw_img_t icon_mm_settings;

extern pw_img_t text_characters[];
extern pw_img_t text_character_colon;
extern pw_img_t text_character_dash;
extern pw_img_t text_character_slash;

extern pw_img_t img_route;

extern pw_img_t img_pokemon_small_frame1;
extern pw_img_t img_pokemon_small_frame2;
extern pw_img_t img_pokemon_large_frame1;
extern pw_img_t img_pokemon_large_frame2;
extern pw_img_t text_pokemon_name;

extern pw_img_t icon_mm_larrow;
extern pw_img_t icon_mm_rarrow;
extern pw_img_t icon_mm_return;

extern pw_img_t icon_arrow_up_normal;
extern pw_img_t icon_arrow_up_offset;
extern pw_img_t icon_arrow_up_inverted;
extern pw_img_t icon_arrow_down_normal;
extern pw_img_t icon_arrow_down_offset;
extern pw_img_t icon_arrow_down_inverted;
extern pw_img_t icon_arrow_left_normal;
extern pw_img_t icon_arrow_left_offset;
extern pw_img_t icon_arrow_left_inverted;
extern pw_img_t icon_arrow_right_normal;
extern pw_img_t icon_arrow_right_offset;
extern pw_img_t icon_arrow_right_inverted;

extern pw_img_t icon_watt_symbol;

#endif /* PW_IMAGES_H */
