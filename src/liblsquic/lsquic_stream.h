/* Copyright (c) 2017 - 2018 LiteSpeed Technologies Inc.  See LICENSE. */
#ifndef LSQUIC_STREAM_H
#define LSQUIC_STREAM_H

#define LSQUIC_GQUIC_STREAM_HANDSHAKE 1
#define LSQUIC_GQUIC_STREAM_HEADERS   3

#define LSQUIC_STREAM_DEFAULT_PRIO 16   /* RFC 7540, Section 5.3.5 */

struct lsquic_stream_if;
struct lsquic_stream_ctx;
struct lsquic_conn_public;
struct stream_frame;
struct uncompressed_headers;
enum enc_level;
enum swtp_status;
struct frame_gen_ctx;
struct data_frame;

TAILQ_HEAD(lsquic_streams_tailq, lsquic_stream);

#ifndef LSQUIC_KEEP_STREAM_HISTORY
#   ifdef NDEBUG
#       define LSQUIC_KEEP_STREAM_HISTORY 0
#   else
#       define LSQUIC_KEEP_STREAM_HISTORY 1
#   endif
#endif

#if LSQUIC_KEEP_STREAM_HISTORY
#define SM_HIST_BITS 6
#define SM_HIST_IDX_MASK ((1 << SM_HIST_BITS) - 1)
typedef unsigned char sm_hist_idx_t;
#endif

/*
 *  +----------+----------------------------------+
 *  | Low Bits | Stream Type                      |
 *  +----------+----------------------------------+
 *  | 0x0      | Client-Initiated, Bidirectional  |
 *  |          |                                  |
 *  | 0x1      | Server-Initiated, Bidirectional  |
 *  |          |                                  |
 *  | 0x2      | Client-Initiated, Unidirectional |
 *  |          |                                  |
 *  | 0x3      | Server-Initiated, Unidirectional |
 *  +----------+----------------------------------+
 */

enum stream_id_type
{
    SIT_BIDI_CLIENT,
    SIT_BIDI_SERVER,
    SIT_UNI_CLIENT,
    SIT_UNI_SERVER,
    N_SITS
};

#define SIT_MASK (N_SITS - 1)

#define SIT_SHIFT 2

enum stream_dir { SD_BIDI, SD_UNI, N_SDS };

struct stream_hq_frame
{
    /* At which point in the stream (sm_payload) to insert the HQ frame. */
    uint64_t            shf_off;
    union {
        /* Points to the frame if SHF_FIXED_SIZE is not set */
        unsigned char  *frame_ptr;
        /* If SHF_FIXED_SIZE is set, the size of the frame to follow.
         * Non-fixed frame size gets calculated using sm_payload when they
         * are closed.
         */
        size_t          frame_size;
    }                   shf_u;
#define shf_frame_ptr shf_u.frame_ptr
#define shf_frame_size shf_u.frame_size
    enum hq_frame_type  shf_frame_type:8;
    enum shf_flags {
        SHF_TWO_BYTES   = 1 << 0,   /* Use two byte to encode frame length */
        SHF_FIXED_SIZE  = 1 << 1,   /* Payload size guaranteed */
        SHF_ACTIVE      = 1 << 2,
        SHF_WRITTEN     = 1 << 3,   /* Framing bytes have been packetized */
        SHF_CC_PAID     = 1 << 4,   /* Paid connection cap */
    }                   shf_flags:8;
};

struct hq_filter
{
    struct varint_read_state    hqfi_vint_state;
    unsigned                    hqfi_seqno;
    /* No need to copy the value: use it directly */
#define hqfi_left hqfi_vint_state.val
    enum hq_frame_type          hqfi_type:8;
    enum {
        HQFI_FLAG_GOT_HEADERS   = 1 << 0,
        HQFI_FLAG_ERROR         = 1 << 1,
        HQFI_FLAG_BEGIN         = 1 << 2,
        HQFI_FLAG_BLOCKED       = 1 << 3,
    }                           hqfi_flags:8;
    enum {
        HQFI_STATE_READING_SIZE_BEGIN,
        HQFI_STATE_READING_SIZE_CONTINUE,
        HQFI_STATE_READING_TYPE,
        HQFI_STATE_READING_PAYLOAD,
    }                           hqfi_state:8;
};

