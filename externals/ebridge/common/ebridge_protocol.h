/*
 * Eglantine Bridge - wire protocol (version 1)
 *
 * Shared by the Switch client (C, libnx) and the PC host (C++, inside Eglantine).
 * All multi-byte fields are little-endian (both ARM64 Switch and x86-64 PC are LE).
 * Every struct is packed; sizes are checked at compile time at the bottom.
 *
 * Full description: docs/PROTOCOL.md
 */
#ifndef EBRIDGE_PROTOCOL_H
#define EBRIDGE_PROTOCOL_H

#include <stdint.h>

#define EB_PROTO_VERSION   1

/* One port number for everything after discovery:
 * TCP <port> = control channel, UDP <port> = input / video / audio / rumble. */
#define EB_DEFAULT_PORT    52141
#define EB_DISCOVERY_PORT  52140   /* UDP, broadcast on the LAN */

#define EB_MAGIC           0x31474245u  /* "EBG1" in memory */
#define EB_UDP_MAGIC       0x4245u      /* "EB" in memory */

#define EB_NAME_LEN        32
#define EB_ID_LEN          16
#define EB_MAX_TOUCH       4
#define EB_MAX_UDP_PACKET  1400         /* keep below typical MTU */

#pragma pack(push, 1)

/* ------------------------------------------------------------------ */
/* Discovery (UDP broadcast to EB_DISCOVERY_PORT)                      */
/* ------------------------------------------------------------------ */

enum {
    EB_DISC_QUERY    = 1,  /* client -> broadcast */
    EB_DISC_RESPONSE = 2,  /* host -> client (unicast reply) */
};

enum {
    EB_HOST_FLAG_GAME_RUNNING = 1u << 0,
    EB_HOST_FLAG_BUSY         = 1u << 1,  /* another client is connected */
};

typedef struct {
    uint32_t magic;     /* EB_MAGIC */
    uint8_t  type;      /* EB_DISC_QUERY */
    uint8_t  version;   /* EB_PROTO_VERSION */
    uint16_t reserved;
} eb_disc_query_t;

typedef struct {
    uint32_t magic;
    uint8_t  type;      /* EB_DISC_RESPONSE */
    uint8_t  version;
    uint16_t port;      /* TCP+UDP port to connect to */
    uint8_t  host_id[EB_ID_LEN];
    char     name[EB_NAME_LEN];   /* NUL-terminated if shorter */
    uint32_t flags;     /* EB_HOST_FLAG_* */
} eb_disc_response_t;

/* ------------------------------------------------------------------ */
/* Control channel (TCP). Each message = eb_ctl_header_t + payload.    */
/* ------------------------------------------------------------------ */

enum {
    EB_CTL_HELLO         = 1,   /* C->H  eb_ctl_hello_t */
    EB_CTL_PAIR_REQUIRED = 2,   /* H->C  (no payload) host shows a PIN */
    EB_CTL_PAIR_PIN      = 3,   /* C->H  eb_ctl_pair_pin_t */
    EB_CTL_PAIR_OK       = 4,   /* H->C  eb_ctl_pair_ok_t */
    EB_CTL_WELCOME       = 5,   /* H->C  eb_ctl_welcome_t, session starts */
    EB_CTL_REJECT        = 6,   /* H->C  eb_ctl_reject_t, then close */
    EB_CTL_PING          = 7,   /* C->H  eb_ctl_ping_t */
    EB_CTL_PONG          = 8,   /* H->C  eb_ctl_ping_t (echoed) */
    EB_CTL_BYE           = 9,   /* either way, no payload */
    EB_CTL_REQUEST_IDR   = 10,  /* C->H  ask for a keyframe (video loss) */
    EB_CTL_GAMES_REQUEST = 11,  /* C->H  eb_ctl_games_request_t (or no payload): list the games */
    EB_CTL_GAME_ENTRY    = 12,  /* H->C  eb_ctl_game_t, one per game */
    EB_CTL_GAMES_END     = 13,  /* H->C  eb_ctl_games_end_t, after the last entry */
    EB_CTL_LAUNCH        = 14,  /* C->H  eb_ctl_launch_t: start that game */
    EB_CTL_GAME_ICON     = 15,  /* H->C  eb_ctl_game_icon_t + up to EB_ICON_CHUNK bytes */
    EB_CTL_STATUS        = 16,  /* H->C  eb_ctl_status_t: what the emulator is doing */
};
/* A host that doesn't know GAMES_REQUEST just ignores it: the console then
 * waits ~1 s for GAMES_END and goes straight to the stream. */

