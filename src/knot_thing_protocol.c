/*
 * Copyright (c) 2016, CESAR.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 *
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <avr_errno.h>
#include <avr_unistd.h>

#include "knot_thing_protocol.h"
#include "storage.h"
#include "comm.h"

/*KNoT client storage mapping */
#define KNOT_UUID_FLAG_ADDR		0
#define KNOT_UUID_FLAG_LEN		1
#define KNOT_UUID_ADDR			(KNOT_UUID_FLAG_ADDR + KNOT_UUID_FLAG_LEN)
#define KNOT_TOKEN_FLAG_ADDR		(KNOT_UUID_ADDR + KNOT_PROTOCOL_UUID_LEN)
#define KNOT_TOKEN_FLAG_LEN		1
#define KNOT_TOKEN_ADDR			(KNOT_TOKEN_FLAG_ADDR + KNOT_TOKEN_FLAG_LEN)

/* KNoT protocol client states */
#define STATE_DISCONNECTED		0
#define STATE_CONNECTING		1
#define STATE_AUTHENTICATING		2
#define STATE_REGISTERING		3
#define STATE_SCHEMA			4
#define STATE_SCHEMA_RESP		5
#define STATE_ONLINE			6
#define STATE_ERROR			7
#define STATE_MAX			(STATE_ERROR+1)

#ifndef MIN
#define MIN(a,b)			(((a) < (b)) ? (a) : (b))
#endif

static uint8_t enable_run = 0, schema_sensor_id = 0;
static char uuid[KNOT_PROTOCOL_UUID_LEN];
static char token[KNOT_PROTOCOL_TOKEN_LEN];
static char device_name[KNOT_PROTOCOL_DEVICE_NAME_LEN];
static schema_function schemaf;
static data_function thing_read;
static data_function thing_write;
static config_function configf;
static int sock = -1;
static events_function eventf;

int knot_thing_protocol_init(const char *thing_name, data_function read,
	data_function write, schema_function schema, config_function config,
							events_function event)
{
	int len;
	if (hal_comm_init("NRF0") < 0)
		return -1;

	sock = hal_comm_socket(HAL_COMM_PF_NRF24, HAL_COMM_PROTO_RAW);
	if (sock < 0)
		return -1;
	memset(device_name, 0, sizeof(device_name));

	len = MIN(strlen(thing_name), sizeof(device_name) - 1);
	strncpy(device_name, thing_name, len);
	enable_run = 1;
	schemaf = schema;
	thing_read = read;
	thing_write = write;
	config = config;
	eventf = event;
}

void knot_thing_protocol_exit(void)
{
	hal_comm_close(sock);
	enable_run = 0;
}

static int send_register(void)
{
	ssize_t nbytes;
	knot_msg_register msg;
	int len;

	memset(&msg, 0, sizeof(msg));
	len = MIN(sizeof(msg.devName), strlen(device_name));

	msg.hdr.type = KNOT_MSG_REGISTER_REQ;
	strncpy(msg.devName, device_name, len);
	msg.hdr.payload_len = len;
	/* FIXME: Open socket */
	nbytes = hal_comm_write(-1, &msg, sizeof(msg.hdr) + len);
	if (nbytes < 0) {
		return -1;
	}

	return 0;
}

static int read_register(void)
{
	ssize_t nbytes;
	knot_msg_credential crdntl;
	const uint8_t buffer[] = { 0x01 };

	memset(&crdntl, 0, sizeof(crdntl));
	/* FIXME: Open socket */
	nbytes = hal_comm_read(-1, &crdntl, sizeof(crdntl));

	if (nbytes == -EAGAIN)
		return -EAGAIN;

	if (nbytes > 0) {
		if (crdntl.result != KNOT_SUCCESS)
			return -1;

		hal_storage_write(KNOT_UUID_ADDR, crdntl.uuid,
						KNOT_PROTOCOL_UUID_LEN);
		hal_storage_write(KNOT_TOKEN_ADDR, crdntl.token,
						KNOT_PROTOCOL_TOKEN_LEN);

		hal_storage_write(KNOT_UUID_FLAG_ADDR, buffer, KNOT_UUID_FLAG_LEN);
		hal_storage_write(KNOT_TOKEN_FLAG_ADDR, buffer, KNOT_TOKEN_FLAG_LEN);
	} else if (nbytes < 0)
		return nbytes;

	return 0;
}