struct stream_filter_if
{
    int         (*sfi_readable)(struct lsquic_stream *);
    size_t      (*sfi_filter_df)(struct lsquic_stream *, struct data_frame *);
    void        (*sfi_decr_left)(struct lsquic_stream *, size_t);
};

enum stream_flags {
    STREAM_WANT_READ    = (1 << 0),
    STREAM_WANT_WRITE   = (1 << 1),
    STREAM_FIN_RECVD    = (1 << 2),     /* Received STREAM frame with FIN bit set */
    STREAM_RST_RECVD    = (1 << 3),     /* Received RST frame */
    STREAM_SEND_WUF     = (1 << 4),     /* WUF: Window Update Frame */
    STREAM_LAST_WRITE_OK= (1 << 5),     /* Used to break out of write event dispatch loop */
    STREAM_SEND_BLOCKED = (1 << 6),
    STREAM_SEND_RST     = (1 << 7),     /* Error: want to send RST_STREAM */
    STREAM_U_READ_DONE  = (1 << 8),     /* User is done reading (shutdown was called) */
    STREAM_U_WRITE_DONE = (1 << 9),     /* User is done writing (shutdown was called) */
    STREAM_FIN_SENT     = (1 <<10),     /* FIN was written to network */
    STREAM_RST_SENT     = (1 <<11),     /* RST_STREAM was written to network */
    STREAM_WANT_FLUSH   = (1 <<12),     /* Flush until sm_flush_to is hit */
    STREAM_FIN_REACHED  = (1 <<13),     /* User read data up to FIN */
    STREAM_FINISHED     = (1 <<14),     /* Stream is finished */
    STREAM_ONCLOSE_DONE = (1 <<15),     /* on_close has been called */
    STREAM_CALL_ONCLOSE = (1 <<16),
    STREAM_FREE_STREAM  = (1 <<17),
    STREAM_USE_HEADERS  = (1 <<18),
    STREAM_HEADERS_SENT = (1 <<19),
    STREAM_HAVE_UH      = (1 <<20),     /* Have uncompressed headers */
    STREAM_CONN_LIMITED = (1 <<21),
    STREAM_HEAD_IN_FIN  = (1 <<22),     /* Incoming headers has FIN bit set */
    STREAM_ABORT_CONN   = (1 <<23),     /* Unrecoverable error occurred */
    STREAM_FRAMES_ELIDED= (1 <<24),
    STREAM_FORCE_FINISH = (1 <<25),     /* Replaces FIN sent and received */
    STREAM_ONNEW_DONE   = (1 <<26),     /* on_new_stream has been called */
    STREAM_AUTOSWITCH   = (1 <<27),
    STREAM_RW_ONCE      = (1 <<28),     /* When set, read/write events are dispatched once per call */
    STREAM_IETF         = (1 <<29),
    STREAM_CRYPTO       = (1 <<30),
    STREAM_QPACK_DEC    = (1 <<31),     /* QPACK decoder is holding a reference to this stream */
};

struct lsquic_stream
{
    lsquic_stream_id_t              id;
    struct lsquic_hash_elem         sm_hash_el;
    enum stream_flags               stream_flags;
    unsigned                        n_unacked;

