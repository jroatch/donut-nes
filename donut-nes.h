#ifndef INCLUDE_DONUT_NES_H
#define INCLUDE_DONUT_NES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

int donut_decompress(uint8_t* dst, int dst_capacity, const uint8_t* src, int src_length, int* src_bytes_read);

int donut_compress(uint8_t* dst, int dst_capacity, const uint8_t* src, int src_length, int* src_bytes_read);

#define donut_compress_bound(x) ((((x) + 63) / 64) * 65)

int donut_unpack_block(uint8_t* dst, const uint8_t* src);
int donut_pack_block(uint8_t* dst, const uint8_t* src, int cpu_limit, const uint8_t* mask);
int donut_unpack_pb8(uint64_t* dst, const uint8_t* src, uint8_t top_value);
int donut_pack_pb8(uint8_t* dst, uint64_t src, uint8_t top_value);
uint64_t donut_flip_plane(uint64_t plane);
int donut_block_runtime_cost(const uint8_t* buf, int len);

#ifdef DONUT_NES_IMPLEMENTATION

#include <string.h>

static uint64_t donut_read_uint64_le(const uint8_t* buf)
{
    return ((uint64_t)*(buf+0) << (8*0)) |
		((uint64_t)*(buf+1) << (8*1)) |
		((uint64_t)*(buf+2) << (8*2)) |
		((uint64_t)*(buf+3) << (8*3)) |
		((uint64_t)*(buf+4) << (8*4)) |
		((uint64_t)*(buf+5) << (8*5)) |
		((uint64_t)*(buf+6) << (8*6)) |
		((uint64_t)*(buf+7) << (8*7));
}

static void donut_write_uint64_le(uint8_t* buf, uint64_t plane)
{
	*(buf+0) = (plane >> (8*0)) & 0xff;
	*(buf+1) = (plane >> (8*1)) & 0xff;
	*(buf+2) = (plane >> (8*2)) & 0xff;
	*(buf+3) = (plane >> (8*3)) & 0xff;
	*(buf+4) = (plane >> (8*4)) & 0xff;
	*(buf+5) = (plane >> (8*5)) & 0xff;
	*(buf+6) = (plane >> (8*6)) & 0xff;
	*(buf+7) = (plane >> (8*7)) & 0xff;
}

int donut_unpack_pb8(uint64_t* dst, const uint8_t* src, uint8_t top_value)
{
    const uint8_t* p = src;
	uint8_t pb8_byte = top_value;
	uint8_t pb8_flags = *p;
    ++p;
	uint64_t val = 0;
	for (int i = 0; i < 8; ++i) {
		if (pb8_flags & 0x80) {
			pb8_byte = *p;
			++p;
		}
		pb8_flags <<= 1;
		val <<= 8;
		val |= pb8_byte;
	}
    *dst = val;
	return p - src;
}

int donut_pack_pb8(uint8_t* dst, uint64_t src, uint8_t top_value)
{
	uint8_t pb8_flags = 0;
	uint8_t pb8_byte = top_value;
	uint8_t* p = dst;
	++p;
	for (int i = 0; i < 8; ++i) {
		uint8_t c = src >> (8*(7-i));
		if (c != pb8_byte) {
			*p = c;
			++p;
			pb8_byte = c;
			pb8_flags |= 0x80>>i;
		}
	}
	*dst = pb8_flags;
	return p - dst;
}

/*static int donut_unpack_pb8_greedy_read(uint64_t* dst, const uint8_t* src, uint8_t top_value)
{
	uint8_t pb8_byte = top_value;
    uint64_t buf_read = donut_read_uint64_le(src);
    uint8_t pb8_flags = buf_read & 0xff;
    int n = 1;
	uint64_t val = 0;
    if (pb8_flags == 0xff) {
        val = donut_read_uint64_le(src+1);
        *dst = val;
        return 1+8;
    }
	for (int i = 0; i < 8; ++i) {
		if (pb8_flags & 0x80) {
            buf_read >>= 8;
			pb8_byte = buf_read;
			++n;
		}
		pb8_flags <<= 1;
		val <<= 8;
		val |= pb8_byte;
	}
    *dst = val;
	return n;
}*/

