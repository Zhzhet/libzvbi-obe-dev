/*
 *  libzvbi - vbi_dvb_mux unit test
 *
 *  Copyright (C) 2007 Michael H. Schimek
 */

/* $Id: test-dvb_mux.cc,v 1.1 2007/08/27 06:43:39 mschimek Exp $ */

#undef NDEBUG

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "src/misc.h"
#include "src/dvb.h"
#include "src/dvb_mux.h"
#include "src/dvb_demux.h"
#include "src/version.h"

#if 3 == VBI_VERSION_MINOR
#  define sp_samples_per_line samples_per_line
#else
   /* Has no samples_per_line field yet. */
#  define sp_samples_per_line bytes_per_line
#endif

static uint8_t *		cb_pes_bp;
static uint8_t *		cb_ts_bp;
static vbi_dvb_mux *		cb_multiplexer;
static vbi_bool		cb_pes_cmp;
static vbi_bool		cb_ts_cmp;
static int			cb_next_continuity_counter;

// XXX Later.
enum {
	VBI_ERR_BUFFER_OVERFLOW = 0,
	VBI_ERR_AMBIGUOUS_VIDEOSTD = 0,
	VBI_ERR_LINE_NUMBER = 0,
	VBI_ERR_LINE_ORDER = 0,
	VBI_ERR_INVALID_SERVICE = 0,
	VBI_ERR_SAMPLE_NUMBER = 0,
	VBI_ERR_NO_RAW_DATA = 0,
	VBI_ERR_SAMPLING_PAR = 0,
};

enum {
	EXPECT_FAILURE = FALSE,
	EXPECT_SUCCESS = TRUE
};

// Data unit size.
enum {
	VARIABLE = FALSE,
	FIXED = TRUE
};

enum {
	NO_STUFFING = FALSE,
	STUFFING = TRUE,
	ANY_STUFFING = 0x12345,
};

static const unsigned int ANY_DATA_IDENTIFIER = 0x12345;
static const vbi_videostd_set ANY_VIDEOSTD = 0x12345;

static const vbi_service_set ALL_SERVICES = -1;

// EN 301 775 table 2.
static const unsigned int
data_identifiers[] = {
	0,	// "reserved for future use"
	0x0F,
	0x10,	/* "EBU Teletext only or EBU Teletext combined with
		    VPS and/or WSS and/or Closed Captioning and/or
		    VBI sample data" */
	0x1F,
	0x20,	// "reserved for future use"
	0x7F,
	0x80,	// "user defined"
	0x98,
	0x99,	/* "EBU Teletext and/or VPS and/or WSS and/or Closed
		    Captioning and/or VBI sample data" */
	0x9B,
	0x9C,	// "user defined"
	0xFF,
	UINT_MAX
};

// EN 301 775 table 3.
static const vbi_service_set
good_services [] = {
	0,
	VBI_SLICED_CAPTION_625,
	// EN 301 775 section 4.8.2: Only first field.
	VBI_SLICED_CAPTION_625_F1,
	VBI_SLICED_TELETEXT_B_625,
	VBI_SLICED_TELETEXT_B_L10_625,
	VBI_SLICED_TELETEXT_B_L25_625,
	// EN 301 775 section 4.6.2: Only first field.
	VBI_SLICED_VPS,
	VBI_SLICED_WSS_625,
};

static vbi_bool
is_good_service			(vbi_service_set	service)
{
	unsigned int i;

	for (i = 0; i < N_ELEMENTS (good_services); ++i) {
		if (service == good_services[i])
			return TRUE;
	}

	return FALSE;
}

static const vbi_service_set
all_services [] = {
	0,
	-1,
	VBI_SLICED_2xCAPTION_525,
	VBI_SLICED_CAPTION_525,
	VBI_SLICED_CAPTION_525_F1,
	VBI_SLICED_CAPTION_525_F2,
	// Just a little challenge.
	VBI_SLICED_CAPTION_625 | VBI_SLICED_WSS_625,
	VBI_SLICED_CAPTION_625,
	VBI_SLICED_CAPTION_625_F1,
	VBI_SLICED_CAPTION_625_F2,
	VBI_SLICED_TELETEXT_A,
	VBI_SLICED_TELETEXT_BD_525,
	VBI_SLICED_TELETEXT_B_525,
	VBI_SLICED_TELETEXT_B_625 | VBI_SLICED_VPS,
	VBI_SLICED_TELETEXT_B_625,
	VBI_SLICED_TELETEXT_B_L10_625,
	VBI_SLICED_TELETEXT_B_L25_625,
	VBI_SLICED_TELETEXT_C_525,
	VBI_SLICED_TELETEXT_D_525,
	VBI_SLICED_VBI_525,
	VBI_SLICED_VBI_625,
	VBI_SLICED_VPS | VBI_SLICED_CAPTION_625,
	VBI_SLICED_VPS | VBI_SLICED_VPS_F2,
	VBI_SLICED_VPS,
	VBI_SLICED_VPS_F2,
	VBI_SLICED_WSS_625,
	VBI_SLICED_WSS_CPR1204,
};

/* These line numbers are bad because they cannot be encoded in a
   ETS 300 472 / EN 301 775 compliant stream. */
static const unsigned int
bad_line_numbers [] = {
	32,
	262,
	263 + 32,
	312,
	313 + 32,
	524,
	525,
	526,
	624,
	625,
	626,
	INT_MAX,
	((unsigned int) INT_MAX) + 1,
	UINT_MAX
};

static const unsigned int
raw_offsets [] = {
	0,
	1,
	39,
	40,
	41,
	250,
	251,
	252,
	719,
	720,
	721,
	INT_MAX,
	((unsigned int) INT_MAX) + 1,
	UINT_MAX
};

static const unsigned int
border_uints [] = {
	INT_MAX,
	((unsigned int) INT_MAX) + 1,
	UINT_MAX
};

#define RAND(var) memset_rand (&(var), sizeof (var))

static void *
memset_rand			(void *			d1,
				 size_t			n)
{
	uint8_t *d = (uint8_t *) d1;

	while (n-- > 0)
		*d++ = rand ();

	return d1;
}

static void *
xmalloc				(size_t			n_bytes)
{
	void *p;

	p = malloc (n_bytes);
	assert (NULL != p);

	return p;
}

static uint8_t *
xralloc				(size_t			n_bytes)
{
	uint8_t *p_begin;
	uint8_t *p_end;
	uint8_t *p;
	unsigned int x;

	p_begin = (uint8_t *) xmalloc (n_bytes);

	p_end = p_begin + (n_bytes & ~3);

	for (p = p_begin; p < p_end; p += 4)
		* (uint32_t *) p = mrand48 ();

	p_end = p + (n_bytes & 3);
	x = mrand48 ();

	while (p < p_end) {
		*p++ = x;
		x >>= 8;
	}

	return p_begin;
}

static vbi_sliced *
alloc_sliced			(unsigned int		n_lines)
{
	vbi_sliced *sliced;

	sliced = (vbi_sliced *) xmalloc (n_lines * sizeof (vbi_sliced));

	/* Must initialize the .data[] arrays for valgrind. We fill
	   with 0xFF because zeros have special meaning. */
	memset (sliced, -1, n_lines * sizeof (vbi_sliced));

	return sliced;
}

static uint8_t *
alloc_raw_frame			(const vbi_sampling_par *sp)
{
	unsigned int n_lines;
	unsigned int size;

	n_lines = sp->count[0] + sp->count[1];
	assert (n_lines > 0);

	size = (n_lines - 1) * sp->bytes_per_line;
	size += sp->sp_samples_per_line;

	return xralloc (size);
}

static void
assert_stuffing_ok		(unsigned int *		n_sliced_dus,
				 unsigned int *		n_raw_dus,
				 unsigned int *		n_stuffing_dus,
				 const uint8_t *	p,
				 unsigned int		n_bytes,
				 vbi_bool		fixed_length)
{
	/* Verify the value of reserved bits and stuffing bytes
	   which are ignored by the vbi_dvb_demux. */

	*n_sliced_dus = 0;
	*n_raw_dus = 0;
	*n_stuffing_dus = 0;

	while (n_bytes >= 2) {
		unsigned int data_unit_id;
		unsigned int data_unit_length;
		unsigned int n_pixels;
		unsigned int min_bits;
		unsigned int i;

		data_unit_id = p[0];
		data_unit_length = p[1];

		// EN 301 775 section 4.4.2.
		if (fixed_length)
			assert (0x2C == data_unit_length);

		// EN 301 775 table 3.
		switch (data_unit_id) {
		case 0x02: // "EBU Teletext non-subtitle data"
			// EN 301 775 table 4.
			min_bits = 2 + 1 + 5 + 8 + 336;
			assert (0xC0 == (p[2] & 0xC0));
			++*n_sliced_dus;
			break;

		case 0x03: // "EBU Teletext subtitle data"
			// Not supported by libzvbi.
			assert (0);

		case 0xB4: /* DATA_UNIT_ZVBI_WSS_CPR1204 */
			// Should not appear here.
			assert (0);

		case 0xB5: /* DATA_UNIT_ZVBI_CLOSED_CAPTION_525 */
			// Should not appear here.
			assert (0);

		case 0xB6: /* DATA_UNIT_ZVBI_MONOCHROME_SAMPLES_525 */
			// Should not appear here.
			assert (0);

		case 0xC0: // "Inverted Teletext"
		case 0xC1: // "Reserved"? Teletext? (EN 301 775 table 1)
			// Not supported by libzvbi.
			assert (0);

		case 0xC3: // "VPS"
			// EN 301 775 table 6.
			min_bits = 2 + 1 + 5 + 104;
			assert (0xC0 == (p[2] & 0xC0));
			++*n_sliced_dus;
			break;

		case 0xC4: // "WSS"
			// EN 301 775 table 8.
			min_bits = 2 + 1 + 5 + 14 + 2;
			assert (0xC0 == (p[2] & 0xC0));
			assert (0x03 == (p[4] & 0x03));
			++*n_sliced_dus;
			break;

		case 0xC5: // "Closed Captioning"
			// EN 301 775 table 10.
			min_bits = 2 + 1 + 5 + 16;
			assert (0xC0 == (p[2] & 0xC0));
			++*n_sliced_dus;
			break;

		case 0xC6: // "monochrome 4:2:2 samples"
			n_pixels = p[5];
			min_bits = 1 + 1 + 1 + 5 + 16 + 8 + 8 * n_pixels;
			++*n_raw_dus;
			break;

		case 0xFF: // "stuffing"
			// EN 301 775 table 1.
			min_bits = 0;
			++*n_stuffing_dus;
			break;

		default:
			assert (0);
		}

		/* Our mux should not generate stuffing data units
		   between other data units. */
		if (0xFF != data_unit_id)
			assert (0 == *n_stuffing_dus);

		assert (data_unit_length >= min_bits / 8);

		assert (n_bytes >= 2 + data_unit_length);

		// EN 301 775 table 1: N * stuffing_byte [8].
		for (i = min_bits / 8; i < data_unit_length; ++i)
			assert (0xFF == p[2 + i]);

		p += 2 + data_unit_length;
		n_bytes -= 2 + data_unit_length;
	}

	assert (0 == n_bytes);
}

static void
assert_raw_data_units_ok	(unsigned int *		n_raw_dus,
				 unsigned int *		n_stuffing_dus,
				 const uint8_t *	p,
				 unsigned int		n_bytes,
				 vbi_bool		fixed_length,
				 const uint8_t *	raw,
				 unsigned int		raw_offset,
				 vbi_videostd_set	videostd_set,
				 unsigned int		frame_line,
				 unsigned int		first_pixel_position_0,
				 unsigned int		n_pixels_total)
{
	unsigned int f2_start;
	unsigned int next_first_pixel_position;

	*n_raw_dus = 0;
	*n_stuffing_dus = 0;

	if (0 != (videostd_set & VBI_VIDEOSTD_SET_525_60)) {
		assert (0 == (videostd_set & VBI_VIDEOSTD_SET_625_50));
		f2_start = 263;
	} else if (0 != (videostd_set & VBI_VIDEOSTD_SET_625_50)) {
		f2_start = 313;
	} else {
		assert (0);
	}

	next_first_pixel_position = first_pixel_position_0 + raw_offset;

	while (n_bytes >= 2) {
		unsigned int data_unit_id;
		unsigned int data_unit_length;
		unsigned int min_bits;
		unsigned int first_segment_flag;
		unsigned int last_segment_flag;
		unsigned int field_parity;
		unsigned int line_offset;
		unsigned int first_pixel_position;
		unsigned int n_pixels;
		unsigned int i;

		data_unit_id = p[0];
		data_unit_length = p[1];

		// EN 301 775 section 4.4.2.
		if (fixed_length)
			assert (0x2C == data_unit_length);

		// EN 301 775 table 3.
		switch (data_unit_id) {
		case 0x02: // "EBU Teletext non-subtitle data"
		case 0x03: // "EBU Teletext subtitle data"
		case 0xC0: // "Inverted Teletext"
		case 0xC1: // "Reserved"? Teletext? (EN 301 775 table 1)
		case 0xC3: // "VPS"
		case 0xC4: // "WSS"
		case 0xC5: // "Closed Captioning"
			// Should not appear here.
			assert (0);

		case 0xC6: // "monochrome 4:2:2 samples"
			// EN 301 775 table 12.
			first_segment_flag	= !!(p[2] & 0x80);
			last_segment_flag	= !!(p[2] & 0x40);
			field_parity		= !!(p[2] & 0x20);
			line_offset		= p[2] & 0x1F;
			first_pixel_position	= p[3] * 256 + p[4];
			n_pixels		= p[5];

			// EN 301 775 section 4.9.2
			assert ((first_pixel_position
				 == first_pixel_position_0)
				== first_segment_flag);

			// EN 301 775 section 4.9.2
			assert ((first_pixel_position + n_pixels
				 == (first_pixel_position_0
				     + n_pixels_total))
				== last_segment_flag);

			assert (field_parity == (frame_line < f2_start));
			if (0 == field_parity)
				assert (line_offset == frame_line - f2_start);
			else
				assert (line_offset == frame_line);

			// EN 301 775 table 12, section 4.9.2.
			assert (first_pixel_position <= 719);

			/* EN 301 775 section 4.9.2: "If this segment
			   is followed by another (i.e. last_segment_flag
			   equals '0'), the value of first_pixel_position
			   of the next segment shall equal the sum of the
			   current values of first_pixel_position and
			   n_pixels." */
			assert (first_pixel_position
				== next_first_pixel_position);

			next_first_pixel_position =
				first_pixel_position + n_pixels;

			// EN 301 775 table 12, section 4.9.2.
			assert (n_pixels >= 1);
			assert (n_pixels <= 251);

			assert (0 == memcmp (raw, p + 6, n_pixels));
			raw += n_pixels;

			// EN 301 775 table 12.
			min_bits = 1 + 1 + 1 + 5 + 16 + 8 + n_pixels * 8;

			++*n_raw_dus;

			break;

		case 0xFF: // "stuffing"
			// EN 301 775 table 1.
			min_bits = 0;
			++*n_stuffing_dus;
			break;

		default:
			assert (0);
		}

		/* Our mux should not generate stuffing data units
		   between other data units. */
		if (0xFF != data_unit_id)
			assert (0 == *n_stuffing_dus);

		assert (data_unit_length >= min_bits / 8);

		assert (n_bytes >= 2 + data_unit_length);

		// EN 301 775 table 1: N * stuffing_byte [8].
		for (i = min_bits / 8; i < data_unit_length; ++i)
			assert (0xFF == p[2 + i]);

		p += 2 + data_unit_length;
		n_bytes -= 2 + data_unit_length;
	}

	assert (0 == n_bytes);
}