    /* There are more than one reason that a stream may be put onto
     * connections's sending_streams queue.  Note that writing STREAM
     * frames is done separately.
     */
    #define STREAM_SENDING_FLAGS (STREAM_SEND_WUF| \
                                          STREAM_SEND_RST|STREAM_SEND_BLOCKED)

    #define STREAM_WRITE_Q_FLAGS (STREAM_WANT_FLUSH|STREAM_WANT_WRITE)

    /* Any of these flags will cause user-facing read and write and
     * shutdown calls to return an error.  They also make the stream
     * both readable and writeable, as we want the user to collect
     * the error.
     */
    #define STREAM_RST_FLAGS (STREAM_RST_RECVD|STREAM_RST_SENT|\
                                                        STREAM_SEND_RST)

    #define STREAM_SERVICE_FLAGS (STREAM_CALL_ONCLOSE|STREAM_FREE_STREAM|\
                                                            STREAM_ABORT_CONN)

    const struct lsquic_stream_if  *stream_if;
    struct lsquic_stream_ctx       *st_ctx;
    struct lsquic_conn_public      *conn_pub;
    TAILQ_ENTRY(lsquic_stream)      next_send_stream, next_read_stream,
                                        next_write_stream, next_service_stream,
                                        next_prio_stream;

    uint32_t                        error_code;
    uint64_t                        tosend_off;
    uint64_t                        sm_payload;     /* Not counting HQ frames */
    uint64_t                        max_send_off;

    /* From the network, we get frames, which we keep on a list ordered
     * by offset.
     */
    struct data_in                 *data_in;
    uint64_t                        read_offset;
    lsquic_sfcw_t                   fc;

    /* Ring buffer.  Next entry indexed by sm_next_hq_frame. */
    struct stream_hq_frame          sm_hq_frames[2];

    struct hq_filter                sm_hq_filter;

    /** If @ref STREAM_WANT_FLUSH is set, flush until this offset. */
    uint64_t                        sm_flush_to;

    /* Last offset sent in BLOCKED frame */
    uint64_t                        blocked_off;

    struct uncompressed_headers    *uh,
                                   *push_req;

    unsigned char                  *sm_buf;
    void                           *sm_onnew_arg;

    unsigned char                  *sm_header_block;
    uint64_t                        sm_hb_compl;

    /* A stream may be generating STREAM or CRYPTO frames */
    size_t                        (*sm_frame_header_sz)(
                                        const struct lsquic_stream *, unsigned);
    enum swtp_status              (*sm_write_to_packet)(struct frame_gen_ctx *,
                                                const size_t);
    size_t                        (*sm_write_avail)(struct lsquic_stream *);
    int                           (*sm_readable)(struct lsquic_stream *);

    /* This element is optional */
    const struct stream_filter_if  *sm_sfi;

    /* How much data there is in sm_header_block and how much of it has been
     * sent:
     */
    unsigned                        sm_hblock_sz,
                                    sm_hblock_off;

    unsigned short                  sm_n_buffered;  /* Amount of data in sm_buf */

    unsigned char                   sm_priority;  /* 0: high; 255: low */
    unsigned char                   sm_enc_level;
    unsigned char                   sm_next_hq_frame;   /* Valid values 0 and 1 */
    enum {
        SSHS_BEGIN,         /* Nothing has happened yet */
        SSHS_ENC_SENDING,   /* Sending encoder stream data */
        SSHS_HBLOCK_SENDING,/* Sending header block data */
    }                               sm_send_headers_state:8;

#if LSQUIC_KEEP_STREAM_HISTORY
    sm_hist_idx_t                   sm_hist_idx;
#endif

#if LSQUIC_KEEP_STREAM_HISTORY
    /* Stream history: see enum stream_history_event */
    unsigned char                   sm_hist_buf[ 1 << SM_HIST_BITS ];
#endif
};

enum stream_ctor_flags
{
    SCF_CALL_ON_NEW   = (1 << 0), /* Call on_new_stream() immediately */
    SCF_USE_DI_HASH   = (1 << 1), /* Use hash-based data input.  If not set,
                                   * the nocopy data input is used.
                                   */
    SCF_DI_AUTOSWITCH = (1 << 2), /* Automatically switch between nocopy
                                   * and hash-based to data input for optimal
                                   * performance.
                                   */
    SCF_DISP_RW_ONCE  = (1 << 3),
    SCF_IETF          = (1 << 4),
    SCF_HTTP          = (1 << 5),
};

lsquic_stream_t *
lsquic_stream_new (lsquic_stream_id_t id, struct lsquic_conn_public *,
                   const struct lsquic_stream_if *, void *stream_if_ctx,
                   unsigned initial_sfrw, unsigned initial_send_off,
                   enum stream_ctor_flags);

struct lsquic_stream *
lsquic_stream_new_crypto (enum enc_level,
        struct lsquic_conn_public *conn_pub,
        const struct lsquic_stream_if *stream_if, void *stream_if_ctx,
        enum stream_ctor_flags ctor_flags);

void
lsquic_stream_call_on_new (lsquic_stream_t *);

void
lsquic_stream_destroy (lsquic_stream_t *);

#define lsquic_stream_is_reset(stream) \
    (!!((stream)->stream_flags & STREAM_RST_FLAGS))