uint64_t donut_flip_plane(uint64_t plane)
{
	uint64_t result = 0;
	uint64_t t;
	int i;
	if (plane == 0xffffffffffffffff)
		return plane;
	if (plane == 0x0000000000000000)
		return plane;
	for (i = 0; i < 8; ++i) {
		t = plane >> i;
		t &= 0x0101010101010101;
		t *= 0x0102040810204080;
		t >>= 56;
		t &= 0xff;
		result |= t << (i*8);
	}
	return result;
}

int donut_unpack_block(uint8_t* dst, const uint8_t* src)
{
	int i;
	const uint8_t* p = src;
	uint8_t block_header = *p;
	++p;
	if (block_header >= 0xc0)
		return 0;
	if (block_header == 0x2a) {
		memcpy(dst, p, 64);
		return 65;
	}
	uint8_t plane_def = 0xffaa5500 >> ((block_header & 0x0c) << 1);
	bool single_plane_mode = false;
	if (block_header & 0x02) {
		plane_def = *p;
		++p;
		single_plane_mode = ((block_header & 0x04) && (plane_def != 0x00));
	}
	uint64_t prev_plane = 0x0000000000000000;
	for (i = 0; i < 8; ++i) {
		uint64_t plane = 0x0000000000000000;
		if ((!(i & 1) && (block_header & 0x20)) || ((i & 1) && (block_header & 0x10))) {
			plane = 0xffffffffffffffff;
		}
		if (plane_def & 0x80) {
			if (single_plane_mode)
				p = src+2;
			p += donut_unpack_pb8(&plane, p, (uint8_t)plane);
			if (block_header & 0x01)
				plane = donut_flip_plane(plane);
		}
		plane_def <<= 1;
		if (i & 1) {
			if (block_header & 0x80)
				prev_plane ^= plane;
			if (block_header & 0x40)
				plane ^= prev_plane;
			donut_write_uint64_le(dst, prev_plane);
			dst += 8;
			donut_write_uint64_le(dst, plane);
			dst += 8;
		}
		prev_plane = plane;
	}
	return p - src;
}

static uint8_t donut_popcount(uint8_t x)
{
	x = (x & 0x55 ) + ((x >>  1) & 0x55 );
	x = (x & 0x33 ) + ((x >>  2) & 0x33 );
	x = (x & 0x0f ) + ((x >>  4) & 0x0f );
	return (int)x;
}

int donut_block_runtime_cost(const uint8_t* buf, int len)
{
	if (len <= 0)
		return 0;
	uint8_t block_header = buf[0];
	--len;
	if (block_header >= 0xc0)
		return 0;
	if (block_header == 0x2a)
		return 1268;
	int cycles = 1298;
	if (block_header & 0xc0)
		cycles += 640;
	if (block_header & 0x20)
		cycles += 4;
	if (block_header & 0x10)
		cycles += 4;
	uint8_t plane_def = 0xffaa5500 >> ((block_header & 0x0c) << 1);
	uint8_t pb8_count = 0x08040400 >> ((block_header & 0x0c) << 1);
	bool single_plane_mode = false;
	if (block_header & 0x02) {
		if (len <= 0)
			return 0;
		cycles += 5;
		plane_def = buf[1];
		--len;
		pb8_count = donut_popcount(plane_def);
		single_plane_mode = ((block_header & 0x04) && (plane_def != 0x00));
	}
	cycles += (block_header & 0x01) ? (pb8_count * 614) : (pb8_count * 75);
	if (single_plane_mode) {
        len *= pb8_count;
		cycles += pb8_count;
	}
	len -= pb8_count;
    cycles += len * 6;
	return cycles;
}

// TODO: Clean up fill_dont_care_bits stuff!
static uint64_t donut_nes_fill_dont_care_bits_helper(uint64_t plane, uint64_t dont_care_mask, uint64_t xor_bg, uint8_t top_value) {
	uint64_t result_plane = 0;
	uint64_t backwards_smudge_plane = 0;
	uint64_t current_byte, mask, inv_mask;
	int i;
	if (dont_care_mask == 0x0000000000000000)
		return plane;

	current_byte = top_value;
	for (i = 0; i < 8; ++i) {
		mask = dont_care_mask & ((uint64_t)0xff << (i*8));
		inv_mask = ~dont_care_mask & ((uint64_t)0xff << (i*8));
		current_byte = (current_byte & mask) | (plane & inv_mask);
		backwards_smudge_plane |= current_byte;
		current_byte = current_byte << 8;
	}
	backwards_smudge_plane ^= xor_bg & dont_care_mask;

	current_byte = (uint64_t)top_value << 56;
	for (i = 0; i < 8; ++i) {
		mask = dont_care_mask & ((uint64_t)0xff << (8*(7-i)));
		inv_mask = ~dont_care_mask & ((uint64_t)0xff << (8*(7-i)));
		if ((plane & inv_mask) == (current_byte & inv_mask)) {
			current_byte = (current_byte & mask) | (plane & inv_mask);
		} else {
			current_byte = (backwards_smudge_plane & mask) | (plane & inv_mask);
		}
		result_plane |= current_byte;
		current_byte = current_byte >> 8;
	}

	return result_plane;
}