static void
assert_pes_packet_ok		(unsigned int *		n_sliced_dus,
				 unsigned int *		n_raw_dus,
				 unsigned int *		n_stuffing_dus,
				 const uint8_t *	p,
				 unsigned int		n_bytes,
				 unsigned int		data_identifier,
				 unsigned int		min_size,
				 unsigned int		max_size)
{
	unsigned int PES_packet_length;
	unsigned int PES_header_data_length;
	vbi_bool fixed_length;
	unsigned int i;

	// EN 301 775 section 4.4.2.
	fixed_length = (data_identifier >= 0x10
			&& data_identifier <= 0x1F);

	assert (n_bytes >= 46);

	/* packet_start_code_prefix [24],
	   stream_id [8] */
	assert (0x00 == p[0]);
	assert (0x00 == p[1]);
	assert (0x01 == p[2]);
	assert (0xBD == p[3]);

	PES_packet_length = p[4] * 256 + p[5];

	// EN 301 775 section 4.3.
	assert (0 == (PES_packet_length + 6) % 184);

	assert (PES_packet_length + 6 >= min_size);
	assert (PES_packet_length + 6 <= max_size);

	/* '10',
	   PES_scrambling_control [2],
	   PES_priority,
	   data_alignment_indicator,
	   copyright,
	   original_or_copy */
	assert (0x84 == p[6]);
	
	/* PTS_DTS_flags [2],
	   ESCR_flag,
	   ES_rate_flag
	   DSM_trick_mode_flag,
	   additional_copy_info_flag,
	   PES_CRC_flag,
	   PES_extension_flag */
	assert (0x80 == p[7]);

	PES_header_data_length = p[8];

	// EN 301 775 section 4.3.
	assert (0x24 == PES_header_data_length);

	/* '0010',
	   PTS 32...30 [3]
	   marker_bit,
	   PTS 29 ... 15 [15],
	   marker_bit,
	   PTS 14 ... 0 [15]
	   marker_bit */
	assert (0x21 == (p[9] & 0xF1));
	assert (0x01 == (p[11] & 0x01));
	assert (0x01 == (p[13] & 0x01));

	// EN 301 775 section 4.3. (9 + 0x24 == 45)
	for (i = 14; i <= 44; ++i) {
		/* stuffing_byte [8] */
		assert (0xFF == p[i]);
	}

	assert (data_identifier == p[45]);

	p += 46;
	n_bytes -= 46;

	assert (PES_packet_length - 40 == n_bytes);

	assert_stuffing_ok (n_sliced_dus,
			    n_raw_dus,
			    n_stuffing_dus,
			    p,
			    n_bytes,
			    fixed_length);
}

static void
assert_same_sliced		(const vbi_sliced *	sliced_in,
				 unsigned int		n_lines_in,
				 const vbi_sliced *	sliced_out,
				 unsigned int		n_lines_out,
				 vbi_service_set	service_mask)
{
	unsigned int i_in;
	unsigned int i_out;

	i_out = 0;

	for (i_in = 0; i_in < n_lines_in; ++i_in) {
		vbi_service_set id_in;
		vbi_service_set id_out;
		unsigned int payload_bits;

		id_in = sliced_in[i_in].id;
		id_out = sliced_out[i_out].id;

		switch (id_in & service_mask) {
		case VBI_SLICED_CAPTION_625_F1:
		case VBI_SLICED_CAPTION_625:
			assert (VBI_SLICED_CAPTION_625_F1 == id_out);
			break;

		case VBI_SLICED_TELETEXT_B_L10_625:
		case VBI_SLICED_TELETEXT_B_L25_625:
		case VBI_SLICED_TELETEXT_B_625:
			assert (VBI_SLICED_TELETEXT_B_625 == id_out);
			break;

		case VBI_SLICED_VPS:
		case VBI_SLICED_WSS_625:
			assert (id_in == id_out);
			break;

		default:
			// Was not encoded.
			continue;
		}

		assert (i_out < n_lines_out);

		assert (sliced_in[i_in].line == sliced_out[i_out].line);

		payload_bits = vbi_sliced_payload_bits (id_in);

		assert (payload_bits > 0);
		assert ( ((payload_bits + 7) >> 3)
			<= sizeof (sliced_in[0].data));

		assert (0 == memcmp (sliced_in[i_in].data,
				     sliced_out[i_out].data,
				     payload_bits >> 3));

		if ((payload_bits & 7) > 0) {
			unsigned int last_in;
			unsigned int last_out;
			unsigned int mask;

			last_in = sliced_in[i_in].data[payload_bits >> 3];
			last_out = sliced_out[i_out].data[payload_bits >> 3];
			mask = (1 << (payload_bits & 7)) - 1;

			assert (0 == ((last_in ^ last_out) & mask));
		}

		++i_out;
	}

	assert (i_out == n_lines_out);
}

static void
assert_du_conversion_ok		(const uint8_t *	packet,
				 unsigned int		packet_size,
				 const vbi_sliced *	sliced_in,
				 unsigned int		n_lines_in,
				 vbi_service_set	service_mask)
{
	vbi_sliced *sliced_out;
	const uint8_t *p;
	unsigned int p_left;
	unsigned int n_lines_out;
	unsigned int max_lines_out; 
	vbi_bool success;

	max_lines_out = n_lines_in * 2 + 1;
	sliced_out = alloc_sliced (max_lines_out);
	memset_rand (sliced_out, sizeof (sliced_out));

	p = packet;
	p_left = packet_size;

	success = _vbi_dvb_demultiplex_sliced (sliced_out,
						&n_lines_out,
						max_lines_out,
						&p,
						&p_left);
	assert (TRUE == success);
	assert (n_lines_out < max_lines_out);

	assert (n_lines_out <= n_lines_in);
	assert (0 == p_left);

	assert_same_sliced (sliced_in,
			    n_lines_in,
			    sliced_out,
			    n_lines_out,
			    service_mask);

	free (sliced_out);
	sliced_out = (vbi_sliced *) -1;
}

static void
assert_pes_conversion_ok	(const uint8_t *	packet,
				 unsigned int		packet_size,
				 const vbi_sliced *	sliced_in,
				 unsigned int		n_lines_in,
				 vbi_service_set	service_mask,
				 int64_t		pts_in)
{
	vbi_dvb_demux *dx;
	const uint8_t *p;
	vbi_sliced *sliced_out;
	unsigned int p_left;
	unsigned int n_lines_out;
	unsigned int max_lines_out;
	int64_t pts_valid_bits;
	int64_t pts_out;

	max_lines_out = n_lines_in * 2 + 1;
	sliced_out = alloc_sliced (max_lines_out);
	memset_rand (sliced_out, sizeof (sliced_out));

	pts_out = rand();

	dx = vbi_dvb_pes_demux_new (/* callback */ NULL,
				     /* user_data */ NULL);
	assert (NULL != dx);

	p = packet;
	p_left = packet_size;

	n_lines_out = vbi_dvb_demux_cor (dx,
					  sliced_out,
					  max_lines_out,
					  &pts_out,
					  &p,
					  &p_left);
	assert (0 == n_lines_out);
	assert (0 == p_left);

	p = packet;
	p_left = packet_size;

	n_lines_out = vbi_dvb_demux_cor (dx,
					  sliced_out,
					  max_lines_out,
					  &pts_out,
					  &p,
					  &p_left);
	// Frame complete?
	/* FIXME: Frame end may be unclear, e.g. only data
	   units with line_offset = 0 in the packet. */
	if (0 == p_left) {
		vbi_dvb_demux_delete (dx);
		free (sliced_out);
		return;
	}

	assert (n_lines_out < max_lines_out);
	assert (n_lines_out <= n_lines_in);

	vbi_dvb_demux_delete (dx);
	dx = (vbi_dvb_demux *) -1;

	assert_same_sliced (sliced_in,
			    n_lines_in,
			    sliced_out,
			    n_lines_out,
			    service_mask);

	// FIXME: Compare raw data frame.

	// ISO 13818-1 section 2.4.3.7.
	pts_valid_bits = ((int64_t) 1 << 33) - 1;

	assert (0 == ((pts_in ^ pts_out) & pts_valid_bits));

	free (sliced_out);
}

/*
	Test vbi_dvb_multiplex_sliced().
 */

static void
assert_multiplex_sliced		(uint8_t * const	p1,
				 const unsigned int	p1_size,
				 const vbi_sliced * const s1,
				 const unsigned int	s1_lines,
				 const vbi_service_set	service_mask,
				 unsigned int		data_identifier,
				 vbi_bool		stuffing,
				 vbi_bool		exp_success,
				 int			exp_errno,
				 unsigned int		exp_out_lines,
				 unsigned int		exp_out_data_size,
				 unsigned int		exp_consumed_lines)
{
	uint8_t *p;
	uint8_t *rand_buffer;
	const vbi_sliced *s;
	unsigned int p_left;
	unsigned int s_left;
	unsigned int fixed_length;
	unsigned int n_sliced_dus;
	unsigned int n_raw_dus;
	unsigned int n_stuffing_dus;
	vbi_bool success;

	if (ANY_STUFFING == stuffing) {
		assert_multiplex_sliced (p1, p1_size,
					 s1, s1_lines,
					 service_mask,
					 data_identifier,
					 /* stuffing */ FALSE,
					 exp_success,
					 exp_errno,
					 exp_out_lines,
					 exp_out_data_size,
					 exp_consumed_lines);
		stuffing = TRUE;

		if (exp_success)
			exp_out_data_size = p1_size;
	}

	if (ANY_DATA_IDENTIFIER == data_identifier) {
		assert_multiplex_sliced (p1, p1_size,
					 s1, s1_lines,
					 service_mask,
					 /* data_identifier */ 0x99,
					 stuffing,
					 exp_success,
					 exp_errno,
					 exp_out_lines,
					 exp_out_data_size,
					 exp_consumed_lines);
		data_identifier = 0x10;

		if (exp_success) {
			if (0 == p1_size % 46) {
				if (stuffing)
					exp_out_data_size = p1_size;
				else
					exp_out_data_size = exp_out_lines * 46;
			} else {
				exp_success = FALSE;
				exp_errno = VBI_ERR_BUFFER_OVERFLOW;
				exp_out_lines = 0;
				exp_out_data_size = 0;
				exp_consumed_lines = 0;
			}
		}
	}

	if (NULL != p1) {
		rand_buffer = xralloc (p1_size);
		memcpy (p1, rand_buffer, p1_size);
	} else {
		rand_buffer = NULL;
	}

	p = p1;
	p_left = p1_size;

	s = s1;
	s_left = s1_lines;

	success = vbi_dvb_multiplex_sliced (&p, &p_left,
					     &s, &s_left,
					     service_mask,
					     data_identifier,
					     stuffing);
	assert (exp_success == success);

	if (!success) {
		exp_errno = exp_errno;
		// XXX later: assert (exp_errno == errno);
	}

	assert (p1 + exp_out_data_size == p);
	assert (p1_size - exp_out_data_size == p_left);

	assert (s1 + exp_consumed_lines == s);
	assert (s1_lines - exp_consumed_lines == s_left);

	if (NULL == p1)
		goto finish;

	assert (0 == memcmp (p, rand_buffer + exp_out_data_size,
			     p1_size - exp_out_data_size));

	// EN 301 775 section 4.4.2.
	fixed_length = (data_identifier >= 0x10
			&& data_identifier <= 0x1F);

	assert_stuffing_ok (&n_sliced_dus,
			    &n_raw_dus,
			    &n_stuffing_dus,
			    p1,
			    exp_out_data_size,
			    fixed_length);

	if (success && stuffing) {
		assert (exp_out_lines == n_sliced_dus);
		assert (0 == n_raw_dus);

		assert_du_conversion_ok (p1,
					 p1_size,
					 s1,
					 s1_lines - s_left,
					 service_mask);
	} else {
		assert (exp_out_lines == n_sliced_dus);
		assert (0 == n_raw_dus);
		assert (0 == n_stuffing_dus);

		if (exp_out_data_size > 0) {
			assert_du_conversion_ok (p1,
						 exp_out_data_size,
						 s1,
						 s1_lines - s_left,
						 service_mask);
		}
	}

 finish:
	free (rand_buffer);
}

static void
test_ms_stuffing		(unsigned int		buffer_size,
				 vbi_sliced *		sliced,
				 unsigned int		n_lines,
				 unsigned int		data_identifier,
				 unsigned int		exp_out_lines)
{
	uint8_t *buffer;

	buffer = (uint8_t *) xmalloc (buffer_size);

	assert_multiplex_sliced (buffer,
				 buffer_size,
				 sliced,
				 n_lines,
				 ALL_SERVICES,
				 data_identifier,
				 STUFFING,
				 EXPECT_SUCCESS,
				 /* exp_errno */ 0,
				 exp_out_lines,
				 /* exp_out_data_size */ buffer_size,
				 /* exp_consumed_lines */ exp_out_lines);
	free (buffer);
}

