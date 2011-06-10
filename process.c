#include <unistd.h>

#include "data.h"
#include "tables.h"

static unsigned long ts_pack;
static int ts_pack_shown;

void show_ts_pack(struct ts *ts, uint16_t pid, char *wtf, char *extra, uint8_t *ts_packet) {
	char cw1_dump[8 * 6];
	char cw2_dump[8 * 6];
	if (ts->debug_level >= 4) {
		if (ts_pack_shown)
			return;
		int stype = ts_packet_get_scrambled(ts_packet);
		ts_hex_dump_buf(cw1_dump, 8 * 6, ts->key.cw    , 8, 0);
		ts_hex_dump_buf(cw2_dump, 8 * 6, ts->key.cw + 8, 8, 0);
		fprintf(stderr, "@ %s %s %03x %5ld %7ld | %s   %s | %s\n",
			stype == 0 ? "------" :
			stype == 2 ? "even 0" :
			stype == 3 ? "odd  1" : "??????",
			wtf,
			pid,
			ts_pack, ts_pack * 188,
			cw1_dump, cw2_dump, extra ? extra : wtf);
	}
}

static void dump_ts_pack(struct ts *ts, uint16_t pid, uint8_t *ts_packet) {
	if (pid == 0x010)		show_ts_pack(ts, pid, "nit", NULL, ts_packet);
	else if (pid == 0x11)	show_ts_pack(ts, pid, "sdt", NULL, ts_packet);
	else if (pid == 0x12)	show_ts_pack(ts, pid, "epg", NULL, ts_packet);
	else					show_ts_pack(ts, pid, "---", NULL, ts_packet);
}

static void decode_packet(struct ts *ts, uint8_t *ts_packet) {
	int scramble_idx = ts_packet_get_scrambled(ts_packet);
	if (scramble_idx > 1) {
		if (ts->key.is_valid_cw) {
			// scramble_idx 2 == even key
			// scramble_idx 3 == odd key
			ts_packet_set_not_scrambled(ts_packet);
			uint8_t payload_ofs = ts_packet_get_payload_offset(ts_packet);
			dvbcsa_decrypt(ts->key.csakey[scramble_idx - 2], ts_packet + payload_ofs, 188 - payload_ofs);
		} else {
			// Can't decrypt the packet just make it NULL packet
			if (ts->pid_filter)
				ts_packet_set_pid(ts_packet, 0x1fff);
		}
	}
}

static void decode_buffer(struct ts *ts, uint8_t *data, int data_len) {
	int i;
	int batch_sz = dvbcsa_bs_batch_size(); // 32?
	int even_packets = 0;
	int odd_packets  = 0;
	struct dvbcsa_bs_batch_s even_pcks[batch_sz + 1];
	struct dvbcsa_bs_batch_s odd_pcks [batch_sz + 1];

	// Prepare batch structure
	for (i = 0; i < batch_sz; i++) {
		uint8_t *ts_packet = data + (i * 188);

		int scramble_idx = ts_packet_get_scrambled(ts_packet);
		if (scramble_idx > 1) {
			if (ts->key.is_valid_cw) {
				uint8_t payload_ofs = ts_packet_get_payload_offset(ts_packet);
				if (scramble_idx == 2) { // scramble_idx 2 == even key
					even_pcks[even_packets].data = ts_packet + payload_ofs;
					even_pcks[even_packets].len  = 188 - payload_ofs;
					even_packets++;
				}
				if (scramble_idx == 3) { // scramble_idx 3 == odd key
					odd_pcks[odd_packets].data = ts_packet + payload_ofs;
					odd_pcks[odd_packets].len  = 188 - payload_ofs;
					odd_packets++;
				}
				ts_packet_set_not_scrambled(ts_packet);
			} else {
				if (ts->pid_filter)
					ts_packet_set_pid(ts_packet, 0x1fff);
			}
		}
	}

	// Decode packets
	if (even_packets) {
		even_pcks[even_packets].data = NULL; // Last one...
		dvbcsa_bs_decrypt(ts->key.bs_csakey[0], even_pcks, 184);
	}
	if (odd_packets) {
		odd_pcks[odd_packets].data = NULL; // Last one...
		dvbcsa_bs_decrypt(ts->key.bs_csakey[1], odd_pcks, 184);
	}

	// Fill write buffer
	for (i=0; i<data_len; i += 188) {
		uint8_t *ts_packet = data + i;

		if (!ts->pid_filter) {
			cbuf_fill(ts->write_buf, ts_packet, 188);
		} else {
			uint16_t pid = ts_packet_get_pid(ts_packet);
			if (pidmap_get(&ts->pidmap, pid)) // PAT or allowed PIDs
				cbuf_fill(ts->write_buf, ts_packet, 188);
		}
	}
}

void *decode_thread(void *_ts) {
	struct ts *ts = _ts;
	uint8_t *data;
	int data_size;
	int req_size = 188 * dvbcsa_bs_batch_size();

	while (!ts->decode_stop) {
		data = cbuf_peek(ts->decode_buf, req_size, &data_size);
		if (data_size < req_size) {
			usleep(10000);
			continue;
		}
		data = cbuf_get(ts->decode_buf, req_size, &data_size);
		if (data)
			decode_buffer(ts, data, data_size);
	}

	do { // Flush data
		data = cbuf_get(ts->decode_buf, req_size, &data_size);
		if (data)
			decode_buffer(ts, data, data_size);
	} while(data);

	return NULL;
}

void *write_thread(void *_ts) {
	struct ts *ts = _ts;
	uint8_t *data;
	int data_size;

	while (!ts->write_stop) {
		data_size = 0;
		data = cbuf_peek(ts->write_buf, FRAME_SIZE, &data_size);
		if (data_size < FRAME_SIZE) {
			usleep(5000);
			continue;
		}
		data = cbuf_get (ts->write_buf, FRAME_SIZE, &data_size);
		if (data)
			write(ts->output.fd, data, data_size);
	}

	do { // Flush data
		data = cbuf_get(ts->write_buf, FRAME_SIZE, &data_size);
		if (data)
			write(ts->output.fd, data, data_size);
	} while(data);

	return NULL;
}

void process_packets(struct ts *ts, uint8_t *data, ssize_t data_len) {
	ssize_t i;
	for (i=0; i<data_len; i += 188) {
		uint8_t *ts_packet = data + i;
		uint16_t pid = ts_packet_get_pid(ts_packet);

		ts_pack_shown = 0;

		process_pat(ts, pid, ts_packet);
		process_cat(ts, pid, ts_packet);
		process_pmt(ts, pid, ts_packet);
		process_emm(ts, pid, ts_packet);
		process_ecm(ts, pid, ts_packet);

		if (!ts_pack_shown)
			dump_ts_pack(ts, pid, ts_packet);

		if (ts->threaded) {
			// Add to decode buffer. The decoder thread will handle it
			if (cbuf_fill(ts->decode_buf, ts_packet, 188) != 0) {
				ts_LOGf("Decode buffer is full, waiting...\n");
				cbuf_dump(ts->decode_buf);
				usleep(10000);
			}
		} else {
			decode_packet(ts, ts_packet);
			if (ts->pid_filter) {
				if (pidmap_get(&ts->pidmap, pid)) // PAT or allowed PIDs
					write(ts->output.fd, ts_packet, 188);
			} else {
				write(ts->output.fd, ts_packet, 188);
			}
		}

		ts_pack++;
	}
}