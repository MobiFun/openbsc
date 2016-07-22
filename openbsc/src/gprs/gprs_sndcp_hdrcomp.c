/* GPRS SNDCP header compression handler */

/* (C) 2016 by Sysmocom s.f.m.c. GmbH
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
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>

#include <osmocom/core/utils.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/linuxlist.h>
#include <osmocom/core/talloc.h>
#include <osmocom/gsm/tlv.h>

#include <openbsc/gprs_llc.h>
#include <openbsc/sgsn.h>
#include <openbsc/gprs_sndcp_xid.h>
#include <openbsc/slhc.h>

#define GPRS_SNDCP_HDRCOMP_DEBUG 1


static struct slcompress *tx_comp;
static struct slcompress *rx_comp;
static int errors;

void gprs_sndcp_hdrcomp_init(void)
{
	printf("===== HDRCOMP INIT =====\n");
	tx_comp = slhc_init(10, 10);
	rx_comp = slhc_init(10, 10);
	errors = 0;
}

static void gprs_sndcp_hdrcomp_stat(void)
{
	printf(" * Status:\n");
	printf("   slhc_i_status(tx_comp)\n");
	slhc_i_status(tx_comp);
	printf("   slhc_o_status(tx_comp)\n");
	slhc_o_status(tx_comp);
	printf("   slhc_i_status(rx_comp)\n");
	slhc_i_status(rx_comp);
	printf("   slhc_o_status(rx_comp)\n");
	slhc_o_status(rx_comp);
	printf("ERRORS=%i\n",errors);
}


/* FIXME: FOR EXPERIMENTATION ONLY! REMOVE AS SOON AS POSSIBLE */
static uint16_t header_checksum(uint8_t *iph, unsigned int ihl)
{
	int i;
	uint16_t temp;
	uint32_t accumulator = 0xFFFF;

	for(i=0;i<ihl*2;i++)
	{
		temp = ((*iph) << 8)&0xFF00;
		iph++;
		temp |= (*iph)&0xFF;
		iph++;

		accumulator+=temp;
		if(accumulator>0xFFFF)
		{
			accumulator++;
			accumulator&=0xFFFF;
		}
	}

    return (uint16_t)(htons(~accumulator)&0xFFFF);
}


/* Compress a packet using Van Jacobson RFC1144 header compression */
static int gprs_sndcp_hdrcomp_rfc1144_compress(struct slcompress *comp, uint8_t *packet, int packet_len)
{
	uint8_t *packet_compressed;
	uint8_t *packet_compressed_ptr;	/* Not used */
	int packet_compressed_len;

	/* Reserve some space for to store the compression result */
	packet_compressed = talloc_zero_size(NULL,packet_len);

	/* Run compressor */
	memcpy(packet_compressed,packet,packet_len);
	packet_compressed_len = slhc_compress(comp, packet, packet_len, (uint8_t*)packet_compressed, &packet_compressed_ptr, 1);

	/* Copy back compression result */	
	memcpy(packet,packet_compressed,packet_len);
	talloc_free(packet_compressed);

	return packet_compressed_len;
}


/* Expand a packet using Van Jacobson RFC1144 header compression */
static int gprs_sndcp_hdrcomp_rfc1144_expand(struct slcompress *comp, uint8_t *packet, int packet_len)
{
	int packet_decompressed_len;

	/* Restore the original version nibble on marked uncompressed packets */
	if((packet[0] & SL_TYPE_UNCOMPRESSED_TCP) == 0x70)
	{
#if GPRS_SNDCP_HDRCOMP_DEBUG == 1
		printf("gprs_sndcp_hdrcomp_rfc1144_expand(): Received unconmpressed, but marked packet\n");
#endif
		packet[0] &= 0x4F;
		packet_decompressed_len = slhc_remember(comp, packet, packet_len);
		return packet_decompressed_len;
	}

	/* Uncompress compressed packets */
	else if((packet[0] & SL_TYPE_COMPRESSED_TCP) == 0x80)
	{
#if GPRS_SNDCP_HDRCOMP_DEBUG == 1
		printf("gprs_sndcp_hdrcomp_rfc1144_expand(): Received compressed packet\n");
#endif
		packet_decompressed_len = slhc_uncompress(comp, packet, packet_len);
		return  packet_decompressed_len;
	}

	/* Normal packets will not be touched */
	else if((packet[0] & 0xF0) == 0x40)
	{
#if GPRS_SNDCP_HDRCOMP_DEBUG == 1
		printf("gprs_sndcp_hdrcomp_rfc1144_expand(): Received uncompressed, unmarked packet\n");
#endif
		return packet_len;
	}

	return -EINVAL;
}