static void donut_nes_fill_dont_care_bits(uint64_t* planes, const uint64_t* masks, uint8_t mode)
{
	// TODO: Clean this up!
	uint64_t plane_predict_l;
	uint64_t plane_predict_m;
	for (int i = 0; i < 8; i += 2) {
		plane_predict_l = (mode & 0x20) ? 0xffffffffffffffff : 0x0000000000000000;
		planes[i+0] = donut_nes_fill_dont_care_bits_helper(planes[i+0], masks[i+0], 0, plane_predict_l);
		plane_predict_m = (mode & 0x10) ? 0xffffffffffffffff : 0x0000000000000000;
		planes[i+1] = donut_nes_fill_dont_care_bits_helper(planes[i+1], masks[i+1], 0, plane_predict_m);

		if (mode & 0x80)
			planes[i+0] = donut_nes_fill_dont_care_bits_helper(planes[i+0], masks[i+0], planes[i+1], plane_predict_l);
		if (mode & 0x40)
			planes[i+1] = donut_nes_fill_dont_care_bits_helper(planes[i+1], masks[i+1], planes[i+0], plane_predict_m);
	}
	return;
}

static bool donut_nes_all_pb8_planes_match(const uint8_t* buf, int len, int pb8_count)
{
	// a block of 0 dupplicate pb8 planes is 1 byte more then normal,
	// and a normal block of 1 pb8 plane is 5 cycles less to decode
	if (pb8_count <= 1)
		return false;
	len -= 2;
	// if planes don't divide evenly
	if (len % pb8_count)
		return false;
	int pb8_length = len/pb8_count;
	int i, c;
	for (c = 0, i = pb8_length; i < len; ++i, ++c) {
		if (c >= pb8_length)
			c = 0;
		if (buf[c+2] != buf[i+2])
			return false;
	}
	return true;
}

