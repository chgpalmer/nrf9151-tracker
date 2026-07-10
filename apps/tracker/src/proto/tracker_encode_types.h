/*
 * Generated using zcbor version 0.9.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
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
#define DEFAULT_MAX_QTY 3

struct gps_obs {
	struct zcbor_string gps_obs_device_id_m;
	int32_t gps_obs_lat_e7_m;
	int32_t gps_obs_lon_e7_m;
	int32_t gps_obs_alt_dm_m;
	uint32_t gps_obs_acc_dm_m;
	uint32_t gps_obs_spd_dms_m;
	uint32_t gps_obs_hdg_ddeg_m;
	uint32_t gps_obs_sats_m;
	uint32_t gps_obs_fix_age_s_m;
};

struct cell_obs {
	struct zcbor_string cell_obs_device_id_m;
	uint32_t cell_obs_mcc_m;
	uint32_t cell_obs_mnc_m;
	uint32_t cell_obs_tac_m;
	uint32_t cell_obs_cell_id_m;
	int32_t cell_obs_rsrp_dbm_m;
};

#ifdef __cplusplus
}
#endif

#endif /* TRACKER_ENCODE_TYPES_H__ */
