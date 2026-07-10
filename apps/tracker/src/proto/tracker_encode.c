/*
 * Generated using zcbor version 0.9.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"
#include "tracker_encode.h"
#include "zcbor_print.h"

#if DEFAULT_MAX_QTY != 3
#error "The type file was generated with a different default_max_qty than this file"
#endif

#define log_result(state, result, func) do { \
	if (!result) { \
		zcbor_trace_file(state); \
		zcbor_log("%s error: %s\r\n", func, zcbor_error_str(zcbor_peek_error(state))); \
	} else { \
		zcbor_log("%s success\r\n", func); \
	} \
} while(0)

static bool encode_cell_obs(zcbor_state_t *state, const struct cell_obs *input);
static bool encode_gps_obs(zcbor_state_t *state, const struct gps_obs *input);


static bool encode_cell_obs(
		zcbor_state_t *state, const struct cell_obs *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_map_start_encode(state, 7) && (((((zcbor_uint32_put(state, (0))))
	&& (zcbor_uint32_put(state, (1))))
	&& (((zcbor_uint32_put(state, (1))))
	&& ((((((*input).cell_obs_device_id_m.len >= 1)
	&& ((*input).cell_obs_device_id_m.len <= 20)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_tstr_encode(state, (&(*input).cell_obs_device_id_m))))
	&& (((zcbor_uint32_put(state, (10))))
	&& ((((((*input).cell_obs_mcc_m <= 999)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).cell_obs_mcc_m))))
	&& (((zcbor_uint32_put(state, (11))))
	&& ((((((*input).cell_obs_mnc_m <= 999)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).cell_obs_mnc_m))))
	&& (((zcbor_uint32_put(state, (12))))
	&& ((((((*input).cell_obs_tac_m <= UINT16_MAX)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).cell_obs_tac_m))))
	&& (((zcbor_uint32_put(state, (13))))
	&& ((((((*input).cell_obs_cell_id_m <= 268435455)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).cell_obs_cell_id_m))))
	&& (((zcbor_uint32_put(state, (14))))
	&& ((((((*input).cell_obs_rsrp_dbm_m >= -160)
	&& ((*input).cell_obs_rsrp_dbm_m <= 0)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_int32_encode(state, (&(*input).cell_obs_rsrp_dbm_m))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 7))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_gps_obs(
		zcbor_state_t *state, const struct gps_obs *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_map_start_encode(state, 10) && (((((zcbor_uint32_put(state, (0))))
	&& (zcbor_uint32_put(state, (1))))
	&& (((zcbor_uint32_put(state, (1))))
	&& ((((((*input).gps_obs_device_id_m.len >= 1)
	&& ((*input).gps_obs_device_id_m.len <= 20)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_tstr_encode(state, (&(*input).gps_obs_device_id_m))))
	&& (((zcbor_uint32_put(state, (2))))
	&& ((((((*input).gps_obs_lat_e7_m >= -900000000)
	&& ((*input).gps_obs_lat_e7_m <= 900000000)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_int32_encode(state, (&(*input).gps_obs_lat_e7_m))))
	&& (((zcbor_uint32_put(state, (3))))
	&& ((((((*input).gps_obs_lon_e7_m >= -1800000000)
	&& ((*input).gps_obs_lon_e7_m <= 1800000000)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_int32_encode(state, (&(*input).gps_obs_lon_e7_m))))
	&& (((zcbor_uint32_put(state, (4))))
	&& ((((((*input).gps_obs_alt_dm_m >= -100000)
	&& ((*input).gps_obs_alt_dm_m <= 1000000)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_int32_encode(state, (&(*input).gps_obs_alt_dm_m))))
	&& (((zcbor_uint32_put(state, (5))))
	&& ((((((*input).gps_obs_acc_dm_m <= 1000000)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).gps_obs_acc_dm_m))))
	&& (((zcbor_uint32_put(state, (6))))
	&& ((((((*input).gps_obs_spd_dms_m <= 20000)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).gps_obs_spd_dms_m))))
	&& (((zcbor_uint32_put(state, (7))))
	&& ((((((*input).gps_obs_hdg_ddeg_m <= 3600)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).gps_obs_hdg_ddeg_m))))
	&& (((zcbor_uint32_put(state, (8))))
	&& ((((((*input).gps_obs_sats_m <= 64)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).gps_obs_sats_m))))
	&& (((zcbor_uint32_put(state, (9))))
	&& ((((((*input).gps_obs_fix_age_s_m <= 3600)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).gps_obs_fix_age_s_m))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 10))));

	log_result(state, res, __func__);
	return res;
}



int cbor_encode_gps_obs(
		uint8_t *payload, size_t payload_len,
		const struct gps_obs *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[3];

	return zcbor_entry_function(payload, payload_len, (void *)input, payload_len_out, states,
		(zcbor_decoder_t *)encode_gps_obs, sizeof(states) / sizeof(zcbor_state_t), 1);
}


int cbor_encode_cell_obs(
		uint8_t *payload, size_t payload_len,
		const struct cell_obs *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[3];

	return zcbor_entry_function(payload, payload_len, (void *)input, payload_len_out, states,
		(zcbor_decoder_t *)encode_cell_obs, sizeof(states) / sizeof(zcbor_state_t), 1);
}
