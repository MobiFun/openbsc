/* Test SLHC/RFC1144 TCP/IP Header compression/decompression */

/* (C) 2016 by sysmocom s.f.m.c. GmbH <info@sysmocom.de>
 * All Rights Reserved
 *
 * Author: Philipp Maier
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <openbsc/slhc.h>
#include <openbsc/debug.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>

#include <osmocom/core/application.h>

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

/* Number of compression slots (S0-1) */
#define SLOTS 8

/* Maximum packet bytes to display */
#define DISP_MAX_BYTES 100

/* Sample packets to test with */
#define PACKETS_LEN 6
char *packets[] = {
	"4510004046dd40004006a9a7c0a8646ec0a864640017ad8b81980100f3ac984d801800e32a1600000101080a000647de06d1bf5efffd18fffd20fffd23fffd27",
	"4510005b46de40004006a98bc0a8646ec0a864640017ad8b8198010cf3ac984d801800e3867500000101080a000647df06d1bf61fffb03fffd1ffffd21fffe22fffb05fffa2001fff0fffa2301fff0fffa2701fff0fffa1801fff0",
	"4510003746df40004006a9aec0a8646ec0a864640017ad8b81980133f3ac989f801800e35fd700000101080a000647e106d1bf63fffd01",
	"4510003746e040004006a9adc0a8646ec0a864640017ad8b81980136f3ac98a2801800e35fd200000101080a000647e106d1bf64fffb01",
	"4510007446e140004006a96fc0a8646ec0a864640017ad8b81980139f3ac98a5801800e37b9b00000101080a000647e206d1bf640d0a2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d0d0a57656c6c636f6d6520746f20706f6c6c75780d0a2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d2d0d0a0d0a",
	"4510004246e240004006a9a0c0a8646ec0a864640017ad8b81980179f3ac98a5801800e3dab000000101080a000647ec06d1bf6f706f6c6c7578206c6f67696e3a20"
};

/* Compress a packet using Van Jacobson RFC1144 header compression */
static int compress(uint8_t *data_o, uint8_t *data_i, int len,
		    struct slcompress *comp)
{
	uint8_t *comp_ptr;	/* Not used */
	int compr_len;

	/* Create a working copy of the incoming data */
	memcpy(data_o, data_i, len);

	/* Run compressor */
	compr_len = slhc_compress(comp, data_i, len, data_o, &comp_ptr, 0);
	return compr_len;
}

/* Expand a packet using Van Jacobson RFC1144 header compression */
static int expand(uint8_t *data_o, uint8_t *data_i, int len,
		  struct slcompress *comp)
{
	int data_decompressed_len;

	/* Create a working copy of the incoming data */
	memcpy(data_o, data_i, len);

	/* Handle an uncompressed packet (learn header information */
	if ((data_i[0] & SL_TYPE_UNCOMPRESSED_TCP) == SL_TYPE_UNCOMPRESSED_TCP) {
		data_o[0] &= 0x4F;
		data_decompressed_len = slhc_remember(comp, data_o, len);
		return data_decompressed_len;
	}

	/* Uncompress compressed packets */
	else if (data_o[0] & SL_TYPE_COMPRESSED_TCP) {
		data_decompressed_len = slhc_uncompress(comp, data_o, len);
		return data_decompressed_len;
	}

	/* Regular or unknown packets will not be touched */
	return len;
}

/* Calculate IP Header checksum */
static uint16_t calc_ip_csum(uint8_t *data, int len)
{
	int i;
	uint32_t accumulator = 0;
	uint16_t *pointer = (uint16_t *) data;

	for (i = len; i > 1; i -= 2) {
		accumulator += *pointer;
		pointer++;
	}

	if (len % 2)
		accumulator += *pointer;

	accumulator = (accumulator & 0xffff) + ((accumulator >> 16) & 0xffff);
	accumulator += (accumulator >> 16) & 0xffff;
	return (~accumulator);
}

/* Calculate TCP/IP checksum */
static uint16_t calc_tcpip_csum(const void *ctx, uint8_t *packet, int len)
{
	uint8_t *buf;
	uint16_t csum;

	buf = talloc_zero_size(ctx, len);
	memset(buf, 0, len);
	memcpy(buf, packet + 12, 8);
	buf[9] = packet[9];
	buf[11] = (len - 20) & 0xFF;
	buf[10] = (len - 20) >> 8 & 0xFF;
	memcpy(buf + 12, packet + 20, len - 20);
	csum = calc_ip_csum(buf, len - 20 + 12);
	talloc_free(buf);
	return csum;
}

/* Check TCP/IP packet */
static void check_packet(const void *ctx, uint8_t *packet, int len)
{
	/* Check IP header */
	OSMO_ASSERT(len > 20);
	OSMO_ASSERT(calc_ip_csum(packet, 20) == 0);

	/* Check TCP packet */
	if (packet[9] != 0x06)
		return;
	OSMO_ASSERT(len > 40);
	OSMO_ASSERT(calc_tcpip_csum(ctx, packet, len) == 0);
}

