#include "cell_info.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <nrf_modem_at.h>

/* RSRP index → dBm (same formula as modem_info.h RSRP_IDX_TO_DBM). */
#define RSRP_IDX_TO_DBM(idx) ((idx) < 0 ? (idx) - 140 : (idx) - 141)

/* Helper: extract the next comma-delimited XMONITOR field, stripping the
 * surrounding quotes if present. Returns the token or NULL. Uses strtok state. */
static char *xmon_field(char *first)
{
	char *f = strtok(first, ",");

	if (f && f[0] == '"') {
		f++;
		char *e = strchr(f, '"');

		if (e) {
			*e = '\0';
		}
	}
	return f;
}

/* XMONITOR: <reg>,"<long>","<short>","<plmn>","<tac>",<AcT>,<band>,
 *           "<cell_id>",<phys_cell_id>,<EARFCN>,<rsrp>,<snr>,...
 * PLMN is "MCCMNC" (MCC=first 3 digits); TAC and cell-id are hex strings. */
int cell_info_read(struct serving_cell *sc)
{
	char at_resp[256];

	memset(sc, 0, sizeof(*sc));

	if (nrf_modem_at_cmd(at_resp, sizeof(at_resp), "AT%%XMONITOR") != 0) {
		return -EIO;
	}

	char *cp = strchr(at_resp, ':');

	if (!cp) {
		return -EINVAL;
	}
	cp++;

	xmon_field(cp);                    /* 1: reg status */
	char *longn = xmon_field(NULL);    /* 2: long name  */
	xmon_field(NULL);                  /* 3: short name */
	char *plmn = xmon_field(NULL);     /* 4: plmn "MCCMNC" */
	char *tac  = xmon_field(NULL);     /* 5: tac (hex)  */
	char *act  = xmon_field(NULL);     /* 6: AcT (7=LTE-M, 9=NB-IoT) */
	char *band = xmon_field(NULL);     /* 7: band       */
	char *cid  = xmon_field(NULL);     /* 8: cell id (hex) */
	xmon_field(NULL);                  /* 9: phys cell  */
	xmon_field(NULL);                  /* 10: EARFCN    */
	char *rsrp = strtok(NULL, ",\r\n");/* 11: rsrp idx  */

	if (!plmn || strlen(plmn) < 5 || !tac || !cid) {
		return -ENODATA;
	}

	/* Split PLMN: MCC = first 3 chars, MNC = the rest (2 or 3 digits). */
	char mcc_buf[4] = { plmn[0], plmn[1], plmn[2], '\0' };

	sc->mcc = atoi(mcc_buf);
	sc->mnc = atoi(plmn + 3);
	sc->tac = strtoul(tac, NULL, 16);
	sc->cid = strtoul(cid, NULL, 16);
	sc->act = act ? atoi(act) : 0;
	sc->band = band ? atoi(band) : 0;
	sc->rsrp_dbm = rsrp ? RSRP_IDX_TO_DBM(atoi(rsrp)) : 0;
	if (longn) {
		strncpy(sc->op, longn, sizeof(sc->op) - 1);
	}
	sc->valid = true;
	return 0;
}