enum {
    EB_REJECT_BAD_VERSION = 1,
    EB_REJECT_BAD_PIN     = 2,
    EB_REJECT_BUSY        = 3,
    EB_REJECT_PROTOCOL    = 4,
};

enum {
    EB_CODEC_H264 = 1u << 0,
    EB_CODEC_HEVC = 1u << 1,
};

typedef struct {
    uint16_t type;
    uint16_t length;    /* payload bytes after this header */
} eb_ctl_header_t;

typedef struct {
    uint32_t magic;                 /* EB_MAGIC */
    uint16_t version;               /* EB_PROTO_VERSION */
    uint16_t reserved;
    uint8_t  client_id[EB_ID_LEN];  /* random, generated once per console */
    uint8_t  pair_secret[EB_ID_LEN];/* all zero if not paired with this host */
    char     name[EB_NAME_LEN];
    uint16_t max_width;             /* decoder limits, e.g. 1920x1080 */
    uint16_t max_height;
    uint8_t  codecs;                /* EB_CODEC_* bitmask */
    uint8_t  max_fps;
    uint16_t reserved2;
} eb_ctl_hello_t;

typedef struct {
    uint32_t pin;                   /* 4 digits shown by the host */
} eb_ctl_pair_pin_t;

typedef struct {
    uint8_t pair_secret[EB_ID_LEN]; /* client stores this for next time */
    uint8_t host_id[EB_ID_LEN];
} eb_ctl_pair_ok_t;

typedef struct {
    uint32_t session_token;         /* must be in every UDP packet */
    uint16_t udp_port;
    uint16_t width;                 /* video stream size */
    uint16_t height;
    uint8_t  fps;
    uint8_t  codec;                 /* one EB_CODEC_* value */
    uint32_t bitrate_kbps;
} eb_ctl_welcome_t;

/* Game list. Names are UTF-8, NUL-terminated (cut to fit). */
#define EB_GAME_NAME_LEN 96
enum { EB_GAME_RUNNING = 1u << 0 };   /* eb_ctl_game_t.flags */

/* The first 112 bytes are the original entry: a console reading only those still works. */
typedef struct {
    uint16_t index;                 /* 0..count-1, used by EB_CTL_LAUNCH */
    uint16_t flags;                 /* EB_GAME_* */
    uint32_t reserved;
    uint64_t title_id;              /* 0 if unknown */
    char     name[EB_GAME_NAME_LEN];
    char     file_type[16];         /* the emulator's columns, already formatted: "NSP", */
    char     size[16];              /* "3.2 GiB", "02:48:50" */
    char     play_time[16];
} eb_ctl_game_t;

/* Icons: only sent if the request asks for them (old consoles would choke on
 * the large messages). RGB888, EB_ICON_SIZE x EB_ICON_SIZE, in chunks. */
enum { EB_GAMES_WANT_ICONS = 1u << 0 };
#define EB_ICON_SIZE  64
#define EB_ICON_BYTES (EB_ICON_SIZE * EB_ICON_SIZE * 3)
#define EB_ICON_CHUNK 1024

typedef struct {
    uint32_t flags;                 /* EB_GAMES_* */
} eb_ctl_games_request_t;

/* Sent on connect and whenever it changes; the console shows a loading screen
 * until EB_STATUS_RUNNING. Older consoles ignore it. */
enum {
    EB_STATUS_IDLE    = 0,   /* no game running */
    EB_STATUS_BOOTING = 1,   /* starting a game */
    EB_STATUS_LOADING = 2,   /* e.g. loading shaders: value / total */
    EB_STATUS_RUNNING = 3,   /* the game is on screen */
    EB_STATUS_ERROR   = 4,   /* the game could not start */
};

typedef struct {
    uint8_t  state;                 /* EB_STATUS_* */
    uint8_t  reserved[3];
    uint32_t value;                 /* progress, if total > 0 */
    uint32_t total;
    char     text[96];              /* UTF-8, ready to show: "Loading shaders" */
    char     game[64];              /* game name, may be empty */
} eb_ctl_status_t;