static void
test_multiplex_sliced_stuffing	(void)
{
	vbi_sliced *sliced;
	unsigned int n_lines;
	unsigned int buffer_size;

	sliced = alloc_sliced (n_lines = 1);

	sliced[0].id = VBI_SLICED_TELETEXT_B_625;
	sliced[0].line = 7;

	for (buffer_size = 2; buffer_size < 46; ++buffer_size) {
		test_ms_stuffing (buffer_size,
				  sliced, n_lines,
				  /* data_identifier */ 0x99,
				  /* exp_out_lines */ 0);
	}

	for (buffer_size = 46; buffer_size < 300; ++buffer_size) {
		test_ms_stuffing (buffer_size,
				  sliced, n_lines,
				  /* data_identifier */ 0x99,
				  /* exp_out_lines */ 1);
	}

	for (buffer_size = 1 * 46;
	     buffer_size <= 10 * 46; buffer_size += 46) {
		test_ms_stuffing (buffer_size,
				  sliced, n_lines,
				  /* data_identifier */ 0x10,
				  /* exp_out_lines */ 1);
	}

	free (sliced);
}

static void
test_multiplex_sliced_null_sliced	(void)
{
	unsigned int buffer_size;

	for (buffer_size = 2; buffer_size < 300; ++buffer_size) {
		test_ms_stuffing (buffer_size,
				  /* sliced */ NULL,
				  /* sliced_lines */ 1,
				  /* data_identifier */ 0x99,
				  /* exp_out_lines */ 0);

		test_ms_stuffing (buffer_size,
				  /* sliced */ (vbi_sliced *) -1,
				  /* sliced_lines */ 0,
				  /* data_identifier */ 0x99,
				  /* exp_out_lines */ 0);
	}

	for (buffer_size = 1 * 46;
	     buffer_size <= 10 * 46; buffer_size += 46) {
		test_ms_stuffing (buffer_size,
				  /* sliced */ NULL,
				  /* sliced_lines */ 1,
				  /* data_identifier */ 0x10,
				  /* exp_out_lines */ 0);

		test_ms_stuffing (buffer_size,
				  /* sliced */ (vbi_sliced *) -1,
				  /* sliced_lines */ 0,
				  /* data_identifier */ 0x10,
				  /* exp_out_lines */ 0);
	}
}

static void
test_ms_line			(vbi_service_set	service,
				 unsigned int		line,
				 vbi_bool		correct)
{
	vbi_sliced *sliced;
	uint8_t *buffer;
	unsigned int buffer_size;
	unsigned int n_lines;

	buffer = (uint8_t *) xmalloc (buffer_size = 20 * 46);
	sliced = alloc_sliced (n_lines = 1);

	sliced[0].id = service;
	sliced[0].line = line;

	if (0 == service) {
		assert (correct);

		/* Will be discarded without further checks. */

		assert_multiplex_sliced (buffer, buffer_size,
					 sliced, n_lines,
					 ALL_SERVICES,
					 ANY_DATA_IDENTIFIER,
					 ANY_STUFFING,
					 EXPECT_SUCCESS,
					 /* exp_errno */ 0,
					 /* exp_out_lines */ 0,
					 /* exp_out_data_size */ 0,
					 /* exp_consumed_lines */ 1);
	} else if (correct) {
		assert_multiplex_sliced (buffer, buffer_size,
					 sliced, n_lines,
					 ALL_SERVICES,
					 /* data_identifier */ 0x10,
					 ANY_STUFFING,
					 EXPECT_SUCCESS,
					 /* exp_errno */ 0,
					 /* exp_out_lines */ 1,
					 /* exp_out_data_size */ 1 * 46,
					 /* exp_consumed_lines */ 1);
	} else {
		assert_multiplex_sliced (buffer, buffer_size,
					 sliced, n_lines,
					 ALL_SERVICES,
					 ANY_DATA_IDENTIFIER,
					 ANY_STUFFING,
					 EXPECT_FAILURE,
					 VBI_ERR_LINE_NUMBER,
					 /* exp_out_lines */ 0,
					 /* exp_out_data_size */ 0,
					 /* exp_consumed_lines */ 0);
	}

	free (sliced);
	free (buffer);
}

static vbi_bool
correct_line_number		(vbi_service_set	service,
				 unsigned int		field,
				 unsigned int		line_offset)
{
	switch (service) {
	case 0:
		return TRUE;

	case VBI_SLICED_TELETEXT_B_625:
	case VBI_SLICED_TELETEXT_B_L10_625:
	case VBI_SLICED_TELETEXT_B_L25_625:
		// EN 301 775 section 4.5.2.
		/* Note an undefined line (0) in the second field
		   is permitted, but libzvbi cannot express such
		   line numbers. */
		if (0 == field && 0 == line_offset)
			return TRUE;
		return (line_offset >= 7 && line_offset <= 22);

	case VBI_SLICED_VPS:
		// EN 301 775 section 4.6.2.
		if (0 == field)
			return (16 == line_offset);
		return FALSE;

	case VBI_SLICED_WSS_625:
		// EN 301 775 section 4.7.2.
		if (0 == field)
			return (23 == line_offset);
		return FALSE;

	case VBI_SLICED_CAPTION_625:
	case VBI_SLICED_CAPTION_625_F1:
		// EN 301 775 section 4.8.2.
		if (0 == field)
			return (21 == line_offset);
		return FALSE;

	case VBI_SLICED_VBI_625:
		// EN 301 775 section 4.9.2.
		return (line_offset >= 7 && line_offset <= 23);

	default:
		break;
	}

	assert (0);

	return FALSE;
}

static void
test_multiplex_sliced_line_number_checks (void)
{
	vbi_sliced *sliced;
	uint8_t *buffer;
	vbi_service_set service;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int i;

	buffer = (uint8_t *) xmalloc (buffer_size = 20 * 46);
	sliced = alloc_sliced (n_lines = 1);

	sliced[0].id = 0;
	sliced[0].line = 100;

	assert_multiplex_sliced (buffer, buffer_size,
				 sliced, n_lines,
				 ALL_SERVICES,
				 ANY_DATA_IDENTIFIER,
				 ANY_STUFFING,
				 EXPECT_SUCCESS,
				 /* exp_errno */ 0,
				 /* exp_out_lines */ 0,
				 /* exp_out_data_size */ 0,
				 /* exp_consumed_lines */ 1);

	free (sliced);
	sliced = (vbi_sliced *) -1;

	free (buffer);
	buffer = (uint8_t *) -1;

	for (i = 0; i <= 31; ++i) {
		unsigned int j;

		for (j = 0; j < N_ELEMENTS (good_services); ++j) {
			vbi_bool correct;

			service = good_services[j];

			correct = correct_line_number (service, 0, i);
			test_ms_line (service, /* line */ i, correct);

			correct = correct_line_number (service, 1, i);
			test_ms_line (service, /* line */ i + 313, correct);
		}
	}

	for (i = 0; i < N_ELEMENTS (bad_line_numbers); ++i) {
		unsigned int j;

		for (j = 0; j < N_ELEMENTS (good_services); ++j) {
			service = good_services[j];

			test_ms_line (service,
				      /* line */ bad_line_numbers[i],
				      /* correct */ 0 == service);
		}
	}
}

static void
test_multiplex_sliced_service_checks
				(vbi_service_set	service)
{
	vbi_sliced *sliced;
	uint8_t *buffer;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int line;
	unsigned int i;

	buffer = (uint8_t *) xmalloc (buffer_size = 20 * 46);
	sliced = alloc_sliced (n_lines = 8);

	/* Verify the data service checks. */

	for (i = 0; i < 6; ++i) {
		sliced[i].id = VBI_SLICED_TELETEXT_B_625;
		sliced[i].line = i + 7;
	}

	if (service & VBI_SLICED_VPS)
		line = 16;
	else if (service & VBI_SLICED_CAPTION_625)
		line = 21;
	else if (service & VBI_SLICED_WSS_625)
		line = 23;
	else
		line = 13;

	sliced[6].id = service;
	sliced[6].line = line;

	sliced[7].id = VBI_SLICED_TELETEXT_B_625;
	sliced[7].line = 320;

	if (is_good_service (service)) {
		unsigned int exp_out_lines;

		if (VBI_SLICED_NONE == service)
			exp_out_lines = n_lines - 1;
		else
			exp_out_lines = n_lines;

		assert_multiplex_sliced (buffer, buffer_size,
					 sliced, n_lines,
					 ALL_SERVICES,
					 /* data identifier */ 0x10,
					 ANY_STUFFING,
					 EXPECT_SUCCESS,
					 /* exp_errno */ 0,
					 exp_out_lines,
					 exp_out_lines * 46,
					 /* exp_consumed_lines */ n_lines);
	} else {
		assert_multiplex_sliced (buffer, buffer_size,
					 sliced, n_lines,
					 ALL_SERVICES,
					 ANY_DATA_IDENTIFIER,
					 ANY_STUFFING,
					 EXPECT_FAILURE,
					 VBI_ERR_INVALID_SERVICE,
					 /* exp_out_lines */ 6,
					 /* exp_out_data_size */ 6 * 46,
					 /* exp_consumed_lines */ 6);
	}

	/* Verify the service filter. */

	if (-1u == service
	    || (VBI_SLICED_TELETEXT_B_625
		== (VBI_SLICED_TELETEXT_B_625 & service))) {
		assert_multiplex_sliced (buffer, buffer_size,
					 sliced, n_lines,
					 /* service_mask */ ~service,
					 ANY_DATA_IDENTIFIER,
					 ANY_STUFFING,
					 EXPECT_SUCCESS,
					 /* exp_errno */ 0,
					 /* exp_out_lines */ 0,
					 /* exp_out_data_size */ 0,
					 /* exp_consumed_lines */ n_lines);
	} else {
		assert_multiplex_sliced (buffer, buffer_size,
					 sliced, n_lines,
					 /* service_mask */ ~service,
					 ANY_DATA_IDENTIFIER,
					 ANY_STUFFING,
					 EXPECT_SUCCESS,
					 /* exp_errno */ 0,
					 /* exp_out_lines */ n_lines - 1,
					 /* exp_out_data_size */
					 (n_lines - 1) * 46,
					 /* exp_consumed_lines */ n_lines);
	}

	free (sliced);
	free (buffer);
}

static void
test_ms_good_line_order		(unsigned int		nth,
				 unsigned int		line)
{
	vbi_sliced *sliced;
	uint8_t *buffer;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int i;

	buffer = (uint8_t *) xmalloc (buffer_size = 20 * 46);
	sliced = alloc_sliced (n_lines = 8);

	for (i = 0; i < 4; ++i) {
		sliced[i].id = VBI_SLICED_TELETEXT_B_625;
		sliced[i].line = i + 7;
	}

	for (i = 4; i < 8; ++i) {
		sliced[i].id = VBI_SLICED_TELETEXT_B_625;
		sliced[i].line = i + 7 + 313;
	}

	assert (nth < n_lines);
	sliced[nth].line = line;

	assert_multiplex_sliced (buffer, buffer_size,
				 sliced, n_lines,
				 ALL_SERVICES,
				 ANY_DATA_IDENTIFIER,
				 ANY_STUFFING,
				 EXPECT_SUCCESS,
				 /* exp_errno */ 0,
				 /* exp_out_lines */ n_lines,
				 /* exp_out_data_size */ n_lines * 46,
				 /* exp_consumed_lines */ n_lines);

	free (sliced);
	free (buffer);
}

static void
test_ms_bad_line_order		(unsigned int		nth,
				 unsigned int		line,
				 unsigned int		bad)
{
	vbi_sliced *sliced;
	uint8_t *buffer;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int i;

	buffer = (uint8_t *) xmalloc (buffer_size = 20 * 46);
	sliced = alloc_sliced (n_lines = 8);
	
	for (i = 0; i < 4; ++i) {
		sliced[i].id = VBI_SLICED_TELETEXT_B_625;
		sliced[i].line = 7 + i;
	}

	for (i = 4; i < 8; ++i) {
		sliced[i].id = VBI_SLICED_TELETEXT_B_625;
		sliced[i].line = 313 + 7 + i - 4;
	}

	assert (nth < n_lines);
	sliced[nth].line = line;

	assert_multiplex_sliced (buffer, buffer_size,
				 sliced, n_lines,
				 ALL_SERVICES,
				 ANY_DATA_IDENTIFIER,
				 ANY_STUFFING,
				 EXPECT_FAILURE,
				 VBI_ERR_LINE_ORDER,
				 /* exp_out_lines */ bad,
				 /* exp_out_data_size */ bad * 46,
				 /* exp_consumed_lines */ bad);
	free (sliced);
	free (buffer);
}

static void
test_multiplex_sliced_line_order_checks (void)
{
	unsigned int i;

	for (i = 0; i < 8; ++i)
		test_ms_good_line_order (i, 0);

	test_ms_bad_line_order (0, 19, 1);
	test_ms_bad_line_order (0, 320, 1);

	for (i = 1; i < 3; ++i) {
		// 7, 8, 9, 10, 320, 321, 322, 323.
		test_ms_bad_line_order (i, 19, i + 1);
		test_ms_bad_line_order (i, 320 + i, i + 1);
		test_ms_bad_line_order (i + 4, 7 + i, i + 4);
	}

	test_ms_good_line_order (3, 19);
	test_ms_good_line_order (4, 19);

	// No line twice.
	test_ms_bad_line_order (2, 7, 2);
	test_ms_bad_line_order (2, 8, 2);
	test_ms_bad_line_order (2, 10, 3);
	test_ms_bad_line_order (6, 320, 6);
	test_ms_bad_line_order (6, 321, 6);
	test_ms_bad_line_order (6, 323, 7);
}

