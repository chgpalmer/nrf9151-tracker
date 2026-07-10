/*
 * Generated using zcbor version 0.9.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 24
 */

#ifndef TRACKER_ENCODE_TYPES_H__
#define TRACKER_ENCODE_TYPES_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zcbor_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Which value for --default-max-qty this file was created with.
 *
 *  The define is used in the other generated file to do a build-time
 *  compatibility check.
 *
 *  See `zcbor --help` for more information about --default-max-qty
 */
#define DEFAULT_MAX_QTY 24

struct gps_entry {
	uint32_t gps_entry_age_s_m;
	int32_t gps_entry_lat_e7_m;
	int32_t gps_entry_lon_e7_m;
	int32_t gps_entry_alt_dm_m;
	uint32_t gps_entry_acc_dm_m;
	uint32_t gps_entry_spd_dms_m;
	uint32_t gps_entry_hdg_ddeg_m;
	uint32_t gps_entry_sats_m;
};

struct cell_entry {
	uint32_t cell_entry_age_s_m;
	uint32_t cell_entry_mcc_m;
	uint32_t cell_entry_mnc_m;
	uint32_t cell_entry_tac_m;
	uint32_t cell_entry_cell_id_m;
	int32_t cell_entry_rsrp_dbm_m;
};

struct repeat_entry {
	uint32_t repeat_entry_age_s_m;
};

struct entry_r {
	union {
		struct gps_entry gps_entry_m;
		struct cell_entry cell_entry_m;
		struct repeat_entry repeat_entry_m;
	};
	enum {
		gps_entry_m_c,
		cell_entry_m_c,
		repeat_entry_m_c,
	} entry_choice;
};

struct batch {
	struct zcbor_string batch_device_id_m;
	struct entry_r batch_entry_m_l_entry_m[24];
	size_t batch_entry_m_l_entry_m_count;
};

#ifdef __cplusplus
}
#endif

#endif /* TRACKER_ENCODE_TYPES_H__ */