/* FIXME: FOR EXPERIMENTATION ONLY! REMOVE AS SOON AS POSSIBLE */
int hdrcomp_test_ind(uint8_t *packet, int packet_len)
{
	int packet_len_compressed;
	int packet_len_uncompressed;
	printf("packet_len=%i\n",packet_len);
	uint16_t checksum;

	uint8_t *packet_backup;
	packet_backup = talloc_zero_size(NULL,packet_len);
	memcpy(packet_backup,packet,packet_len);

	printf("IND:          %s\n",osmo_hexdump_nospc(packet,packet_len));
	packet_len_compressed = gprs_sndcp_hdrcomp_rfc1144_compress(rx_comp, packet, packet_len);
	printf("IND (COMP):   %s\n",osmo_hexdump_nospc(packet,packet_len_compressed));
	packet_len_uncompressed = gprs_sndcp_hdrcomp_rfc1144_expand(tx_comp, packet, packet_len_compressed);
	printf("IND (DECOMP): %s\n",osmo_hexdump_nospc(packet,packet_len_uncompressed));
	printf("packet_len=%i   packet_len_compressed=%i   packet_len_uncompressed=%i\n",packet_len, packet_len_compressed,packet_len_uncompressed);
	if(packet_len != packet_len_uncompressed)
	{
		printf("!!!!!!!!!!!!!!!!!!!!!!!!! PACKET LENGTH MISMATCH !!!!!!!!!!!!!!!!!!!!!!!!!\n");
		talloc_free(packet_backup);
		return 0;
	}


	/* Check packet integrety */
	if(memcmp(packet,packet_backup,packet_len))
	{
		printf("!!!!!!!!!!!!!!!!!!!!!!!!! PACKET CONTENT MISMATCH !!!!!!!!!!!!!!!!!!!!!!!!!\n");
		printf("ORIGINAL:  %s\n",osmo_hexdump_nospc(packet_backup,80));
		printf("PROCESSED: %s\n",osmo_hexdump_nospc(packet,80));

		checksum = header_checksum(packet,5);
		printf("CHECKSUM:  %04x\n",checksum);

		if(checksum == 0x0000)
		{
			printf("<= Checksum looks good!\n");
			
			if(memcmp(packet+20,packet_backup+20,packet_len-20))
				errors++;
			else
				printf("<= Packet looks also good!\n");
		}	
		else
			errors++;
	}


	talloc_free(packet_backup);

	gprs_sndcp_hdrcomp_stat();

	return 1;
}

/* FIXME: FOR EXPERIMENTATION ONLY! REMOVE AS SOON AS POSSIBLE */
int hdrcomp_test_req(uint8_t *packet, int packet_len)
{
	int packet_len_compressed;
	int packet_len_uncompressed;
	uint16_t checksum;
	printf("packet_len=%i\n",packet_len);

	uint8_t *packet_backup;
	packet_backup = talloc_zero_size(NULL,packet_len);
	memcpy(packet_backup,packet,packet_len);

	printf("REQ:          %s\n",osmo_hexdump_nospc(packet,packet_len));
	packet_len_compressed = gprs_sndcp_hdrcomp_rfc1144_compress(tx_comp, packet, packet_len);
	printf("REQ (COMP):   %s\n",osmo_hexdump_nospc(packet,packet_len_compressed));
	packet_len_uncompressed = gprs_sndcp_hdrcomp_rfc1144_expand(rx_comp, packet, packet_len_compressed);
	printf("REQ (DECOMP): %s\n",osmo_hexdump_nospc(packet,packet_len_uncompressed));
	printf("packet_len=%i   packet_len_compressed=%i   packet_len_uncompressed=%i\n",packet_len, packet_len_compressed,packet_len_uncompressed);
	if(packet_len != packet_len_uncompressed)
	{
		printf("!!!!!!!!!!!!!!!!!!!!!!!!! PACKET LENGTH MISMATCH !!!!!!!!!!!!!!!!!!!!!!!!!\n");
		talloc_free(packet_backup);
		errors++;
		return 0;
	}

	/* Check packet integrety */
	if(memcmp(packet,packet_backup,packet_len))
	{
		printf("!!!!!!!!!!!!!!!!!!!!!!!!! PACKET CONTENT MISMATCH !!!!!!!!!!!!!!!!!!!!!!!!!\n");
		printf("ORIGINAL:  %s\n",osmo_hexdump_nospc(packet_backup,80));
		printf("PROCESSED: %s\n",osmo_hexdump_nospc(packet,80));

		checksum = header_checksum(packet,5);
		printf("CHECKSUM:  %04x\n",checksum);

		if(checksum == 0x0000)
		{
			printf("<= Checksum looks good!\n");
			
			if(memcmp(packet+20,packet_backup+20,packet_len-20))
				errors++;
			else
				printf("<= Packet looks also good!\n");
		}	
		else
			errors++;
	}


	talloc_free(packet_backup);
	gprs_sndcp_hdrcomp_stat();


	return 1;
}