typedef struct {
    uint16_t index;                 /* game this icon belongs to */
    uint16_t size;                  /* EB_ICON_SIZE */
    uint32_t offset;                /* of this chunk inside the EB_ICON_BYTES image */
    uint32_t total;                 /* EB_ICON_BYTES */
    uint32_t reserved;
} eb_ctl_game_icon_t;

typedef struct {
    uint16_t count;                 /* entries sent */
    uint16_t reserved;
} eb_ctl_games_end_t;

typedef struct {
    uint16_t index;                 /* from the latest list */
    uint16_t reserved;
} eb_ctl_launch_t;

typedef struct {
    uint16_t reason;                /* EB_REJECT_* */
    uint16_t reserved;
} eb_ctl_reject_t;

typedef struct {
    uint64_t client_time_us;
} eb_ctl_ping_t;

/* ------------------------------------------------------------------ */
/* Stream channel (UDP). Each datagram = eb_udp_header_t + payload.    */
/* ------------------------------------------------------------------ */

enum {
    EB_UDP_HELLO     = 1,   /* C->H  no payload: tells the host our UDP address */
    EB_UDP_INPUT     = 2,   /* C->H  eb_input_t */
    EB_UDP_VIDEO     = 3,   /* H->C  eb_video_frag_t + data   (milestone 2) */
    EB_UDP_AUDIO     = 4,   /* H->C  eb_audio_t + one Opus packet */
    EB_UDP_RUMBLE    = 5,   /* H->C  eb_rumble_t */
    EB_UDP_HEARTBEAT = 6,   /* either way, no payload */
};

typedef struct {
    uint16_t magic;          /* EB_UDP_MAGIC */
    uint8_t  type;           /* EB_UDP_* */
    uint8_t  flags;
    uint32_t session_token;
    uint32_t seq;            /* per-sender, per-type counter */
    uint32_t timestamp_us;   /* sender clock, wraps */
} eb_udp_header_t;

/* Button bits. Order matches InputCommon::VirtualGamepad::VirtualButton
 * in Eglantine, so bit N == VirtualButton N. */
enum {
    EB_BTN_A       = 1u << 0,
    EB_BTN_B       = 1u << 1,
    EB_BTN_X       = 1u << 2,
    EB_BTN_Y       = 1u << 3,
    EB_BTN_LSTICK  = 1u << 4,
    EB_BTN_RSTICK  = 1u << 5,
    EB_BTN_L       = 1u << 6,
    EB_BTN_R       = 1u << 7,
    EB_BTN_ZL      = 1u << 8,
    EB_BTN_ZR      = 1u << 9,
    EB_BTN_PLUS    = 1u << 10,
    EB_BTN_MINUS   = 1u << 11,
    EB_BTN_LEFT    = 1u << 12,
    EB_BTN_UP      = 1u << 13,
    EB_BTN_RIGHT   = 1u << 14,
    EB_BTN_DOWN    = 1u << 15,
    EB_BTN_SL      = 1u << 16,
    EB_BTN_SR      = 1u << 17,
    EB_BTN_HOME    = 1u << 18,
    EB_BTN_CAPTURE = 1u << 19,
};
#define EB_BTN_COUNT 20

enum {
    EB_CTRL_HANDHELD = 0,
    EB_CTRL_PRO      = 1,
    EB_CTRL_DUAL_JOYCON = 2,
    EB_CTRL_LEFT_JOYCON = 3,
    EB_CTRL_RIGHT_JOYCON = 4,
};

typedef struct {
    uint16_t x;          /* 0..1279 (Switch 1 screen) */
    uint16_t y;          /* 0..719 */
    uint8_t  id;         /* finger id */
    uint8_t  reserved[3];
} eb_touch_t;

typedef struct {
    uint8_t  player;          /* 0 = player 1 */
    uint8_t  controller_type; /* EB_CTRL_* */
    uint8_t  touch_count;     /* 0..EB_MAX_TOUCH */
    uint8_t  reserved;
    uint32_t buttons;         /* EB_BTN_* */
    int16_t  lx, ly, rx, ry;  /* -32767..32767, +y = up */
    float    gyro[3];         /* revolutions per second (libnx units) */
    float    accel[3];        /* G */
    eb_touch_t touch[EB_MAX_TOUCH];
} eb_input_t;

