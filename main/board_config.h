/*
 * board_config.h — hardware revision selection
 *
 * V5 is the active board. V4 builds are kept working as an afterthought;
 * comment BOARD_V5 and uncomment BOARD_V4 to build V4 firmware.
 */

#ifndef MAIN_BOARD_CONFIG_H_
#define MAIN_BOARD_CONFIG_H_

#define BOARD_V5
// #define BOARD_V4

#if defined(BOARD_V5) == defined(BOARD_V4)
#error "Define exactly one of BOARD_V5 or BOARD_V4 in board_config.h"
#endif

#endif /* MAIN_BOARD_CONFIG_H_ */