static int send_auth(void)
{
	knot_msg_authentication msg;
	knot_msg_result resp;
	ssize_t nbytes;

	memset(&msg, 0, sizeof(msg));

	msg.hdr.type = KNOT_MSG_AUTH_REQ;
	msg.hdr.payload_len = sizeof(msg.uuid) + sizeof(msg.token);

	strncpy(msg.uuid, uuid, sizeof(msg.uuid));
	strncpy(msg.token, token, sizeof(msg.token));

	nbytes = hal_comm_write(-1, &msg, sizeof(msg.hdr) + msg.hdr.payload_len);
	if (nbytes < 0)
		return -1;

	return 0;
}

static int read_auth(void)
{
	knot_msg_result resp;
	ssize_t nbytes;

	memset(&resp, 0, sizeof(resp));

	nbytes = hal_comm_read(-1, &resp, sizeof(resp));

	if (nbytes == -EAGAIN)
		return -EAGAIN;

	if (nbytes > 0) {
		if (resp.result != KNOT_SUCCESS)
			return -1;

	} else if (nbytes < 0)
		return nbytes;

	return 0;
}

static int send_schema(void)
{
	int err;
	knot_msg_schema msg;
	ssize_t nbytes;

	memset(&msg, 0, sizeof(msg));
	err = schemaf(schema_sensor_id, &msg);

	if (err < 0)
		return err;

	if (msg.hdr.type == KNOT_MSG_SCHEMA_FLAG_END)
		schema_sensor_id = 0;

	nbytes = hal_comm_write(-1, &msg, sizeof(msg.hdr) +
							msg.hdr.payload_len);
	if (nbytes < 0)
		return -1;

	return 0;
}

static int config(knot_msg_config *config)
{
	int err;
	knot_msg_result resp;
	ssize_t nbytes;

	err = configf(config->sensor_id, config->values.event_flags,
						&config->values.lower_limit,
						&config->values.upper_limit);

	memset(&resp, 0, sizeof(resp));

	resp.result = KNOT_SUCCESS;
	if (err < 0)
		resp.result = KNOT_ERROR_UNKNOWN;


	resp.hdr.type = KNOT_MSG_CONFIG_RESP;
	resp.hdr.payload_len = sizeof(resp.result);

	nbytes = hal_comm_write(-1, &resp, sizeof(resp.hdr) + resp.result);
	if (nbytes < 0)
		return -1;

	return 0;
}

static int set_data(knot_msg_data *data)
{
	int err;
	ssize_t nbytes;
	knot_msg_result resp;

	err = thing_write(data->sensor_id, data);

	resp.result = KNOT_SUCCESS;
	if (err < 0)
		resp.result = KNOT_ERROR_UNKNOWN;

	memset(&resp, 0, sizeof(resp));

	resp.hdr.type = KNOT_MSG_DATA_RESP;
	resp.hdr.payload_len = sizeof(resp.result);

	nbytes = hal_comm_write(-1, &resp, sizeof(resp.hdr) + resp.result);
	if (nbytes < 0)
		return -1;

	return 0;
}

static int get_data(knot_msg_data *data)
{
	int err;
	knot_msg_data msg;
	knot_msg_data data_resp;
	ssize_t nbytes;

	memset(&data_resp, 0, sizeof(data_resp));
	err = thing_read(data->sensor_id, &data_resp);

	memset(&msg, 0, sizeof(msg));
	msg.hdr.type = KNOT_MSG_DATA;
	if (err < 0)
		msg.hdr.type = KNOT_ERROR_UNKNOWN;

	msg.sensor_id = data->sensor_id;
	memcpy(&msg.payload, &data_resp, sizeof(data_resp));

	nbytes = hal_comm_write(-1, &msg, sizeof(msg.hdr) +
							msg.hdr.payload_len);
	if (nbytes < 0)
		return -1;

	return 0;
}