static void
test_ms_packet_offset_size	(unsigned int		offset,
				 unsigned int		buffer_size,
				 vbi_bool		stuffing)
{
	vbi_sliced *sliced;
	uint8_t *buffer;
	unsigned int n_lines;
	unsigned int max_lines;
	unsigned int exp_out_lines;
	unsigned int exp_out_data_size;
	vbi_bool full;
	unsigned int i;

	buffer = (uint8_t *) xmalloc (buffer_size + offset);
	sliced = alloc_sliced (max_lines = 24 - 7);

	n_lines = 0;

	exp_out_lines = 0;
	exp_out_data_size = 0;

	full = FALSE;

	for (i = 7; i < 16; ++i) {
		sliced[n_lines].id = VBI_SLICED_TELETEXT_B_625;
		sliced[n_lines++].line = i;
		if (exp_out_data_size + 46 > buffer_size) {
			full = TRUE;
		} else {
			exp_out_data_size += 46;
			++exp_out_lines;
		}
	}

	sliced[n_lines].id = VBI_SLICED_VPS;
	sliced[n_lines++].line = 16;
	if (exp_out_data_size + 16 > buffer_size) {
		full = TRUE;
	} else if (!full) {
		exp_out_data_size += 16;
		++exp_out_lines;
	}

	for (i = 17; i < 21; ++i) {
		sliced[n_lines].id = VBI_SLICED_TELETEXT_B_625;
		sliced[n_lines++].line = i;
		if (exp_out_data_size + 46 > buffer_size) {
			full = TRUE;
		} else if (!full) {
			exp_out_data_size += 46;
			++exp_out_lines;
		}
	}

	sliced[n_lines].id = VBI_SLICED_CAPTION_625;
	sliced[n_lines++].line = 21;
	if (exp_out_data_size + 5 > buffer_size) {
		full = TRUE;
	} else if (!full) {
		exp_out_data_size += 5;
		++exp_out_lines;
	}

	sliced[n_lines].id = VBI_SLICED_TELETEXT_B_625;
	sliced[n_lines++].line = 22;
	if (exp_out_data_size + 46 > buffer_size) {
		full = TRUE;
	} else if (!full) {
		exp_out_data_size += 46;
		++exp_out_lines;
	}

	sliced[n_lines].id = VBI_SLICED_WSS_625;
	sliced[n_lines++].line = 23;
	if (exp_out_data_size + 5 > buffer_size) {
		full = TRUE;
	} else if (!full) {
		exp_out_data_size += 5;
		++exp_out_lines;
	}

	assert (n_lines == max_lines);

	if (stuffing)
		exp_out_data_size = buffer_size;

	assert_multiplex_sliced (buffer + offset, buffer_size,
				 sliced, n_lines,
				 ALL_SERVICES,
				 ANY_DATA_IDENTIFIER,
				 stuffing,
				 EXPECT_SUCCESS,
				 /* exp_errno */ 0,
				 exp_out_lines,
				 exp_out_data_size,
				 /* exp_consumed_lines */ exp_out_lines);

	free (sliced);
	free (buffer);
}

static void
test_multiplex_sliced_packet_size (void)
{
	unsigned int i;

	for (i = 2; i < 2048; (i < 300) ? ++i : i += 7) {
		test_ms_packet_offset_size (0, i, STUFFING);
		test_ms_packet_offset_size (0, i, NO_STUFFING);
	}
}

static void
test_multiplex_sliced_data_identifier_checks
				(unsigned int		data_identifier)
{
	vbi_sliced *sliced;
	unsigned int buffer_size;
	unsigned int n_lines;
	vbi_bool fixed_length;

	sliced = alloc_sliced (n_lines = 1);

	sliced[0].id = VBI_SLICED_TELETEXT_B_625;
	sliced[0].line = 7;

	// EN 301 775 section 4.4.2.
	fixed_length = (data_identifier >= 0x10
			&& data_identifier <= 0x1F);

	for (buffer_size = 20 * 46 - 1;
	     buffer_size <= 20 * 46 + 1; ++buffer_size) {
		uint8_t *buffer;

		buffer = (uint8_t *) xmalloc (buffer_size);

		if (!fixed_length || 0 == buffer_size % 46) {
			assert_multiplex_sliced (buffer, buffer_size,
						 sliced, n_lines,
						 ALL_SERVICES,
						 data_identifier,
						 ANY_STUFFING,
						 EXPECT_SUCCESS,
						 /* exp_errno */ 0,
						 /* exp_out_lines */
						 n_lines,
						 /* exp_out_data_size */
						 n_lines * 46,
						 /* exp_consumed_lines */
						 n_lines);
		} else {
			assert_multiplex_sliced (buffer, buffer_size,
						 sliced, n_lines,
						 ALL_SERVICES,
						 data_identifier,
						 ANY_STUFFING,
						 EXPECT_FAILURE,
						 VBI_ERR_BUFFER_OVERFLOW,
						 /* exp_out_lines */ 0,
						 /* exp_out_data_size */ 0,
						 /* exp_consumed_lines */ 0);
		}

		free (buffer);
		buffer = (uint8_t *) -1;
	}

	free (sliced);
}

static void
test_multiplex_sliced_packet_size_checks (void)
{
	uint8_t *buffer;
	unsigned int buffer_size;

	for (buffer_size = 0; buffer_size <= 1; ++buffer_size) {
		buffer = (uint8_t *) xmalloc (buffer_size);

		assert_multiplex_sliced (buffer, buffer_size,
					 /* sliced */ (vbi_sliced *) -1,
					 /* sliced_lines */ 1,
					 ALL_SERVICES,
					 ANY_DATA_IDENTIFIER,
					 ANY_STUFFING,
					 EXPECT_FAILURE,
					 VBI_ERR_BUFFER_OVERFLOW,
					 /* exp_out_lines */ 0,
					 /* exp_out_data_size */ 0,
					 /* exp_consumed_lines */ 0);
		free (buffer);
		buffer = (uint8_t *) -1;
	}

	buffer = (uint8_t *) xmalloc (buffer_size = 2);

	assert_multiplex_sliced (buffer, buffer_size,
				 /* sliced */ (vbi_sliced *) -1,
				 /* sliced_lines */ 0,
				 ALL_SERVICES,
				 /* data_identifier */ 0x99,
				 STUFFING,
				 EXPECT_SUCCESS,
				 /* exp_errno */ 0,
				 /* exp_out_lines */ 0,
				 /* exp_out_data_size */ buffer_size,
				 /* exp_consumed_lines */ 0);
	free (buffer);
}

static void
test_multiplex_sliced_unaligned_packet (void)
{
	unsigned int i;

	for (i = 1; i < 16; ++i) {
		test_ms_packet_offset_size (i, 20 * 46, STUFFING);
	}
}

static void
test_multiplex_sliced_null_packet_checks (void)
{
	vbi_sliced *sliced;

	sliced = alloc_sliced (1);

	sliced[0].id = VBI_SLICED_TELETEXT_B_625;
	sliced[0].line = 7;

	assert_multiplex_sliced (/* buffer */ NULL,
				 /* buffer_size */ 20 * 46,
				 sliced,
				 /* sliced_lines */ 1,
				 ALL_SERVICES,
				 ANY_DATA_IDENTIFIER,
				 ANY_STUFFING,
				 EXPECT_FAILURE,
				 VBI_ERR_BUFFER_OVERFLOW,
				 /* exp_out_lines */ 0,
				 /* exp_out_data_size */ 0,
				 /* exp_consumed_lines */ 0);

	free (sliced);
}

void
test_multiplex_sliced		(void)
{
	unsigned int i;

	test_multiplex_sliced_null_packet_checks ();
	test_multiplex_sliced_packet_size_checks ();

	for (i = 0; i < N_ELEMENTS (data_identifiers); ++i) {
		unsigned int di = data_identifiers[i];
		test_multiplex_sliced_data_identifier_checks (di);
	}

	test_multiplex_sliced_line_order_checks ();

	for (i = 0; i < N_ELEMENTS (all_services); ++i)
		test_multiplex_sliced_service_checks (all_services[i]);

	test_multiplex_sliced_line_number_checks ();

	test_multiplex_sliced_packet_size ();
	test_multiplex_sliced_unaligned_packet ();
	test_multiplex_sliced_null_sliced ();
	test_multiplex_sliced_stuffing ();
}

/*
	Test vbi_dvb_multiplex_raw().
 */

static void
assert_multiplex_raw		(uint8_t * const 	p1,
				 const unsigned int	p1_size,
				 const uint8_t * const 	r1,
				 const unsigned int	r1_size,
				 unsigned int		data_identifier,
				 vbi_videostd_set	videostd_set,
				 const unsigned int	line,
				 const unsigned int	first_pixel_position,
				 const unsigned int	n_pixels_total,
				 vbi_bool		stuffing,
				 const vbi_bool	exp_success,
				 const int		exp_errno)
{
	uint8_t *p;
	uint8_t *rand_buffer;
	const uint8_t *r;
	unsigned int p_left;
	unsigned int r_left;
	unsigned int exp_out_dus;
	unsigned int exp_out_data_size;
	unsigned int exp_consumed_samples;
	unsigned int n_raw_dus;
	unsigned int n_stuffing_dus;
	vbi_bool fixed_length;
	vbi_bool success;

	if (ANY_DATA_IDENTIFIER == data_identifier) {
		assert_multiplex_raw (p1, p1_size,
				      r1, r1_size,
				      /* data_identifier */ 0x10,
				      videostd_set,
				      line,
				      first_pixel_position,
				      n_pixels_total,
				      stuffing,
				      exp_success,
				      exp_errno);
		data_identifier = 0x99;
	}

	if (ANY_STUFFING == stuffing) {
		assert_multiplex_raw (p1, p1_size,
				      r1, r1_size,
				      data_identifier,
				      videostd_set,
				      line,
				      first_pixel_position,
				      n_pixels_total,
				      /* stuffing */ FALSE,
				      exp_success,
				      exp_errno);
		stuffing = TRUE;
	}

	if (ANY_VIDEOSTD == videostd_set) {
		assert_multiplex_raw (p1, p1_size,
				      r1, r1_size,
				      data_identifier,
				      VBI_VIDEOSTD_SET_525_60,
				      line,
				      first_pixel_position,
				      n_pixels_total,
				      stuffing,
				      exp_success,
				      exp_errno);
		videostd_set = VBI_VIDEOSTD_SET_625_50;
	}

	if (NULL != p1) {
		rand_buffer = xralloc (p1_size);
		memcpy (p1, rand_buffer, p1_size);
	} else {
		rand_buffer = NULL;
	}

	p = p1;
	p_left = p1_size;

	r = r1;
	r_left = r1_size;

	success = vbi_dvb_multiplex_raw (&p, &p_left,
					  &r, &r_left,
					  data_identifier,
					  videostd_set,
					  line,
					  first_pixel_position,
					  n_pixels_total,
					  stuffing);
	assert (exp_success == success);

	if (!success) {
		(void) exp_errno;
		// XXX later: assert (exp_errno == errno);

		assert (p1 == p);
		assert (p1_size == p_left);

		assert (r1 == r);
		assert (r1_size == r_left);

		if (NULL != p1) {
			assert (0 == memcmp (p1, rand_buffer, p1_size));
		}

		goto finish;
	}

	// EN 301 775 section 4.4.2.
	fixed_length = (data_identifier >= 0x10
			&& data_identifier <= 0x1F);

	if (fixed_length) {
		exp_out_dus = MIN (p1_size / 46, (r1_size + 39) / 40);
		exp_out_data_size = exp_out_dus * 46;
		exp_consumed_samples = MIN (r1_size, exp_out_dus * 40);
	} else {
		exp_out_dus = MIN (p1_size / 257, r1_size / 251);
		exp_out_data_size = exp_out_dus * 257;
		exp_consumed_samples = exp_out_dus * 251;

		if (stuffing
		    && exp_out_data_size + 1 == p1_size) {
			/* One byte less to make room for a
			   stuffing data unit. */
			--exp_consumed_samples;
		} else if (exp_consumed_samples < r1_size
			   && exp_out_data_size + 7 <= p1_size) {
			unsigned int n_samples;

			n_samples = MIN (r1_size - exp_consumed_samples,
					 p1_size - 6 - exp_out_data_size);
			++exp_out_dus;
			exp_out_data_size += 6 + n_samples;
			exp_consumed_samples += n_samples;
		}
	}
	
	if (stuffing)
		exp_out_data_size = p1_size;

	assert (p1 + exp_out_data_size == p);
	assert (p1_size - exp_out_data_size == p_left);

	assert (r1 + r1_size - r_left == r);
	assert (r1_size - exp_consumed_samples == r_left);

	assert (0 == memcmp (p, rand_buffer + exp_out_data_size,
			     p1_size - exp_out_data_size));

	assert_raw_data_units_ok (&n_raw_dus,
				  &n_stuffing_dus,
				  p1,
				  exp_out_data_size,
				  fixed_length,
				  r1,
				  /* offset */ n_pixels_total - r1_size,
				  videostd_set,
				  line,
				  first_pixel_position,
				  n_pixels_total);

	assert (exp_out_dus == n_raw_dus);

	if (!stuffing)
		assert (0 == n_stuffing_dus);

 finish:
	free (rand_buffer);
}

static void
test_mr_size_offset		(unsigned int		raw_left,
				 unsigned int		first_pixel_position,
				 unsigned int		n_pixels_total)
{
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	vbi_bool exp_success;

	raw = xralloc (720);
	buffer = (uint8_t *) xmalloc (buffer_size = 20 * 46);

	if (0 == raw_left) {
		assert_multiplex_raw (buffer, buffer_size,
				      raw,
				      /* raw_size */ 0,
				      ANY_DATA_IDENTIFIER,
				      VBI_VIDEOSTD_SET_625_50,
				      /* line */ 10,
				      first_pixel_position,
				      n_pixels_total,
				      ANY_STUFFING,
				      EXPECT_FAILURE,
				      VBI_ERR_NO_RAW_DATA);
		goto finish;
	}

	if (0 == n_pixels_total)
		exp_success = FALSE;
	else if ((uint64_t) first_pixel_position + n_pixels_total
		 > (uint64_t) 720)
		exp_success = FALSE;
	else if (raw_left > n_pixels_total)
		exp_success = FALSE;
	else
		exp_success = TRUE;

	assert_multiplex_raw (buffer, buffer_size,
			      raw, raw_left,
			      ANY_DATA_IDENTIFIER,
			      VBI_VIDEOSTD_SET_625_50,
			      /* line */ 10,
			      first_pixel_position,
			      n_pixels_total,
			      ANY_STUFFING,
			      exp_success,
			      VBI_ERR_SAMPLE_NUMBER);

	free (buffer);

	buffer = (uint8_t *) xmalloc (buffer_size = 2 * 46);

	assert_multiplex_raw (buffer, buffer_size,
			      raw, raw_left,
			      ANY_DATA_IDENTIFIER,
			      VBI_VIDEOSTD_SET_625_50,
			      /* line */ 10,
			      first_pixel_position,
			      n_pixels_total,
			      ANY_STUFFING,
			      exp_success,
			      VBI_ERR_SAMPLE_NUMBER);
 finish:
	free (buffer);
	free (raw);
}