/* Data that from the network gets inserted into the stream using
 * lsquic_stream_frame_in() function.  Returns 0 on success, -1 on
 * failure.  The latter may be caused by flow control violation or
 * invalid stream frame data, e.g. overlapping segments.
 *
 * Note that the caller does gives up control of `frame' no matter
 * what this function returns.
 *
 * This data is read by the user using lsquic_stream_read() function.
 */
int
lsquic_stream_frame_in (lsquic_stream_t *, struct stream_frame *frame);

/* Only one (at least for now) uncompressed header structure is allowed to be
 * passed in, and only in HTTP mode.
 */
int
lsquic_stream_uh_in (lsquic_stream_t *, struct uncompressed_headers *);

void
lsquic_stream_push_req (lsquic_stream_t *,
                        struct uncompressed_headers *push_req);

int
lsquic_stream_rst_in (lsquic_stream_t *, uint64_t offset, uint32_t error_code);

ssize_t
lsquic_stream_read (lsquic_stream_t *stream, void *buf, size_t len);

uint64_t
lsquic_stream_read_offset (const lsquic_stream_t *stream);

/* Return true if we sent all available data to the network and write
 * end of the stream was closed.
 */
int
lsquic_stream_tosend_fin (const lsquic_stream_t *stream);

/* Data to be sent out to the network is written using lsquic_stream_write().
 */
ssize_t
lsquic_stream_write (lsquic_stream_t *stream, const void *buf, size_t len);

void
lsquic_stream_window_update (lsquic_stream_t *stream, uint64_t offset);

int
lsquic_stream_set_max_send_off (lsquic_stream_t *stream, unsigned offset);

/* The caller should only call this function if STREAM_SEND_WUF is set and
 * it must generate a window update frame using this value.
 */
uint64_t
lsquic_stream_fc_recv_off (lsquic_stream_t *stream);

void
lsquic_stream_dispatch_read_events (lsquic_stream_t *);

void
lsquic_stream_dispatch_write_events (lsquic_stream_t *);

void
lsquic_stream_blocked_frame_sent (lsquic_stream_t *);

void
lsquic_stream_rst_frame_sent (lsquic_stream_t *);

void
lsquic_stream_stream_frame_sent (lsquic_stream_t *);

void
lsquic_stream_reset (lsquic_stream_t *, uint32_t error_code);

void
lsquic_stream_reset_ext (lsquic_stream_t *, uint32_t error_code, int close);

void
lsquic_stream_call_on_close (lsquic_stream_t *);

void
lsquic_stream_shutdown_internal (lsquic_stream_t *);

void
lsquic_stream_received_goaway (lsquic_stream_t *);

void
lsquic_stream_acked (lsquic_stream_t *);

#define lsquic_stream_is_closed(s)                                          \
    (((s)->stream_flags & (STREAM_U_READ_DONE|STREAM_U_WRITE_DONE))         \
                            == (STREAM_U_READ_DONE|STREAM_U_WRITE_DONE))
int
lsquic_stream_update_sfcw (lsquic_stream_t *, uint64_t max_off);

int
lsquic_stream_set_priority_internal (lsquic_stream_t *, unsigned priority);

int
lsquic_stream_id_is_critical (int use_http, lsquic_stream_id_t);

int
lsquic_stream_is_critical (const struct lsquic_stream *);

size_t
lsquic_stream_mem_used (const struct lsquic_stream *);

const lsquic_cid_t *
lsquic_stream_cid (const struct lsquic_stream *);

#define lsquic_stream_has_data_to_flush(stream) ((stream)->sm_n_buffered > 0)

int
lsquic_stream_readable (struct lsquic_stream *);

size_t
lsquic_stream_write_avail (struct lsquic_stream *);

#ifndef NDEBUG
size_t
lsquic_stream_flush_threshold (const struct lsquic_stream *, unsigned);
#endif

#define crypto_level(stream) (~0ULL - (stream)->id)

void
lsquic_stream_set_stream_if (struct lsquic_stream *,
                   const struct lsquic_stream_if *, void *stream_if_ctx);

struct qpack_dec_hdl *
lsquic_stream_get_qdh (const struct lsquic_stream *);

#endif