int donut_pack_block(uint8_t* dst, const uint8_t* src, int cpu_limit, const uint8_t* mask)
{
	uint64_t planes[16];
	uint8_t cblock[74];
	int i;

	// if no limit specified, then basically unlimited.
	cpu_limit = (cpu_limit) ? cpu_limit : 16384;

	// first the fallback uncompressed block.
	dst[0] = 0x2a;
	memcpy(dst + 1, src, 64);
	int shortest_len = 65;
	int least_cost = 1268;
	// cpu_limit constrains too much. uncompressed block is all that can happen.
	if (cpu_limit < 1298)
		return shortest_len;
	for (i = 0; i < 8; ++i) {
		planes[i] = donut_read_uint64_le(src+(i*8));
	}
	if (mask) {
		for (i = 0; i < 8; ++i) {
			planes[i+8] = donut_read_uint64_le(mask+(i*8));
		}
	}

	// Try to compress with all 48 different block modes.
	// The rotate bit (0x01) will toggle last so that flip_plane
	// is only called once instead of 24 times.
	uint8_t a = 0x00;
	while (1) {
		if (a >= 0xc0) {
			if (a & 0x01)
				break;
			for (i = 0; i < ((mask) ? 16 : 8); ++i) {
				planes[i] = donut_flip_plane(planes[i]);
			}
			a = 0x01;
		}
		if (mask)
			donut_nes_fill_dont_care_bits(planes, planes+8, a);
		// With the block mode in mind, pack the 64 bytes of data into 8 pb8 planes.
		uint8_t plane_def = 0x00;
		int len = 2;
		int pb8_count = 0;
		for (i = 0; i < 8; ++i) {
			uint64_t plane_predict = 0x0000000000000000;
			uint64_t plane = planes[i];
			if (i & 1) {
				if (a & 0x10)
					plane_predict = 0xffffffffffffffff;
				if (a & 0x40)
					plane ^= planes[i-1];
			} else {
				if (a & 0x20)
					plane_predict = 0xffffffffffffffff;
				if (a & 0x80)
					plane ^= planes[i+1];
			}
			plane_def <<= 1;
			if (plane != plane_predict) {
				len += donut_pack_pb8(cblock + len, plane, (uint8_t)plane_predict);
				plane_def |= 1;
				++pb8_count;
			}
		}
		cblock[0] = a | 0x02;
		cblock[1] = plane_def;
		// now that we have the basic block form, try to find optimizations
		// temp_p is needed because a optimization removes a byte from the start
		int cycles = donut_block_runtime_cost(cblock, len);
		uint8_t* temp_p = cblock;
		if (donut_nes_all_pb8_planes_match(temp_p, len, pb8_count) && ((cycles + pb8_count) <= cpu_limit)) {
			temp_p[0] = a | 0x06;
			len = ((len - 2) / pb8_count) + 2;
			cycles += pb8_count;
		} else {
			for (i = 0; i < 4*8; i += 8) {
				if (plane_def == (0xffaa5500 >> i)) {
					++temp_p;
					temp_p[0] = a | (i >> 1);
					--len;
					cycles -= 5;
					break;
				}
			}
		}
		// TODO: figure out how to do single plane mode block where
		// a pb8 plane with a explict leading 0x00/0xff byte makes it
		// match both predicted planes.

		// compare size and cpu cost to choose the block of this mode
		// or to keep the old one.
		if ((len <= shortest_len) && ((cycles < least_cost) || (len < shortest_len)) && (cycles <= cpu_limit)) {
			memcpy(dst, temp_p, len);
			shortest_len = len;
			least_cost = cycles;
		}
		// onto the next block mode.
		a += 0x10;
	}

	return shortest_len;
}

int donut_decompress(uint8_t* dst, int dst_capacity, const uint8_t* src, int src_length, int* src_bytes_read)
{
	uint8_t scratch_space[64+74];
	int dst_length = 0;
	int bytes_read = 0;
	int l;
	while (1) {
		int src_bytes_remain = src_length - bytes_read;
		int dst_bytes_remain = dst_capacity - dst_length;
		if (src_bytes_remain < 0)
			break;
		if (dst_bytes_remain < 64)
			break;
		if (src_bytes_remain < 74) {
			memset(scratch_space, 0x00, 64+74);
			memcpy(scratch_space+64, src + bytes_read, src_bytes_remain);
			l = donut_unpack_block(scratch_space, scratch_space+64);
			if ((!l) || (l > src_bytes_remain))
				break;
			memcpy(dst + dst_length, scratch_space, 64);
			bytes_read += l;
			dst_length += 64;
			continue;
		}
		l = donut_unpack_block(dst + dst_length, src + bytes_read);
		// TODO: process special codes >=0xc0
		if (!l)
			break;
		bytes_read += l;
		dst_length += 64;
	}
	
	if (src_bytes_read)
		*src_bytes_read = bytes_read;
	return dst_length;
}

int donut_compress(uint8_t* dst, int dst_capacity, const uint8_t* src, int src_length, int* src_bytes_read)
{
	uint8_t scratch_space[65+64];
	int dst_length = 0;
	int bytes_read = 0;
	int l;
	while (1) {
		int src_bytes_remain = src_length - bytes_read;
		int dst_bytes_remain = dst_capacity - dst_length;
		if (src_bytes_remain < 64)
			break;
		if (dst_bytes_remain < 0)
			break;
		if (dst_bytes_remain < 65) {
			memset(scratch_space, 0x00, 65+64);
			memcpy(scratch_space, src, 64);
			l = donut_pack_block(scratch_space+64, scratch_space, 0, NULL);
			if ((!l) || (l > dst_bytes_remain))
				break;
			memcpy(dst, scratch_space+64, l);
			bytes_read += 64;
			dst_length += l;
			continue;
		}
		l = donut_pack_block(dst + dst_length, src + bytes_read, 0, NULL);
		if (!l)
			break;
		bytes_read += 64;
		dst_length += l;
	}
	
	if (src_bytes_read)
		*src_bytes_read = bytes_read;
	return dst_length;
}

#endif // DONUT_NES_IMPLEMENTATION
#endif // INCLUDE_DONUT_NES_H
