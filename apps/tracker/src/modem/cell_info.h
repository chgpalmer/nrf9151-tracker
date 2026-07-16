/*
 * Serving-cell identity, parsed from AT%XMONITOR (cached registration data —
 * no radio scan). Enough for an OpenCelliD single-cell lookup.
 */
#ifndef CELL_INFO_H__
#define CELL_INFO_H__

#include <stdbool.h>
#include <stdint.h>

struct serving_cell {
	int      mcc;
	int      mnc;
	uint32_t tac;
	uint32_t cid;
	int      rsrp_dbm;
	int      act;  /* 3GPP AcT: 7 = LTE-M, 9 = NB-IoT */
	int      band;
	char     op[32];
	bool     valid;
};

/* Read the serving cell from AT%XMONITOR. Fills sc; sc->valid says whether
 * the modem had registration data to give. Main-loop context only (the
 * parser uses strtok). */
int cell_info_read(struct serving_cell *sc);

#endif /* CELL_INFO_H__ */
