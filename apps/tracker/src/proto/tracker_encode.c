/*
 * Generated using zcbor version 0.9.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 24
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"
#include "tracker_encode.h"
#include "zcbor_print.h"

#if DEFAULT_MAX_QTY != 24
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

static bool encode_gps_entry(zcbor_state_t *state, const struct gps_entry *input);
static bool encode_cell_entry(zcbor_state_t *state, const struct cell_entry *input);
static bool encode_repeat_entry(zcbor_state_t *state, const struct repeat_entry *input);
static bool encode_entry(zcbor_state_t *state, const struct entry_r *input);
static bool encode_batch(zcbor_state_t *state, const struct batch *input);


static bool encode_gps_entry(
		zcbor_state_t *state, const struct gps_entry *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_map_start_encode(state, 8) && (((((zcbor_uint32_put(state, (16))))
	&& ((((((*input).gps_entry_age_s_m <= 7200)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).gps_entry_age_s_m))))
	&& (((zcbor_uint32_put(state, (2))))
	&& ((((((*input).gps_entry_lat_e7_m >= -900000000)
	&& ((*input).gps_entry_lat_e7_m <= 900000000)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_int32_encode(state, (&(*input).gps_entry_lat_e7_m))))
	&& (((zcbor_uint32_put(state, (3))))
	&& ((((((*input).gps_entry_lon_e7_m >= -1800000000)
	&& ((*input).gps_entry_lon_e7_m <= 1800000000)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_int32_encode(state, (&(*input).gps_entry_lon_e7_m))))
	&& (((zcbor_uint32_put(state, (4))))
	&& ((((((*input).gps_entry_alt_dm_m >= -100000)
	&& ((*input).gps_entry_alt_dm_m <= 1000000)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_int32_encode(state, (&(*input).gps_entry_alt_dm_m))))
	&& (((zcbor_uint32_put(state, (5))))
	&& ((((((*input).gps_entry_acc_dm_m <= 1000000)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).gps_entry_acc_dm_m))))
	&& (((zcbor_uint32_put(state, (6))))
	&& ((((((*input).gps_entry_spd_dms_m <= 20000)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).gps_entry_spd_dms_m))))
	&& (((zcbor_uint32_put(state, (7))))
	&& ((((((*input).gps_entry_hdg_ddeg_m <= 3600)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).gps_entry_hdg_ddeg_m))))
	&& (((zcbor_uint32_put(state, (8))))
	&& ((((((*input).gps_entry_sats_m <= 64)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).gps_entry_sats_m))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 8))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_cell_entry(
		zcbor_state_t *state, const struct cell_entry *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_map_start_encode(state, 6) && (((((zcbor_uint32_put(state, (16))))
	&& ((((((*input).cell_entry_age_s_m <= 7200)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).cell_entry_age_s_m))))
	&& (((zcbor_uint32_put(state, (10))))
	&& ((((((*input).cell_entry_mcc_m <= 999)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).cell_entry_mcc_m))))
	&& (((zcbor_uint32_put(state, (11))))
	&& ((((((*input).cell_entry_mnc_m <= 999)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).cell_entry_mnc_m))))
	&& (((zcbor_uint32_put(state, (12))))
	&& ((((((*input).cell_entry_tac_m <= UINT16_MAX)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).cell_entry_tac_m))))
	&& (((zcbor_uint32_put(state, (13))))
	&& ((((((*input).cell_entry_cell_id_m <= 268435455)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).cell_entry_cell_id_m))))
	&& (((zcbor_uint32_put(state, (14))))
	&& ((((((*input).cell_entry_rsrp_dbm_m >= -160)
	&& ((*input).cell_entry_rsrp_dbm_m <= 0)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_int32_encode(state, (&(*input).cell_entry_rsrp_dbm_m))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 6))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_repeat_entry(
		zcbor_state_t *state, const struct repeat_entry *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_map_start_encode(state, 1) && (((((zcbor_uint32_put(state, (16))))
	&& ((((((*input).repeat_entry_age_s_m <= 7200)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_uint32_encode(state, (&(*input).repeat_entry_age_s_m))))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 1))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_entry(
		zcbor_state_t *state, const struct entry_r *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((((*input).entry_choice == gps_entry_m_c) ? ((encode_gps_entry(state, (&(*input).gps_entry_m))))
	: (((*input).entry_choice == cell_entry_m_c) ? ((encode_cell_entry(state, (&(*input).cell_entry_m))))
	: (((*input).entry_choice == repeat_entry_m_c) ? ((encode_repeat_entry(state, (&(*input).repeat_entry_m))))
	: false)))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_batch(
		zcbor_state_t *state, const struct batch *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_map_start_encode(state, 3) && (((((zcbor_uint32_put(state, (0))))
	&& (zcbor_uint32_put(state, (2))))
	&& (((zcbor_uint32_put(state, (1))))
	&& ((((((*input).batch_device_id_m.len >= 1)
	&& ((*input).batch_device_id_m.len <= 20)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
	&& (zcbor_tstr_encode(state, (&(*input).batch_device_id_m))))
	&& (((zcbor_uint32_put(state, (15))))
	&& (zcbor_list_start_encode(state, 24) && ((zcbor_multi_encode_minmax(1, 24, &(*input).batch_entry_m_l_entry_m_count, (zcbor_encoder_t *)encode_entry, state, (*&(*input).batch_entry_m_l_entry_m), sizeof(struct entry_r))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 24)))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_map_end_encode(state, 3))));

	log_result(state, res, __func__);
	return res;
}



int cbor_encode_batch(
		uint8_t *payload, size_t payload_len,
		const struct batch *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[6];

	return zcbor_entry_function(payload, payload_len, (void *)input, payload_len_out, states,
		(zcbor_decoder_t *)encode_batch, sizeof(states) / sizeof(zcbor_state_t), 1);
}