static void
test_multiplex_raw_size_offsets	(void)
{
	unsigned int i, j, k;

	for (i = 0; i < N_ELEMENTS (raw_offsets); ++i) {
		for (j = 0; j < N_ELEMENTS (raw_offsets); ++j) {
			for (k = 0; k < N_ELEMENTS (raw_offsets); ++k) {
				test_mr_size_offset (raw_offsets[i],
						     raw_offsets[j],
						     raw_offsets[k]);
			}
		}
	}
}

static void
test_mr_line			(unsigned int		line,
				 vbi_bool		exp_success_525,
				 vbi_bool		exp_success_625)
{
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int raw_size;

	buffer = (uint8_t *) xmalloc (buffer_size = 20 * 46);
	raw = xralloc (raw_size = 720);

	assert_multiplex_raw (buffer, buffer_size,
			      raw, raw_size,
			      ANY_DATA_IDENTIFIER,
			      VBI_VIDEOSTD_SET_525_60,
			      line,
			      /* first_pixel_position */ 0,
			      /* n_pixels_total */ raw_size,
			      ANY_STUFFING,
			      exp_success_525,
			      VBI_ERR_LINE_NUMBER);

	assert_multiplex_raw (buffer, buffer_size,
			      raw, raw_size,
			      ANY_DATA_IDENTIFIER,
			      VBI_VIDEOSTD_SET_625_50,
			      line,
			      /* first_pixel_position */ 0,
			      /* n_pixels_total */ raw_size,
			      ANY_STUFFING,
			      exp_success_625,
			      VBI_ERR_LINE_NUMBER);
	free (raw);
	free (buffer);
}

static void
test_multiplex_raw_line_number_checks (void)
{
	unsigned int line;
	unsigned int i;

	for (line = 0; line < 650; ++line) {
		vbi_bool exp_success_525;
		vbi_bool exp_success_625;

		exp_success_525 = FALSE;
		exp_success_625 = FALSE;

		// EN 301 775 table 13.
		if (line >= 7 && line <= 23) {
			exp_success_525 = TRUE;
			exp_success_625 = TRUE;
		} else if (line >= 263 + 7 && line <= 263 + 23) {
			exp_success_525 = TRUE;
		} else if (line >= 313 + 7 && line <= 313 + 23) {
			exp_success_625 = TRUE;
		}

		test_mr_line (line, exp_success_525, exp_success_625);
	}

	for (i = 0; i < N_ELEMENTS (border_uints); ++i)
		test_mr_line (border_uints[i], FALSE, FALSE);
}

static void
test_multiplex_raw_videostd_checks (void)
{
	uint8_t *buffer;
	unsigned int buffer_size;

	buffer = (uint8_t *) xmalloc (buffer_size = 20 * 46);

	assert_multiplex_raw (buffer, buffer_size,
			      /* raw */ (uint8_t *) -1,
			      /* raw_size */ 720,
			      ANY_DATA_IDENTIFIER,
			      /* videostd_set */ 0,
			      /* line */ 10,
			      /* first_pixel_position */ 0,
			      /* n_pixels_total */ 720,
			      ANY_STUFFING,
			      EXPECT_FAILURE,
			      VBI_ERR_AMBIGUOUS_VIDEOSTD);

#if 3 == VBI_VERSION_MINOR
	assert_multiplex_raw (buffer, buffer_size,
			      /* raw */ (uint8_t *) -1,
			      /* raw_size */ 720,
			      ANY_DATA_IDENTIFIER,
			      (VBI_VIDEOSTD_SET (VBI_VIDEOSTD_PAL_B) |
			       VBI_VIDEOSTD_SET (VBI_VIDEOSTD_NTSC_M)),
			      /* line */ 10,
			      /* first_pixel_position */ 0,
			      /* n_pixels_total */ 720,
			      ANY_STUFFING,
			      EXPECT_FAILURE,
			      VBI_ERR_AMBIGUOUS_VIDEOSTD);
#else
	assert_multiplex_raw (buffer, buffer_size,
			      /* raw */ (uint8_t *) -1,
			      /* raw_size */ 720,
			      ANY_DATA_IDENTIFIER,
			      (VBI_VIDEOSTD_SET_625_50 |
			       VBI_VIDEOSTD_SET_525_60),
			      /* line */ 10,
			      /* first_pixel_position */ 0,
			      /* n_pixels_total */ 720,
			      ANY_STUFFING,
			      EXPECT_FAILURE,
			      VBI_ERR_AMBIGUOUS_VIDEOSTD);
#endif
	free (buffer);
}

static void
test_multiplex_raw_data_identifier
				(unsigned int		data_identifier)
{
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int raw_size;
	vbi_bool fixed_length;

	raw = xralloc (raw_size = 720);

	// EN 301 775 section 4.4.2.
	fixed_length = (data_identifier >= 0x10
			&& data_identifier <= 0x1F);

	for (buffer_size = 20 * 46 - 1;
	     buffer_size <= 20 * 46 + 1; ++buffer_size) {
		uint8_t *buffer;
		vbi_bool exp_success;

		buffer = (uint8_t *) xmalloc (buffer_size);
		exp_success = (!fixed_length || 0 == buffer_size % 46);

		assert_multiplex_raw (buffer, buffer_size,
				      raw, raw_size,
				      data_identifier,
				      ANY_VIDEOSTD,
				      /* line */ 10,
				      /* first_pixel_position */ 0,
				      /* n_pixels_total */ raw_size,
				      ANY_STUFFING,
				      exp_success,
				      VBI_ERR_BUFFER_OVERFLOW);
		free (buffer);
		buffer = (uint8_t *) -1;
	}

	free (raw);
}

static void
test_multiplex_raw_unaligned_raw (void)
{
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int raw_size;
	unsigned int i;

	buffer = (uint8_t *) xmalloc (buffer_size = 20 * 46);
	raw = xralloc (15 + (raw_size = 720));

	for (i = 1; i < 16; ++i) {
		assert_multiplex_raw (buffer, buffer_size,
				      raw + i,
				      raw_size,
				      ANY_DATA_IDENTIFIER,
				      ANY_VIDEOSTD,
				      /* line */ 10,
				      /* first_pixel_position */ 0,
				      /* n_pixels_total */ raw_size,
				      ANY_STUFFING,
				      EXPECT_SUCCESS,
				      /* exp_errno */ 0);
	}

	free (raw);
	free (buffer);
}

static void
test_multiplex_raw_null_raw_checks (void)
{
	uint8_t *buffer;
	unsigned int buffer_size;

	buffer = (uint8_t *) xmalloc (buffer_size = 20 * 46);

	assert_multiplex_raw (buffer, buffer_size,
			      /* raw */ NULL,
			      /* raw_size */ 720,
			      ANY_DATA_IDENTIFIER,
			      ANY_VIDEOSTD,
			      /* line */ 10,
			      /* first_pixel_position */ 0,
			      /* n_pixels_total */ 720,
			      ANY_STUFFING,
			      EXPECT_FAILURE,
			      VBI_ERR_NO_RAW_DATA);
	free (buffer);
}

static void
test_mr_packet_size		(unsigned int		buffer_size,
				 unsigned int		data_identifier,
				 vbi_bool		exp_success)
{
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int raw_size;

	buffer = (uint8_t *) xmalloc (buffer_size);
	raw = xralloc (raw_size = 720);

	assert_multiplex_raw (buffer, buffer_size,
			      raw, raw_size,
			      data_identifier,
			      ANY_VIDEOSTD,
			      /* line */ 10,
			      /* first_pixel_position */ 0,
			      /* n_pixels_total */ raw_size,
			      ANY_STUFFING,
			      exp_success,
			      VBI_ERR_BUFFER_OVERFLOW);
	free (raw);
	free (buffer);
}

static void
test_multiplex_raw_packet_size_checks (void)
{
	unsigned int buffer_size;

	for (buffer_size = 0; buffer_size <= 1; ++buffer_size) {
		test_mr_packet_size (buffer_size,
				     ANY_DATA_IDENTIFIER,
				     EXPECT_FAILURE);
	}

	for (buffer_size = 2; buffer_size <= 45; ++buffer_size) {
		test_mr_packet_size (buffer_size,
				     /* data_identifier */ 0x10,
				     EXPECT_FAILURE);
		test_mr_packet_size (buffer_size,
				     /* data_identifier */ 0x99,
				     EXPECT_SUCCESS);
	}

	for (buffer_size = 46; buffer_size < 900; ++buffer_size) {
		test_mr_packet_size (buffer_size,
				     /* data_identifier */ 0x99,
				     EXPECT_SUCCESS);
	}

	for (buffer_size = 1 * 46;
	     buffer_size < 20 * 46; buffer_size += 46) {
		test_mr_packet_size (buffer_size,
				     /* data_identifier */ 0x10,
				     EXPECT_SUCCESS);
	}
}

static void
test_multiplex_raw_unaligned_packet (void)
{
	uint8_t *raw;
	unsigned int raw_size;
	unsigned int i;

	raw = xralloc (raw_size = 720);

	for (i = 1; i < 16; ++i) {
		uint8_t *buffer;
		unsigned int buffer_size;

		buffer = (uint8_t *) xmalloc (i + (buffer_size = 20 * 46));

		assert_multiplex_raw (buffer + i, buffer_size,
				      raw, raw_size,
				      ANY_DATA_IDENTIFIER,
				      ANY_VIDEOSTD,
				      /* line */ 10,
				      /* first_pixel_position */ 0,
				      /* n_pixels_total */ raw_size,
				      ANY_STUFFING,
				      EXPECT_SUCCESS,
				      /* exp_errno */ 0);
		free (buffer);
		buffer = (uint8_t *) -1;
	}

	free (raw);
}

static void
test_multiplex_raw_null_packet_checks (void)
{
	assert_multiplex_raw (/* packet */ NULL,
			      /* packet_size */ 20 * 46,
			      /* raw */ (uint8_t *) -1,
			      /* raw_size */ 720,
			      ANY_DATA_IDENTIFIER,
			      VBI_VIDEOSTD_SET_625_50,
			      /* line */ 10,
			      /* first_pixel_position */ 0,
			      /* n_pixels_total */ 720,
			      ANY_STUFFING,
			      EXPECT_FAILURE,
			      VBI_ERR_BUFFER_OVERFLOW);

	assert_multiplex_raw (/* packet */ (uint8_t *) -1,
			      /* packet_size */ 0,
			      /* raw */ (uint8_t *) -1,
			      /* raw_size */ 720,
			      ANY_DATA_IDENTIFIER,
			      VBI_VIDEOSTD_SET_625_50,
			      /* line */ 10,
			      /* first_pixel_position */ 0,
			      /* n_pixels_total */ 720,
			      ANY_STUFFING,
			      EXPECT_FAILURE,
			      VBI_ERR_BUFFER_OVERFLOW);
}

static void
test_multiplex_raw		(void)
{
	unsigned int i;

	test_multiplex_raw_null_packet_checks ();
	test_multiplex_raw_null_raw_checks ();
	test_multiplex_raw_packet_size_checks ();

	for (i = 0; i < N_ELEMENTS (data_identifiers); ++i)
		test_multiplex_raw_data_identifier (data_identifiers[i]);

	test_multiplex_raw_videostd_checks ();
	test_multiplex_raw_line_number_checks ();

	test_multiplex_raw_size_offsets ();

	test_multiplex_raw_unaligned_packet ();
	test_multiplex_raw_unaligned_raw ();
}

/*
	Test PES/TS multiplexer.
 */

static const vbi_sampling_par
good_par_625 = {
#if 3 == VBI_VERSION_MINOR
	/* videostd_set */	VBI_VIDEOSTD_SET_625_50,
	/* sampling_format */	VBI_PIXFMT_Y8,
	/* sampling_rate */	13500000,
	/* samples_per_line */	720,
	/* bytes_per_line */	720,
	/* offset */		132,
	/* start */		{ 7, 320 },
	/* count */		{ 17, 17 },
	/* interlaced */	FALSE,
	/* synchronous */	TRUE
#else
	/* scanning */		625,
	/* sampling_format */	VBI_PIXFMT_YUV420,
	/* sampling_rate */	13500000,
	/* bytes_per_line */	720,
	/* offset */		132,
	/* start */		{ 7, 320 },
	/* count */		{ 17, 17 },
	/* interlaced */	FALSE,
	/* synchronous */	TRUE
#endif
};

static const unsigned int
packet_sizes [] = {
	0,
	12,
	183,
	184,
	185,
	1234,
	65503,
	65504,
	65505,
	INT_MAX,
	((unsigned int) INT_MAX) + 1,
	UINT_MAX,
};

static vbi_bool
dvb_mux_pes_cb			(vbi_dvb_mux *		mx,
				 void *			user_data,
				 const uint8_t *	packet,
				 unsigned int		packet_size)
{
	unsigned int min, max;

	assert (mx == cb_multiplexer);
	assert (user_data == &cb_multiplexer);

	min = vbi_dvb_mux_get_pes_packet_min_size (mx);
	max = vbi_dvb_mux_get_pes_packet_max_size (mx);

	assert (0 == packet_size % 184);
	assert (packet_size >= min);
	assert (packet_size <= max);

	if (cb_pes_cmp) {
		// Compare against the output of the PES mux coroutine.
		assert (0 == memcmp (cb_pes_bp, packet, packet_size));
	} else {
		memcpy (cb_pes_bp, packet, packet_size);
	}

	cb_pes_bp += packet_size;

	return TRUE;
}

static vbi_bool
dvb_mux_ts_cb			(vbi_dvb_mux *		mx,
				 void *			user_data,
				 const uint8_t *	packet,
				 unsigned int		packet_size)
{
	unsigned int payload_unit_start_indicator;
	unsigned int continuity_counter;

	assert (mx == cb_multiplexer);
	assert (user_data == &cb_multiplexer);

	assert (188 == packet_size);

	/* sync_byte [8],
	   transport_error_indicator,
	   payload_unit_start_indicator,
	   transport_priority,
	   PID [13] == 0x1234,
	   transport_scrambling_control [2] == '00' (not scrambled),
	   adaptation_field_control [2] == '01'
	     (payload only, no adaption field),
	   continuity_counter [4] */
	assert (0x47 == packet[0]);
	assert (0x12 == (packet[1] & ~0x40));
	assert (0x34 == packet[2]);
	assert (0x10 == (packet[3] & ~0x0F));

	payload_unit_start_indicator = !!(packet[1] & 0x40);

	assert ((0x00 == packet[4] &&
		 0x00 == packet[5] &&
		 0x01 == packet[6] &&
		 0xBD == packet[7]) == payload_unit_start_indicator);

	continuity_counter = packet[3] & 0x0F;

	if (-1 != cb_next_continuity_counter)
		assert ((unsigned int) cb_next_continuity_counter
			== continuity_counter);

	cb_next_continuity_counter = (continuity_counter + 1) & 0xF;

	if (cb_pes_cmp) {
		// Compare against the output of the PES mux coroutine.
		assert (0 == memcmp (cb_pes_bp, packet + 4, 184));
	} else {
		memcpy (cb_pes_bp, packet + 4, 184);
	}

	cb_pes_bp += 184;

	if (cb_ts_cmp) {
		// Compare against the output of the TS mux coroutine.
		assert (0 == memcmp (cb_ts_bp, packet, 188));
	} else {
		memcpy (cb_ts_bp, packet, 188);
	}

	cb_ts_bp += 188;

	return TRUE;
}

