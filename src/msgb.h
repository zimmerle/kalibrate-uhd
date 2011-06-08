#ifndef MSGB_H
#define MSGB_H

#include <stdint.h>

struct session;

enum {
	JM_ADD_FREQ,
	JM_DEL_FREQ,
	JM_REGEN_SIG,
	JM_CLEAR_ALL,
	JM_CLOSE_CONN,
	JM_USRP_SET_GAIN,
	JM_NACK_FREQ,
};

struct msgb {
	uint8_t cmd;
	uint8_t ampl;
	uint8_t band;
	uint16_t arfcn;
	uint16_t secs;
} __attribute__((packed));

#endif /* MSGB_H */