/* Strip TCP options from TCP/IP packet */
static int strip_tcp_options(const void *ctx, uint8_t *packet, int len)
{
	uint8_t doff;
	uint16_t csum;

	/* Check if the packet can be handled here */
	if (len < 37)
		return len;
	if (packet[9] != 0x06)
		return len;

	/* Strip TCP/IP options from packet */
	doff = ((packet[32] >> 4) & 0x0F) * 4;
	memmove(packet + 40, packet + doff + 20, len - 40 - (doff - 20));
	len = len - (doff - 20);

	/* Repair data offset (TCP header length) */
	packet[32] &= 0x0F;
	packet[32] |= 0x50;

	/* Repair checksum */
	packet[36] = 0;
	packet[37] = 0;
	csum = calc_tcpip_csum(ctx, packet, len);
	packet[36] = csum & 0xFF;
	packet[37] = csum >> 8 & 0xFF;

	/* Repair total length */
	packet[3] = len & 0xFF;
	packet[2] = len >> 8 & 0xFF;

	/* Repair IP header checksum */
	packet[10] = 0;
	packet[11] = 0;
	csum = calc_ip_csum(packet, 20);
	packet[10] = csum & 0xFF;
	packet[11] = csum >> 8 & 0xFF;
	printf("csum=%04x\n", csum);

	return len;
}

/* Compress / Decompress packets */
static void test_slhc(const void *ctx)
{
	char packet_ascii[2048];
	int i;

	struct slcompress *comp;
	uint8_t packet[1024];
	int packet_len;
	uint8_t packet_compr[1024];
	int packet_compr_len;
	uint8_t packet_decompr[1024];
	int packet_decompr_len;

	printf("Allocating compression state...\n");
	comp = slhc_init(ctx, SLOTS, SLOTS);
	OSMO_ASSERT(comp);

	for(i=0;i<PACKETS_LEN;i++) {
		/* Read input file */
		memset(packet_ascii, 0, sizeof(packet_ascii));
		memset(packet, 0, sizeof(packet));
		memset(packet_compr, 0, sizeof(packet_compr));
		memset(packet_decompr, 0, sizeof(packet_decompr));
		strcpy(packet_ascii,packets[i]);

		packet_len =
		    osmo_hexparse(packet_ascii, packet, sizeof(packet));
		check_packet(ctx, packet, packet_len);
		packet_len = strip_tcp_options(ctx, packet, packet_len);
		check_packet(ctx, packet, packet_len);

		/* Run compression/decompression algorithm */
		printf("Compressing...\n");
		packet_compr_len =
		    compress(packet_compr, packet, packet_len, comp);
		printf("Decompressing...\n");
		packet_decompr_len =
		    expand(packet_decompr, packet_compr, packet_compr_len,
			   comp);
		OSMO_ASSERT(packet_decompr_len == packet_len);
		check_packet(ctx,packet_decompr,packet_decompr_len);

		/* Display results */
		printf("Results:\n");
		if (packet_compr_len > DISP_MAX_BYTES)
			packet_compr_len = DISP_MAX_BYTES;
		if (packet_len > DISP_MAX_BYTES)
			packet_len = DISP_MAX_BYTES;
		if (packet_decompr_len > DISP_MAX_BYTES)
			packet_decompr_len = DISP_MAX_BYTES;
		printf("Original Packet:    (%i bytes) %s\n", packet_len,
		       osmo_hexdump_nospc(packet, packet_len));
		printf("DecompressedPacket: (%i bytes) %s\n",
		       packet_decompr_len, osmo_hexdump_nospc(packet_decompr,
							      packet_decompr_len));
		printf("CompressedPacket:   (%i bytes) %s\n", packet_compr_len,
		       osmo_hexdump_nospc(packet_compr, packet_compr_len));
		slhc_o_status(comp);
		slhc_o_status(comp);

		printf("\n");
	}

	printf("Freeing compression state...\n");
	slhc_free(comp);
	printf("\n");
}

static struct log_info_cat gprs_categories[] = {
	[DSNDCP] = {
		    .name = "DSNDCP",
		    .description =
		    "GPRS Sub-Network Dependent Control Protocol (SNDCP)",
		    .enabled = 1,.loglevel = LOGL_DEBUG,
		    },
	[DSLHC] = {
		   .name = "DSLHC",
		   .description =
		   "Van Jacobson RFC1144 TCP/IP header compression (SLHC)",
		   .enabled = 1,.loglevel = LOGL_DEBUG,
		   }
};

static struct log_info info = {
	.cat = gprs_categories,
	.num_cat = ARRAY_SIZE(gprs_categories),
};

int main(int argc, char **argv)
{
	void *ctx;

	osmo_init_logging(&info);

	ctx = talloc_named_const(NULL, 0, "slhc_ctx");

	test_slhc(ctx);

	printf("Done\n");

	talloc_report_full(ctx, stderr);
	OSMO_ASSERT(talloc_total_blocks(ctx) == 1);
	return 0;
}

/* stubs */
struct osmo_prim_hdr;
int bssgp_prim_cb(struct osmo_prim_hdr *oph, void *ctx)
{
	abort();
}