int knot_thing_protocol_run(void)
{
	static uint8_t state = STATE_DISCONNECTED;
	uint8_t uuid_flag = 0, token_flag = 0;
	int retval = 0;
	size_t ilen;
	knot_msg kreq;

	if (enable_run == 0)
		return -1;

	/* Network message handling state machine */
	switch (state) {
	case STATE_DISCONNECTED:
		//TODO: call hal_comm_connect()
		state = STATE_CONNECTING;
	break;

	case STATE_CONNECTING:
		//TODO: verify connection status, if not connected,
		//	goto STATE_ERROR
		/*
		 * uuid/token flags indicate wheter they are
		 * stored in EEPROM or not
		 */
		hal_storage_read(KNOT_UUID_FLAG_ADDR, &uuid_flag,
						KNOT_UUID_FLAG_LEN);
		hal_storage_read(KNOT_TOKEN_FLAG_ADDR, &token_flag,
						KNOT_TOKEN_FLAG_LEN);
		/*
		 * If flag was found then we read the addresses and send
		 * the auth request, otherwise register request
		 */
		if(uuid_flag && token_flag) {
			hal_storage_read(KNOT_UUID_ADDR, uuid,
						KNOT_PROTOCOL_UUID_LEN);
			hal_storage_read(KNOT_TOKEN_ADDR, token,
					KNOT_PROTOCOL_TOKEN_LEN);

			if (send_auth() < 0)
				state = STATE_ERROR;
			else
				state = STATE_AUTHENTICATING;
		} else {
			if (send_register() < 0)
				state = STATE_ERROR;
			else
				state = STATE_REGISTERING;
		}
	break;
	/*
	 * Authenticating, Resgistering cases waits (without blocking)
	 * for an response of the respective requests, -EAGAIN means there was
	 * nothing to read so we ignore it, less then 0 an error and 0 success
	 */
	case STATE_AUTHENTICATING:
		retval = read_auth();
		if (retval < 0)
			state = STATE_ERROR;
		else if (retval == 0)
			state = STATE_SCHEMA;
	break;

	case STATE_REGISTERING:
		retval = read_register();
		if (retval < 0)
			state = STATE_ERROR;
		else if (retval == 0)
			state = STATE_SCHEMA;
	break;

	case STATE_SCHEMA:
		/*
		 * FIXME: Currently we are sending an schema for each sensor
		 * individually without receiving a response from the GW.
		 * We need to send the next schema only after
		 * receiving a confirmation from the GW and change to ONLINE
		 * only after the last confirmation was received
		 */
		retval = send_schema();
		schema_sensor_id++;
		if (retval < 0)
			state = STATE_ERROR;
		else if (retval > 0)
			state = STATE_ONLINE;
	break;

	case STATE_SCHEMA_RESP:
		/* TODO: Handle schema responses */
	break;

	case STATE_ONLINE:
		/* FIXME: Open socket first */
		ilen = hal_comm_read(-1, &kreq, sizeof(kreq));
		if (ilen > 0) {
			/* There is config or set data */
			switch (kreq.hdr.type) {
			case KNOT_MSG_CONFIG:
				config(&kreq.config);
				break;
			case KNOT_MSG_SET_DATA:
				set_data(&kreq.data);
				break;
			case KNOT_MSG_GET_DATA:
				get_data(&kreq.data);
				break;
			default:
				/* Invalid command */
				break;
			}
		}
		/*
		 * TODO: Send messages according to the events:
		 * Each iteration increment the index, call
		 * verify_events(knot_msg_data) and use comm_write to
		 * send possible data to the gateway
		 */

	break;

	case STATE_ERROR:
		//TODO: log error
		//TODO: close connection if needed
		//TODO: wait 1s
		state = STATE_DISCONNECTED;
	break;

	default:
		//TODO: log invalid state
		//TODO: close connection if needed
		state = STATE_DISCONNECTED;
	break;
	}

	return 0;
}