static void
assert_dvb_mux_cor		(vbi_dvb_mux *		mx,
				 uint8_t * const	p1,
				 const unsigned int	p1_size,
				 const vbi_sliced * const s1,
				 const unsigned int	s1_lines,
				 const vbi_service_set	service_mask,
				 const uint8_t * const	raw,
				 const vbi_sampling_par * const sp,
				 const int64_t		pts,
				 const vbi_bool 	exp_success,
				 const int		exp_errno,
				 const unsigned int	exp_consumed_lines)
{
	vbi_sampling_par sampling_par;
	vbi_dvb_mux *mx2;
	vbi_dvb_mux *mx3;
	vbi_dvb_mux *mx4;
	uint8_t *p;
	uint8_t *rand_buffer;
	uint8_t *ts_buffer;
	uint8_t *ts_rand_buffer;
	const vbi_sliced *s;
	unsigned int p_left;
	unsigned int s_left;
	unsigned int pes_bytes_out;
	unsigned int ts_bytes_out;
	unsigned int ts_buffer_size;
	unsigned int data_identifier;
	unsigned int min_size;
	unsigned int max_size;
	vbi_bool success;

	data_identifier = vbi_dvb_mux_get_data_identifier (mx);
	min_size = vbi_dvb_mux_get_pes_packet_min_size (mx);
	max_size = vbi_dvb_mux_get_pes_packet_max_size (mx);

	if (NULL != p1) {
		rand_buffer = xralloc (p1_size);
		memcpy (p1, rand_buffer, p1_size);

		ts_buffer_size = p1_size * 188 / 184;
		ts_buffer = (uint8_t *) xmalloc (ts_buffer_size);

		ts_rand_buffer = (uint8_t *) xralloc (ts_buffer_size);
		memcpy (ts_buffer, ts_rand_buffer, ts_buffer_size);
	} else {
		rand_buffer = NULL;
		ts_buffer_size = 0;
		ts_buffer = NULL;
		ts_rand_buffer = NULL;
	}

	if (NULL != sp)
		sampling_par = *sp;
	else
		memset (&sampling_par, -1, sizeof (sampling_par));

	p = p1;
	p_left = p1_size;

	s = s1;
	s_left = s1_lines;

	success = vbi_dvb_mux_cor (mx,
				    &p, &p_left,
				    &s, &s_left,
				    service_mask,
				    raw, sp,
				    pts);

	if (NULL == s1 || 0 == s1_lines)
		assert (FALSE == success);
	else
		assert (exp_success == success);

	pes_bytes_out = p1_size - p_left;

	if (success) {
		unsigned int n_sliced_dus;
		unsigned int n_raw_dus;
		unsigned int n_stuffing_dus;

		assert (p1 + pes_bytes_out == p);

		assert (s1 + exp_consumed_lines == s);
		assert (s1_lines - exp_consumed_lines == s_left);

		assert_pes_packet_ok (&n_sliced_dus,
				      &n_raw_dus,
				      &n_stuffing_dus,
				      p1,
				      pes_bytes_out,
				      data_identifier,
				      min_size,
				      max_size);

		assert (s1_lines >= n_sliced_dus);

		if (0 == service_mask) {
			assert (0 == n_sliced_dus);
			assert (0 == n_raw_dus);
		}

		if (NULL == raw || NULL == sp)
			assert (0 == n_raw_dus);

		assert_pes_conversion_ok (p1,
					  pes_bytes_out,
					  s1,
					  s1_lines,
					  service_mask,
					  pts);
	} else {
		(void) exp_errno;
		// assert (exp_errno == errno);

		assert (p1 == p);
		assert (p1_size == p_left);

		assert (s1 + exp_consumed_lines == s);
		assert (s1_lines - exp_consumed_lines == s_left);
	}

	if (NULL != p1) {
		assert (0 == memcmp (p, rand_buffer + pes_bytes_out,
				     p1_size - pes_bytes_out));
	}

	mx = (vbi_dvb_mux *) -1;

	/* Verify that the PES callback gives exactly the same result. */

	mx2 = vbi_dvb_pes_mux_new (/* callback */ dvb_mux_pes_cb,
				    /* user_data */ &cb_multiplexer);
	assert (NULL != mx2);

	success = vbi_dvb_mux_set_data_identifier (mx2, data_identifier);
	assert (TRUE == success);

	success = vbi_dvb_mux_set_pes_packet_size (mx2, min_size, max_size);
	assert (TRUE == success);

	cb_pes_bp = p1;
	cb_ts_bp = NULL;
	cb_pes_cmp = TRUE;
	cb_ts_cmp = TRUE;
	cb_multiplexer = mx2;

	if (NULL == s1 || 0 == s1_lines)
		cb_pes_cmp = FALSE;

	success = vbi_dvb_mux_feed (mx2,
				     s1, s1_lines,
				     service_mask,
				     raw, sp,
				     pts);

	assert (exp_success == success);

	if (NULL == s1 || 0 == s1_lines) {
		pes_bytes_out = cb_pes_bp - p1;
	} else {
		assert ((unsigned int)(cb_pes_bp - p1) == pes_bytes_out);
	}

	if (success) {
		unsigned int n_sliced_dus;
		unsigned int n_raw_dus;
		unsigned int n_stuffing_dus;

		assert_pes_packet_ok (&n_sliced_dus,
				      &n_raw_dus,
				      &n_stuffing_dus,
				      p1,
				      pes_bytes_out,
				      data_identifier,
				      min_size,
				      max_size);

		assert (s1_lines >= n_sliced_dus);

		if (NULL == raw || NULL == sp)
			assert (0 == n_raw_dus);
	} else {
		(void) exp_errno;
		// assert (exp_errno == errno);
	}

	vbi_dvb_mux_delete (mx2);
	mx2 = (vbi_dvb_mux *) -1;

	/* Test the TS coroutine. */

	mx3 = vbi_dvb_ts_mux_new (/* pid */ 0x1234,
				   /* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx3);

	success = vbi_dvb_mux_set_data_identifier (mx3, data_identifier);
	assert (TRUE == success);

	success = vbi_dvb_mux_set_pes_packet_size (mx3, min_size, max_size);
	assert (TRUE == success);

	p = ts_buffer;
	p_left = ts_buffer_size;

	s = s1;
	s_left = s1_lines;

	success = vbi_dvb_mux_cor (mx3,
				    &p, &p_left,
				    &s, &s_left,
				    service_mask,
				    raw, sp,
				    pts);

	if (NULL == s1 || 0 == s1_lines) {
		assert (FALSE == success);
	} else {
		assert (exp_success == success);
	}

	ts_bytes_out = ts_buffer_size - p_left;

	if (success) {
		assert (ts_buffer + ts_bytes_out == p);

		assert (s1 + exp_consumed_lines == s);
		assert (s1_lines - exp_consumed_lines == s_left);
	} else {
		(void) exp_errno;
		// assert (exp_errno == errno);

		assert (ts_buffer == p);
		assert (ts_buffer_size == p_left);

		assert (s1 + exp_consumed_lines == s);
		assert (s1_lines - exp_consumed_lines == s_left);
	}

	assert (0 == memcmp (p, ts_rand_buffer + ts_bytes_out,
			     ts_buffer_size - ts_bytes_out));

	vbi_dvb_mux_delete (mx3);
	mx3 = (vbi_dvb_mux *) -1;

	/* Verify that the TS callback and the TS coroutine give
	   exactly the same result as the PES coroutine. */
	
	mx4 = vbi_dvb_ts_mux_new (/* pid */ 0x1234,
				   /* callback */ dvb_mux_ts_cb,
				   /* user_data */ &cb_multiplexer);
	assert (NULL != mx4);

	success = vbi_dvb_mux_set_data_identifier (mx4, data_identifier);
	assert (TRUE == success);

	success = vbi_dvb_mux_set_pes_packet_size (mx4, min_size, max_size);
	assert (TRUE == success);

	cb_pes_bp = p1;
	cb_ts_bp = ts_buffer;
	cb_pes_cmp = TRUE;
	cb_ts_cmp = TRUE;
	cb_multiplexer = mx4;
	cb_next_continuity_counter = -1;

	if (NULL == s1 || 0 == s1_lines)
		cb_ts_cmp = FALSE;

	success = vbi_dvb_mux_feed (mx4,
				     s1, s1_lines,
				     service_mask,
				     raw, sp,
				     pts);

	assert (exp_success == success);

	assert ((unsigned int)(cb_pes_bp - p1) == pes_bytes_out);

	if (NULL == s1 || 0 == s1_lines) {
		ts_bytes_out = cb_ts_bp - ts_buffer;
	} else {
		assert ((unsigned int)(cb_ts_bp - ts_buffer) == ts_bytes_out);
	}

	if (!success) {
		(void) exp_errno;
		// assert (exp_errno == errno);
	}

	vbi_dvb_mux_delete (mx4);
	mx4 = (vbi_dvb_mux *) -1;

	free (ts_rand_buffer);
	free (ts_buffer);
	free (rand_buffer);
}

static void
alloc_init_sliced		(vbi_sliced **		sliced_p,
				 unsigned int *		n_lines_p)
{
	const unsigned int max_lines = 2 * (23 - 7) + 1;
	vbi_sliced *sliced;
	unsigned int n_lines;
	unsigned int field;

	sliced = alloc_sliced (max_lines);

	n_lines = 0;

	for (field = 0; field < 2; ++field) {
		unsigned int j;

		for (j = 7; j < 15; ++j) {
			sliced[n_lines].id = VBI_SLICED_TELETEXT_B_625;
			sliced[n_lines++].line = field * 313 + j;
		}

		if (0 == field)
			sliced[n_lines].id = VBI_SLICED_VBI_625;
		else
			sliced[n_lines].id = VBI_SLICED_TELETEXT_B_625;
		sliced[n_lines++].line = field * 313 + 15;

		if (0 == field)
			sliced[n_lines].id = VBI_SLICED_VPS;
		else
			sliced[n_lines].id = VBI_SLICED_TELETEXT_B_625;
		sliced[n_lines++].line = field * 313 + 16;

		for (j = 17; j < 20; ++j) {
			sliced[n_lines].id = VBI_SLICED_TELETEXT_B_625;
			sliced[n_lines++].line = field * 313 + j;
		}

		if (1 == field)
			sliced[n_lines].id = VBI_SLICED_VBI_625;
		else
			sliced[n_lines].id = VBI_SLICED_TELETEXT_B_625;
		sliced[n_lines++].line = field * 313 + 20;

		if (0 == field)
			sliced[n_lines].id = VBI_SLICED_CAPTION_625;
		else
			sliced[n_lines].id = VBI_SLICED_TELETEXT_B_625;
		sliced[n_lines++].line = field * 313 + 21;

		sliced[n_lines].id = VBI_SLICED_TELETEXT_B_625;
		sliced[n_lines++].line = field * 313 + 22;

		if (0 == field) {
			sliced[n_lines].id = VBI_SLICED_WSS_625;
			sliced[n_lines++].line = 23;
		}
	}

	assert (n_lines == max_lines);

	*sliced_p = sliced;
	*n_lines_p = n_lines;
}

static void
test_dvb_mux_cor_partial_reads_and_reset
				(unsigned int		pid)
{
	static const unsigned int steps [] = {
		1, 46, 184, 188, 999999, INT_MAX,
		(unsigned int) INT_MAX + 1, UINT_MAX
	};
	vbi_sliced *sliced;
	const vbi_sliced *s;
	uint8_t *buffer1;
	uint8_t *buffer2;
	uint8_t *p;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int p_left;
	unsigned int s_left;
	unsigned int i;
	vbi_bool success;

	vbi_dvb_mux *mx;

	if (0 == pid) {
		mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
					   /* user_data */ NULL);
		buffer_size = 68 * 46;
	} else {
		mx = vbi_dvb_ts_mux_new (pid,
					  /* callback */ NULL,
					  /* user_data */ NULL);
		buffer_size = 68 * 46 * 188 / 184;
	}

	assert (NULL != mx);

	alloc_init_sliced (&sliced, &n_lines);

	raw = alloc_raw_frame (&good_par_625);

	buffer1 = (uint8_t *) xralloc (buffer_size);
	buffer2 = (uint8_t *) xmalloc (buffer_size);

	p = buffer1;
	p_left = buffer_size;

	s = sliced;
	s_left = n_lines;

	success = vbi_dvb_mux_cor (mx,
				    &p, &p_left,
				    &s, &s_left,
				    ALL_SERVICES,
				    raw,
				    &good_par_625,
				    /* pts */ 0x1234567);
	assert (TRUE == success);
	assert (0 == p_left);
	assert (0 == s_left);

	for (i = 0; i < N_ELEMENTS (steps); ++i) {
		p = buffer2;
		p_left = buffer_size / 2;

		s = sliced;
		s_left = n_lines;

		success = vbi_dvb_mux_cor (mx,
					    &p, &p_left,
					    &s, &s_left,
					    ALL_SERVICES,
					    raw,
					    &good_par_625,
					    /* pts */ 0x1234567);
		assert (TRUE == success);

		// Discard the second half.
		vbi_dvb_mux_reset (mx);

		memset_rand (buffer2, buffer_size);

		p = buffer2;

		s = sliced;
		s_left = n_lines;

		do {
			p_left = steps[i];

			success = vbi_dvb_mux_cor (mx,
						    &p, &p_left,
						    &s, &s_left,
						    ALL_SERVICES,
						    raw,
						    &good_par_625,
						    /* pts */ 0x1234567);
			assert (TRUE == success);
		} while (s_left > 0);

		assert (buffer2 + buffer_size == p);

		if (0 == pid) {
			assert (0 == memcmp (buffer1, buffer2, buffer_size));
		} else {
			unsigned int j;

			for (j = 0; j < buffer_size; j += 188) {
				assert (buffer1[j + 0] == buffer2[j + 0]);
				assert (buffer1[j + 1] == buffer2[j + 1]);
				assert (buffer1[j + 2] == buffer2[j + 2]);

				/* Ignore continuity_counter change
				   due to the reset. (The function
				   intentionally resets not to zero.) */
				assert (0 == ((buffer1[j + 3]
					       ^ buffer2[j + 3]) & 0xF0));

				assert (0 == memcmp (buffer1 + j + 4,
						     buffer2 + j + 4, 184));
			}

			assert (j == buffer_size);
		}
	}

	free (buffer2);
	free (buffer1);
	free (raw);
	free (sliced);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_cor_service_mask (void)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);
	alloc_init_sliced (&sliced, &n_lines);
	raw = alloc_raw_frame (&good_par_625);

	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    sliced, n_lines,
			    (VBI_SLICED_VPS |
			     VBI_SLICED_WSS_625),
			    /* raw */ NULL,
			    &good_par_625,
			    /* pts */ 0x1234567,
			    EXPECT_SUCCESS,
			    /* exp_errno */ 0,
			    /* exp_consumed_lines */ n_lines);

	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    sliced, n_lines,
			    /* service_mask */ 0,
			    raw, &good_par_625,
			    /* pts */ 0x1234567,
			    EXPECT_SUCCESS,
			    /* exp_errno */ 0,
			    /* exp_consumed_lines */ n_lines);

	free (raw);
	free (sliced);
	free (buffer);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_cor_pts (void)
{
	static const int64_t ptss [] = {
		0x8000000000000000ll, -1, 0, 0x7FFFFFFFFFFFFFFFll,
	};
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int i;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);
	alloc_init_sliced (&sliced, &n_lines);
	raw = alloc_raw_frame (&good_par_625);

	for (i = 0; i < N_ELEMENTS (ptss); ++i) {
		assert_dvb_mux_cor (mx,
				    buffer, buffer_size,
				    sliced, n_lines,
				    ALL_SERVICES,
				    raw, &good_par_625,
				    ptss[i],
				    EXPECT_SUCCESS,
				    /* exp_errno */ 0,
				    /* exp_consumed_lines */ n_lines);
	}

	free (raw);
	free (sliced);
	free (buffer);

	vbi_dvb_mux_delete (mx);
}

