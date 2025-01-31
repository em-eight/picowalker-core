#ifndef PW_EEPROM_H
#define PW_EEPROM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/// @file eeprom.h

typedef uint16_t eeprom_addr_t;

/*
 *  Functions defined by the driver
 */
extern void pw_eeprom_init();
extern int pw_eeprom_read(eeprom_addr_t addr, uint8_t *buf, size_t len);
extern int pw_eeprom_write(eeprom_addr_t addr, uint8_t *buf, size_t len);
extern void pw_eeprom_set_area(eeprom_addr_t addr, uint8_t v, size_t len);


/*
 *  Derivative functions, driver agnostic
 */
int pw_eeprom_reliable_read(eeprom_addr_t addr1, eeprom_addr_t addr2, uint8_t *buf, size_t len);
int pw_eeprom_reliable_write(eeprom_addr_t addr1, eeprom_addr_t addr2, uint8_t *buf, size_t len);
uint8_t pw_eeprom_checksum(uint8_t *buf, size_t len);
bool pw_eeprom_check_for_nintendo();
void pw_eeprom_reset(bool clear_events, bool clear_steps);
void pw_eeprom_initialise_health_data(bool clear_time);

#endif /* PW_EEPROM_H */