/* Video fragment header. One encoded frame (Annex-B H.264) is split into
 * frag_count datagrams; frame_id increases by one per frame. Every fragment
 * except the last carries exactly EB_VIDEO_FRAG_DATA bytes, so fragment i
 * starts at offset i * EB_VIDEO_FRAG_DATA. */
enum { EB_FRAME_KEY = 1u << 0 };
#define EB_VIDEO_FRAG_DATA (EB_MAX_UDP_PACKET - 16 - 16)   /* 1368 */
#define EB_VIDEO_MAX_FRAGS 1024                             /* ~1.4 MB per frame */

typedef struct {
    uint32_t frame_id;
    uint16_t frag_index;
    uint16_t frag_count;
    uint8_t  frame_flags;     /* EB_FRAME_* */
    uint8_t  codec;           /* EB_CODEC_* */
    uint16_t reserved;
    uint32_t frame_size;      /* total bytes of the encoded frame */
} eb_video_frag_t;

/* Audio: Opus, 48 kHz stereo, one 10 ms frame (480 samples per channel) per
 * datagram. seq increases by one per frame, so the console can conceal a lost
 * frame (Opus packet loss concealment) instead of skipping it. */
#define EB_AUDIO_RATE     48000
#define EB_AUDIO_CHANNELS 2
#define EB_AUDIO_FRAME    480
enum { EB_AUDIO_OPUS = 1 };

typedef struct {
    uint32_t seq;
    uint16_t samples;         /* per channel, EB_AUDIO_FRAME */
    uint8_t  channels;
    uint8_t  codec;           /* EB_AUDIO_* */
} eb_audio_t;

/* Same four values as libnx HidVibrationValue. */
typedef struct {
    uint8_t player;
    uint8_t reserved[3];
    float   amp_low;
    float   freq_low;
    float   amp_high;
    float   freq_high;
} eb_rumble_t;

#pragma pack(pop)

/* Compile-time size checks (also mirrored in tools/ebridge_sim.py). */
#ifdef __cplusplus
#define EB_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define EB_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
EB_STATIC_ASSERT(sizeof(eb_disc_query_t) == 8, "eb_disc_query_t");
EB_STATIC_ASSERT(sizeof(eb_disc_response_t) == 60, "eb_disc_response_t");
EB_STATIC_ASSERT(sizeof(eb_ctl_header_t) == 4, "eb_ctl_header_t");
EB_STATIC_ASSERT(sizeof(eb_ctl_hello_t) == 80, "eb_ctl_hello_t");
EB_STATIC_ASSERT(sizeof(eb_ctl_pair_ok_t) == 32, "eb_ctl_pair_ok_t");
EB_STATIC_ASSERT(sizeof(eb_ctl_welcome_t) == 16, "eb_ctl_welcome_t");
EB_STATIC_ASSERT(sizeof(eb_ctl_game_t) == 160, "eb_ctl_game_t");
EB_STATIC_ASSERT(sizeof(eb_ctl_game_icon_t) == 16, "eb_ctl_game_icon_t");
EB_STATIC_ASSERT(sizeof(eb_ctl_status_t) == 172, "eb_ctl_status_t");
EB_STATIC_ASSERT(sizeof(eb_ctl_games_end_t) == 4, "eb_ctl_games_end_t");
EB_STATIC_ASSERT(sizeof(eb_ctl_launch_t) == 4, "eb_ctl_launch_t");
EB_STATIC_ASSERT(sizeof(eb_udp_header_t) == 16, "eb_udp_header_t");
EB_STATIC_ASSERT(sizeof(eb_input_t) == 72, "eb_input_t");
EB_STATIC_ASSERT(sizeof(eb_video_frag_t) == 16, "eb_video_frag_t");
EB_STATIC_ASSERT(sizeof(eb_rumble_t) == 20, "eb_rumble_t");
EB_STATIC_ASSERT(sizeof(eb_audio_t) == 8, "eb_audio_t");

#endif /* EBRIDGE_PROTOCOL_H */