static void
test_mx_raw_offset		(vbi_dvb_mux *		mx,
				 const vbi_sliced *	sliced,
				 unsigned int		n_lines,
				 const uint8_t *	raw,
				 unsigned int		bytes_per_line,
				 unsigned int		samples_per_line,
				 unsigned int		offset)
{
	vbi_sampling_par sp;
	uint8_t *buffer;
	unsigned int buffer_size;

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);

	sp = good_par_625;
	sp.bytes_per_line = bytes_per_line;
	sp.sp_samples_per_line = samples_per_line;
	sp.offset = offset;

	if (offset < 132
	    || (uint64_t) offset + samples_per_line > 132 + 720
	    || 0 == samples_per_line
	    || samples_per_line > bytes_per_line) {
		assert_dvb_mux_cor (mx,
				    buffer, buffer_size,
				    /* sliced */ (vbi_sliced *) -1,
				    /* sliced_lines */ 17,
				    ALL_SERVICES,
				    /* raw */ (uint8_t *) -1,
				    &sp,
				    /* pts */ 0x1234567,
				    EXPECT_FAILURE,
				    VBI_ERR_SAMPLING_PAR,
				    /* exp_consumed_lines */ 0);
	} else if (bytes_per_line < INT_MAX) {
		assert_dvb_mux_cor (mx,
				    buffer, buffer_size,
				    sliced, n_lines,
				    ALL_SERVICES,
				    raw, &sp,
				    /* pts */ 0x1234567,
				    EXPECT_SUCCESS,
				    /* exp_errno */ 0,
				    /* exp_consumed_lines */ n_lines);
	}

	free (buffer);
}

static void
assert_dvb_mux_cor_bad_par	(vbi_dvb_mux *		mx,
				 vbi_sampling_par *	sp)
{
	uint8_t *buffer;
	unsigned int buffer_size;

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);

	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    /* sliced */ (vbi_sliced *) -1,
			    /* sliced_lines */ 17,
			    ALL_SERVICES,
			    /* raw */ (uint8_t *) -1,
			    sp,
			    /* pts */ 0x1234567,
			    EXPECT_FAILURE,
			    VBI_ERR_SAMPLING_PAR,
			    /* exp_consumed_lines */ 0);
	free (buffer);
}

static void
test_dvb_mux_cor_sampling_parameter_checks (void)
{
	vbi_sampling_par sp;
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int i;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	// FIXME: Test vbi_valid_sampling_par_log().

#if 3 == VBI_VERSION_MINOR
	sp = good_par_625;
	sp.videostd_set = 0,
	assert_dvb_mux_cor_bad_par (mx, &sp);

	sp = good_par_625;
	sp.videostd_set = VBI_VIDEOSTD_SET_525_60;
	assert_dvb_mux_cor_bad_par (mx, &sp);

	sp = good_par_625;
	sp.videostd_set = (VBI_VIDEOSTD_SET (VBI_VIDEOSTD_PAL_B) |
			   VBI_VIDEOSTD_SET (VBI_VIDEOSTD_NTSC_M));
	assert_dvb_mux_cor_bad_par (mx, &sp);
#else
	sp = good_par_625;
	sp.scanning = 0;
	assert_dvb_mux_cor_bad_par (mx, &sp);

	sp = good_par_625;
	sp.scanning = 525;
	assert_dvb_mux_cor_bad_par (mx, &sp);
#endif

	sp = good_par_625;
	sp.sampling_format = VBI_PIXFMT_YUYV;
	assert_dvb_mux_cor_bad_par (mx, &sp);

	sp = good_par_625;
	sp.sampling_rate = 27000000;
	assert_dvb_mux_cor_bad_par (mx, &sp);

	sp = good_par_625;
	sp.synchronous = FALSE;
	assert_dvb_mux_cor_bad_par (mx, &sp);

	alloc_init_sliced (&sliced, &n_lines);
	raw = alloc_raw_frame (&good_par_625);

	for (i = 0; i < N_ELEMENTS (raw_offsets); ++i) {
		unsigned int j;

		for (j = 0; j < N_ELEMENTS (raw_offsets); ++j) {
			unsigned int k;

			for (k = 0; k < N_ELEMENTS (raw_offsets); ++k) {
				test_mx_raw_offset (mx,
						    sliced, n_lines,
						    raw,
						    raw_offsets[i],
						    raw_offsets[j],
						    raw_offsets[k]);
			}
		}
	}

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);

	sp = good_par_625;
	sp.interlaced = TRUE;
	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    sliced, n_lines,
			    ALL_SERVICES,
			    raw, &sp,
			    /* pts */ 0x1234567,
			    EXPECT_SUCCESS,
			    /* exp_errno */ 0,
			    /* exp_consumed_lines */ n_lines);

	free (buffer);
	free (raw);
	free (sliced);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_cor_unaligned_raw	(void)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int i;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);
	sliced = alloc_sliced (n_lines = 1);
	raw = alloc_raw_frame (&good_par_625);

	sliced[0].id = VBI_SLICED_VBI_625;
	sliced[0].line = 7;

	for (i = 1; i < 16; ++i) {
		assert_dvb_mux_cor (mx,
				    buffer, buffer_size,
				    sliced, n_lines,
				    ALL_SERVICES,
				    raw + i,
				    &good_par_625,
				    /* pts */ 0x1234567,
				    EXPECT_SUCCESS,
				    /* exp_errno */ 0,
				    /* exp_consumed_lines */ 1);
	}

	free (raw);
	free (sliced);
	free (buffer);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_cor_null_raw_or_sp_checks (void)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);
	alloc_init_sliced (&sliced, &n_lines);
	raw = alloc_raw_frame (&good_par_625);

	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    sliced, n_lines,
			    ALL_SERVICES,
			    /* raw */ NULL,
			    &good_par_625,
			    /* pts */ 0x1234567,
			    EXPECT_FAILURE,
			    VBI_ERR_NO_RAW_DATA,
			    /* exp_consumed_lines */ 15 - 7);

	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    sliced, n_lines,
			    ALL_SERVICES,
			    raw,
			    /* sp */ NULL,
			    /* pts */ 0x1234567,
			    EXPECT_FAILURE,
			    VBI_ERR_NO_RAW_DATA,
			    /* exp_consumed_lines */ 15 - 7);

	free (raw);
	free (sliced);
	free (buffer);

	vbi_dvb_mux_delete (mx);
}

static void
test_mx_line			(const vbi_sampling_par *sp,
				 vbi_service_set	service,
				 unsigned int		line,
				 vbi_bool		correct)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);
	sliced = alloc_sliced (n_lines = 1);
	raw = alloc_raw_frame (sp);

	sliced[0].id = service;
	sliced[0].line = line;

	if (correct) {
		assert_dvb_mux_cor (mx,
				    buffer, buffer_size,
				    sliced, n_lines,
				    ALL_SERVICES,
				    raw, sp,
				    /* pts */ 0x1234567,
				    EXPECT_SUCCESS,
				    /* exp_errno */ 0,
				    /* exp_consumed_lines */ 1);
	} else {
		assert_dvb_mux_cor (mx,
				    buffer, buffer_size,
				    sliced, n_lines,
				    ALL_SERVICES,
				    raw, sp,
				    /* pts */ 0x1234567,
				    EXPECT_FAILURE,
				    VBI_ERR_LINE_NUMBER,
				    /* exp_consumed_lines */ 0);
	}

	free (raw);
	free (sliced);
	free (buffer);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_cor_sp_line_number_checks (void)
{
	vbi_sampling_par sp;

	sp = good_par_625;
	sp.count[0] = 0;

	test_mx_line (&sp, VBI_SLICED_VBI_625, 7, EXPECT_FAILURE);

	sp = good_par_625;
	sp.count[1] = 0;

	test_mx_line (&sp, VBI_SLICED_VBI_625, 320, EXPECT_FAILURE);

	sp = good_par_625;
	sp.start[0] = 8;
	sp.count[0] = 22 - 8 + 1;
	sp.start[1] = 313 + 8;
	sp.count[1] = 22 - 8 + 1;

	test_mx_line (&sp, VBI_SLICED_VBI_625, 7, EXPECT_FAILURE);
	test_mx_line (&sp, VBI_SLICED_VBI_625, 8, EXPECT_SUCCESS);
	test_mx_line (&sp, VBI_SLICED_VBI_625, 22, EXPECT_SUCCESS);
	test_mx_line (&sp, VBI_SLICED_VBI_625, 23, EXPECT_FAILURE);
	test_mx_line (&sp, VBI_SLICED_VBI_625, 313 + 7, EXPECT_FAILURE);
	test_mx_line (&sp, VBI_SLICED_VBI_625, 313 + 8, EXPECT_SUCCESS);
	test_mx_line (&sp, VBI_SLICED_VBI_625, 313 + 22, EXPECT_SUCCESS);
	test_mx_line (&sp, VBI_SLICED_VBI_625, 313 + 23, EXPECT_FAILURE);
}

static void
test_dvb_mux_cor_line_number_checks (void)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	vbi_service_set service;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int i;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);
	sliced = alloc_sliced (n_lines = 1);

	sliced[0].id = 0;
	sliced[0].line = 100;

	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    sliced, n_lines,
			    ALL_SERVICES,
			    /* raw */ NULL,
			    /* sp */ NULL,
			    /* pts */ 0x1234567,
			    EXPECT_SUCCESS,
			    /* exp_errno */ 0,
			    /* exp_consumed_lines */ 1);

	free (sliced);
	sliced = (vbi_sliced *) -1;

	free (buffer);
	buffer = (uint8_t *) -1;

	vbi_dvb_mux_delete (mx);
	mx = (vbi_dvb_mux *) -1;

	for (i = 0; i <= 31; ++i) {
		vbi_bool correct;
		unsigned int j;

		for (j = 0; j < N_ELEMENTS (good_services); ++j) {
			service = good_services[j];

			correct = correct_line_number (service, 0, i);
			test_mx_line (&good_par_625, service,
				      /* line */ i, correct);

			correct = correct_line_number (service, 1, i);
			test_mx_line (&good_par_625, service,
				      /* line */ i + 313, correct);
		}

		service = VBI_SLICED_VBI_625;

		correct = correct_line_number (service, 0, i);
		test_mx_line (&good_par_625, service,
			      /* line */ i, correct);

		correct = correct_line_number (service, 0, i);
		test_mx_line (&good_par_625, service,
			      /* line */ i + 313, correct);
	}

	for (i = 0; i < N_ELEMENTS (bad_line_numbers); ++i) {
		unsigned int j;

		for (j = 0; j < N_ELEMENTS (good_services); ++j) {
			service = good_services[j];

			test_mx_line (&good_par_625,
				      service,
				      /* line */ bad_line_numbers[i],
				      /* correct */ 0 == service);
		}

		test_mx_line (&good_par_625,
			      VBI_SLICED_VBI_625,
			      /* line */ bad_line_numbers[i],
			      /* correct */ FALSE);
	}
}

static void
test_dvb_mux_cor_service_checks (void)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int i;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);
	sliced = alloc_sliced (n_lines = 8);
	raw = alloc_raw_frame (&good_par_625);

	for (i = 0; i < 6; ++i) {
		sliced[i].id = VBI_SLICED_TELETEXT_B_625;
		sliced[i].line = i + 7;
	}

	sliced[7].id = VBI_SLICED_TELETEXT_B_625;
	sliced[7].line = 320;

	for (i = 0; i < N_ELEMENTS (all_services); ++i) {
		vbi_service_set service;
		unsigned int line;

		service = all_services[i];

		if (service & VBI_SLICED_VPS)
			line = 16;
		else if (service & VBI_SLICED_CAPTION_625)
			line = 21;
		else if (service & VBI_SLICED_WSS_625)
			line = 23;
		else
			line = 13;

		sliced[6].id = service;
		sliced[6].line = line;

		if (VBI_SLICED_VBI_625 == service
		    || is_good_service (service)) {
			assert_dvb_mux_cor (mx,
					    buffer, buffer_size,
					    sliced, n_lines,
					    ALL_SERVICES,
					    raw, &good_par_625,
					    /* pts */ 0x1234567,
					    EXPECT_SUCCESS,
					    /* exp_errno */ 0,
					    /* exp_consumed_lines */ n_lines);
		} else {
			assert_dvb_mux_cor (mx,
					    buffer, buffer_size,
					    sliced, n_lines,
					    ALL_SERVICES,
					    raw, &good_par_625,
					    /* pts */ 0x1234567,
					    EXPECT_FAILURE,
					    VBI_ERR_INVALID_SERVICE,
					    /* exp_consumed_lines */ 6);
		}
	}

	free (raw);
	free (sliced);
	free (buffer);

	vbi_dvb_mux_delete (mx);
}

static void
test_mx_line_order		(vbi_sliced *		sliced,
				 unsigned int		n_lines,
				 vbi_bool		exp_success,
				 unsigned int		exp_consumed_lines)
{
	vbi_dvb_mux *mx;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);
	raw = alloc_raw_frame (&good_par_625);

	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    sliced, n_lines,
			    ALL_SERVICES,
			    raw, &good_par_625,
			    /* pts */ 0x1234567,
			    exp_success,
			    VBI_ERR_LINE_ORDER,
			    exp_consumed_lines);

	free (raw);
	free (buffer);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_cor_line_order_checks (void)
{
	vbi_sliced *sliced;
	unsigned int n_lines;

	alloc_init_sliced (&sliced, &n_lines);

	assert (VBI_SLICED_TELETEXT_B_625 == sliced[1].id);
	assert (8 == sliced[1].line);
	assert (VBI_SLICED_TELETEXT_B_625 == sliced[2].id);
	assert (9 == sliced[2].line);
	assert (VBI_SLICED_TELETEXT_B_625 == sliced[3].id);
	assert (10 == sliced[3].line);

	sliced[1].line = 0;
	sliced[2].line = 0;
	test_mx_line_order (sliced, n_lines, EXPECT_SUCCESS, n_lines);

	sliced[1].line = 10;
	sliced[2].line = 0;
	test_mx_line_order (sliced, n_lines, EXPECT_FAILURE, 3);

	sliced[1].line = 8;
	sliced[2].line = 8;
	test_mx_line_order (sliced, n_lines, EXPECT_FAILURE, 2);

	sliced[1].line = 55;
	sliced[2].line = 9;
	test_mx_line_order (sliced, n_lines, EXPECT_FAILURE, 2);

	sliced[1].id = VBI_SLICED_TELETEXT_B_625;
	sliced[1].line = 11;
	sliced[2].id = VBI_SLICED_VBI_625;
	sliced[2].line = 9;
	test_mx_line_order (sliced, n_lines, EXPECT_FAILURE, 2);

	sliced[1].id = VBI_SLICED_VBI_625;
	sliced[1].line = 11;
	sliced[2].id = VBI_SLICED_TELETEXT_B_625;
	sliced[2].line = 9;
	test_mx_line_order (sliced, n_lines, EXPECT_FAILURE, 2);

	sliced[1].id = VBI_SLICED_TELETEXT_B_625;
	sliced[1].line = 8;
	sliced[2].id = VBI_SLICED_TELETEXT_B_625;
	sliced[2].line = 9;

	assert (VBI_SLICED_TELETEXT_B_625 == sliced[17 + 1].id);
	assert (313 + 8 == sliced[17 + 1].line);
	assert (VBI_SLICED_TELETEXT_B_625 == sliced[17 + 2].id);
	assert (313 + 9 == sliced[17 + 2].line);

	sliced[17 + 1].line = 313 + 10;
	test_mx_line_order (sliced, n_lines, EXPECT_FAILURE, 17 + 2);

	free (sliced);
}

static void
test_dvb_mux_cor_packet_overflow_checks (void)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	unsigned int buffer_size;
	unsigned int max_size;
	unsigned int n_lines;
	unsigned int i;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	vbi_dvb_mux_set_pes_packet_size (mx, 0, UINT_MAX);
	max_size = vbi_dvb_mux_get_pes_packet_max_size (mx);

	// Cannot fit because the header takes another 46 bytes.
	n_lines = max_size / 46;
	sliced = alloc_sliced (n_lines);

	for (i = 0; i < n_lines; ++i) {
		sliced[i].id = VBI_SLICED_TELETEXT_B_625;
		sliced[i].line = 0;
	}

	buffer_size = (n_lines + 1) * 46;
	buffer = (uint8_t *) xmalloc (buffer_size);

	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    sliced, n_lines,
			    ALL_SERVICES,
			    /* raw */ NULL,
			    /* sp */ NULL,
			    /* pts */ 0x1234567,
			    EXPECT_FAILURE,
			    VBI_ERR_BUFFER_OVERFLOW,
			    /* exp_consumed_lines */ n_lines - 1);

	free (buffer);
	free (sliced);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_cor_null_sliced_checks (void)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				    /* user_data */ NULL);
	assert (NULL != mx);

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);
	alloc_init_sliced (&sliced, &n_lines);
	raw = alloc_raw_frame (&good_par_625);

	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    /* sliced */ NULL,
			    n_lines,
			    ALL_SERVICES,
			    raw,
			    &good_par_625,
			    /* pts */ 0x1234567,
			    EXPECT_SUCCESS,
			    /* exp_errno */ 0,
			    /* exp_consumed_lines */ 0);

	assert_dvb_mux_cor (mx,
			    buffer, buffer_size,
			    sliced,
			    /* n_lines */ 0,
			    ALL_SERVICES,
			    raw,
			    &good_par_625,
			    /* pts */ 0x1234567,
			    EXPECT_SUCCESS,
			    /* exp_errno */ 0,
			    /* exp_consumed_lines */ 0);

	free (raw);
	free (sliced);
	free (buffer);

	vbi_dvb_mux_delete (mx);

}

static void
test_dvb_mux_cor_unaligned_packet (void)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int i;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	buffer = (uint8_t *) xmalloc (15 + (buffer_size = 4 << 10));
	alloc_init_sliced (&sliced, &n_lines);
	raw = alloc_raw_frame (&good_par_625);

	for (i = 1; i < 16; ++i) {
		assert_dvb_mux_cor (mx,
				    buffer + i, buffer_size,
				    sliced, n_lines,
				    ALL_SERVICES,
				    raw, &good_par_625,
				    /* pts */ 0x1234567,
				    EXPECT_SUCCESS,
				    /* exp_errno */ 0,
				    /* exp_consumed_lines */ n_lines);
	}

	free (raw);
	free (sliced);
	free (buffer);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_cor_null_packet_checks (void)
{
	vbi_dvb_mux *mx;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	assert_dvb_mux_cor (mx,
			    /* buffer */ NULL,
			    /* buffer_size */ 4 << 10,
			    /* sliced */ (vbi_sliced *) -1,
			    /* sliced_lines */ 17,
			    ALL_SERVICES,
			    /* raw */ NULL,
			    /* sp */ NULL,
			    /* pts */ 0x1234567,
			    EXPECT_FAILURE,
			    VBI_ERR_BUFFER_OVERFLOW,
			    /* exp_consumed_lines */ 0);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_feed_no_callback_checks (void)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *raw;
	unsigned int n_lines;
	vbi_bool success;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);

	alloc_init_sliced (&sliced, &n_lines);
	raw = alloc_raw_frame (&good_par_625);

	success = vbi_dvb_mux_feed (mx,
				     sliced, n_lines,
				     ALL_SERVICES,
				     raw, &good_par_625,
				     /* pts */ 0x1234567);
	assert (FALSE == success);
	// XXX Later
	// assert (VBI_ERR_NO_CALLBACK == errno);

	free (raw);
	free (sliced);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_data_identifier_accessors (void)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int buffer_size;
	unsigned int n_lines;
	unsigned int di_tested;
	unsigned int i;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	// Default.
	assert (0x10 == vbi_dvb_mux_get_data_identifier (mx));

	for (i = 0; i < 300; ++i) {
		unsigned int old_di;
		unsigned int new_di;
		vbi_bool success;

		old_di = 0x1F ^ (i & 0xF);
		vbi_dvb_mux_set_data_identifier (mx, old_di); 

		success = vbi_dvb_mux_set_data_identifier (mx, i);

		// EN 300 775 table 2.
		assert (success == ((i >= 0x10 && i <= 0x1F)
				    || (i >= 0x99 && i <= 0x9B)));

		new_di = vbi_dvb_mux_get_data_identifier (mx);

		if (success) {
			assert (i == new_di);
		} else {
			// No change.
			assert (old_di == new_di);
		}
	}

	buffer = (uint8_t *) xmalloc (buffer_size = 4 << 10);
	alloc_init_sliced (&sliced, &n_lines);
	raw = alloc_raw_frame (&good_par_625);

	di_tested = 0;

	for (i = 0; i < N_ELEMENTS (data_identifiers); ++i) {
		unsigned int di;

		di = data_identifiers[i];
		if (!vbi_dvb_mux_set_data_identifier (mx, di))
			continue;

		di_tested |= 1 << (di >= 0x99);

		assert_dvb_mux_cor (mx,
				    buffer, buffer_size,
				    sliced, n_lines,
				    ALL_SERVICES,
				    raw, &good_par_625,
				    /* pts */ 0x1234567,
				    EXPECT_SUCCESS,
				    /* exp_errno */ 0,
				    /* exp_consumed_lines */ n_lines);
	}

	assert (3 == di_tested);

	free (raw);
	free (sliced);
	free (buffer);

	vbi_dvb_mux_delete (mx);
}

static void
test_mx_packet_size		(unsigned int		min_size,
				 unsigned int		max_size)
{
	vbi_dvb_mux *mx;
	vbi_sliced *sliced;
	uint8_t *buffer;
	uint8_t *raw;
	unsigned int n_lines;
	vbi_bool success;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	success = vbi_dvb_mux_set_pes_packet_size (mx, min_size, max_size);
	assert (TRUE == success);

	buffer = (uint8_t *) xmalloc (max_size);
	alloc_init_sliced (&sliced, &n_lines);
	raw = alloc_raw_frame (&good_par_625);

	if (max_size <= 184)
		n_lines = 1;

	assert_dvb_mux_cor (mx,
			    buffer,
			    /* buffer_size */ max_size,
			    sliced, n_lines,
			    ALL_SERVICES,
			    raw, &good_par_625,
			    /* pts */ 0x1234567,
			    EXPECT_SUCCESS,
			    /* exp_errno */ 0,
			    /* exp_consumed_lines */ n_lines);

	free (raw);
	free (sliced);
	free (buffer);

	vbi_dvb_mux_delete (mx);
}

static void
test_dvb_mux_packet_size_accessors (void)
{
	vbi_dvb_mux *mx;
	unsigned int min;
	unsigned int max;
	unsigned int i;

	mx = vbi_dvb_pes_mux_new (/* callback */ NULL,
				   /* user_data */ NULL);
	assert (NULL != mx);

	min = vbi_dvb_mux_get_pes_packet_min_size (mx);
	max = vbi_dvb_mux_get_pes_packet_max_size (mx);

	// Defaults.
	assert (184 == min);
	assert (65504 == max);

	for (i = 0; i < N_ELEMENTS (packet_sizes); ++i) {
		unsigned int j;

		for (j = 0; j < N_ELEMENTS (packet_sizes); ++j) {
			vbi_bool success;

			success = vbi_dvb_mux_set_pes_packet_size
				(mx,
				 packet_sizes[i],
				 packet_sizes[j]);

			assert (TRUE == success);

			min = vbi_dvb_mux_get_pes_packet_min_size (mx);
			max = vbi_dvb_mux_get_pes_packet_max_size (mx);

			assert (0 == min % 184);
			assert (0 == max % 184);
			assert (min >= 184);
			assert (max <= 65504);
			assert (min <= max);

			if (packet_sizes[i] <= 65504)
				assert (min >= packet_sizes[i]);

			if (packet_sizes[j] >= min) // sic
				assert (max <= packet_sizes[j]);
		}
	}

	vbi_dvb_mux_delete (mx);
	mx = (vbi_dvb_mux *) -1;

	test_mx_packet_size (184, 184);
	test_mx_packet_size (184, 65504);
	test_mx_packet_size (65504, 65504);
}

static void
test_dvb_mux_new_pid_checks	(void)
{
	unsigned int i;

	for (i = 0x0000; i <= 0x2000; ++i) {
		vbi_dvb_mux *mx;

		mx = vbi_dvb_ts_mux_new (/* pid */ i,
					  /* callback */ NULL,
					  /* user_data */ NULL);
		assert ((NULL != mx) == (i >= 0x0010 && i <= 0x1FFE));

		vbi_dvb_mux_delete (mx);
		mx = (vbi_dvb_mux *) -1;
	}

	assert (NULL == vbi_dvb_ts_mux_new (0x123456, NULL, NULL)); 
	assert (NULL == vbi_dvb_ts_mux_new (UINT_MAX, NULL, NULL)); 
}

static void
test_dvb_mux			(void)
{
	vbi_dvb_mux_delete (NULL);

	test_dvb_mux_new_pid_checks ();

	test_dvb_mux_packet_size_accessors ();
	test_dvb_mux_data_identifier_accessors ();

	test_dvb_mux_feed_no_callback_checks ();
	test_dvb_mux_cor_null_packet_checks ();
	test_dvb_mux_cor_null_sliced_checks ();
	test_dvb_mux_cor_packet_overflow_checks ();
	test_dvb_mux_cor_line_order_checks ();
	test_dvb_mux_cor_service_checks ();
	test_dvb_mux_cor_line_number_checks ();
	test_dvb_mux_cor_null_raw_or_sp_checks ();
	test_dvb_mux_cor_sampling_parameter_checks ();
	test_dvb_mux_cor_sp_line_number_checks ();

	test_dvb_mux_cor_unaligned_packet ();
	test_dvb_mux_cor_unaligned_raw ();
	test_dvb_mux_cor_service_mask ();
	test_dvb_mux_cor_partial_reads_and_reset (/* pid */ 0);
	test_dvb_mux_cor_partial_reads_and_reset (/* pid */ 0x1234);
	test_dvb_mux_cor_pts ();
}

int
main				(void)
{
	test_multiplex_sliced ();
	test_multiplex_raw ();

	test_dvb_mux ();

	return 0;
}

/*
Local variables:
c-set-style: K&R
c-basic-offset: 8
End:
*/
