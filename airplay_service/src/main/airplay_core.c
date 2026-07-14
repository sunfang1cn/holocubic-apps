/* Author: sunfang1cn@gmail.com */

#include <stddef.h>
#include <stdint.h>

#include "airplay_crypto.h"
#include "decoder/impl/esp_alac_dec.h"
#include "esp_chip_info.h"
#include "module_abi.h"

#define AIRPLAY_EXPORT __attribute__((visibility("default"), used))
#define AIRPLAY_CORE_VERSION "0.3.15"
#define AIRPLAY_PACKET_SLOTS 512u
#define AIRPLAY_PACKET_MAX 2048u
#define AIRPLAY_PREBUFFER_PACKETS 160u
#define AIRPLAY_MISSING_TICKS 192u
#define AIRPLAY_RESEND_TAIL_GUARD 4u
#define AIRPLAY_RESEND_MAX_COUNT 32u
#define AIRPLAY_RESEND_MIN_INTERVAL_US 60000u
#define AIRPLAY_RESEND_PRIORITY_EARLY 1u
#define AIRPLAY_RESEND_PRIORITY_CRITICAL 2u
#define AIRPLAY_RESYNC_HIGH_WATER 192u
#define AIRPLAY_PCM_PACKET_MAX 2048u
#define AIRPLAY_PCM_MAX AIRPLAY_PCM_PACKET_MAX
#define AIRPLAY_PCM_SLOTS 256u
#define AIRPLAY_PCM_CHUNKS 4u
#define AIRPLAY_PCM_CHUNK_SLOTS (AIRPLAY_PCM_SLOTS / AIRPLAY_PCM_CHUNKS)
#define AIRPLAY_PCM_START_PACKETS 16u
#define AIRPLAY_PCM_RETRANSMIT_FLOOR 24u
#define AIRPLAY_TASK_STACK 8192u
#define AIRPLAY_TASK_PRIORITY 10u
#define AIRPLAY_TASK_STOP_WAIT_MS 1000u
#define AIRPLAY_INTERNAL_HEAP_RESERVE (48u * 1024u)

enum {
    AIRPLAY_CODEC_NONE = 0,
    AIRPLAY_CODEC_L16 = 1,
    AIRPLAY_CODEC_ALAC = 2,
};

typedef struct airplay_packet_t {
    volatile uint8_t valid;
    uint8_t reserved;
    uint16_t sequence;
    uint16_t len;
    uint16_t payload_type;
    uint32_t timestamp;
    uint8_t data[AIRPLAY_PACKET_MAX];
} airplay_packet_t;

typedef struct airplay_pcm_packet_t {
    volatile uint8_t valid;
    uint8_t reserved;
    uint16_t len;
    uint32_t generation;
    uint8_t data[AIRPLAY_PCM_PACKET_MAX];
} airplay_pcm_packet_t;

typedef struct airplay_instance_t {
    module_host_api_v1 host;
    void *decoder;
    void *i2s_stream;
    void *play_task;
    void *output_task;
    airplay_packet_t *slots;
    airplay_pcm_packet_t *pcm_slots;
    airplay_pcm_packet_t *pcm_chunks[AIRPLAY_PCM_CHUNKS];
    uint8_t *encoded_buf;
    uint8_t *decrypt_buf;
    uint8_t *pcm_buf;
    uint8_t magic_cookie[24];
    uint8_t aes_key[16];
    uint8_t aes_iv[16];
    airplay_aes128_context_t aes_ctx;
    volatile uint8_t configured;
    volatile uint8_t encrypted;
    volatile uint8_t task_running;
    volatile uint8_t output_task_running;
    volatile uint8_t task_stop;
    volatile uint8_t playing;
    volatile uint8_t buffering;
    volatile uint8_t first_valid;
    volatile uint8_t expected_valid;
    volatile uint8_t newest_valid;
    volatile uint16_t first_sequence;
    volatile uint16_t expected_sequence;
    volatile uint16_t newest_sequence;
    volatile uint16_t packet_count;
    volatile uint16_t left_peak;
    volatile uint16_t right_peak;
    volatile uint16_t resend_sequence;
    volatile uint16_t resend_count;
    volatile uint16_t resend_batch_max;
    volatile uint32_t resend_serial;
    volatile uint32_t resend_packets_requested;
    volatile uint32_t resend_requests_published;
    volatile uint32_t early_resend_requests;
    volatile uint32_t early_resend_packets;
    volatile uint8_t resend_pending_valid;
    volatile uint8_t resend_priority;
    volatile uint8_t missing_burst_active;
    volatile uint32_t flush_serial;
    volatile uint32_t packets_received;
    volatile uint32_t packets_ingested;
    volatile uint32_t bytes_ingested;
    volatile uint32_t packets_dropped;
    volatile uint32_t packets_invalid;
    volatile uint32_t packets_payload_dropped;
    volatile uint32_t packets_slot_collision;
    volatile uint32_t packets_duplicate;
    volatile uint32_t packets_late;
    volatile uint32_t packets_lost;
    volatile uint32_t loss_bursts;
    volatile uint32_t burst_loss_packets;
    volatile uint32_t packets_decoded;
    volatile uint32_t bytes_written;
    volatile uint32_t decode_errors;
    volatile uint32_t buffer_resyncs;
    volatile uint32_t buffer_resync_discarded;
    volatile uint32_t pcm_write_serial;
    volatile uint32_t pcm_read_serial;
    volatile uint32_t pcm_underruns;
    volatile uint16_t packet_buffer_high_water;
    volatile uint16_t packet_span_high_water;
    volatile uint16_t pcm_buffer_high_water;
    uint64_t aes_us_total;
    uint64_t decode_us_total;
    uint64_t packet_us_total;
    uint64_t i2s_write_us_total;
    uint64_t last_rtp_us;
    uint64_t last_producer_us;
    uint64_t last_output_us;
    uint64_t last_resend_publish_us;
    volatile uint32_t aes_calls;
    volatile uint32_t i2s_write_calls;
    volatile uint32_t aes_us_max;
    volatile uint32_t decode_us_max;
    volatile uint32_t packet_us_max;
    volatile uint32_t i2s_write_us_max;
    volatile uint32_t rtp_gap_us_max;
    volatile uint32_t rtp_gap_over_20ms;
    volatile uint32_t rtp_gap_over_100ms;
    volatile uint32_t rtp_gap_over_500ms;
    volatile uint32_t producer_gap_us_max;
    volatile uint32_t output_gap_us_max;
    uint32_t sample_rate;
    uint32_t frame_length;
    uint16_t channels;
    uint16_t bits;
    uint16_t payload_type;
    uint16_t prebuffer_packets;
    uint16_t gain_q15;
    uint8_t codec;
    uint8_t missing_ticks;
    uint8_t missing_wait_ticks;
    uint8_t workspace_internal;
    uint8_t pcm_queue_internal;
    uint8_t pcm_queue_chunked;
    const char *last_error;
} airplay_instance_t;

static const module_host_api_v1 *s_host = NULL;

static const module_manifest_t s_manifest = {
    MODULE_MANIFEST_MAGIC,
    MODULE_SDK_VERSION,
    sizeof(module_manifest_t),
    "airplay_core",
    AIRPLAY_CORE_VERSION,
    "AirPlay 1 RSA/AES/ALAC realtime core",
    0,
    MODULE_BOOTSTRAP_ABI_VERSION,
};

static void copy_bytes(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void zero_bytes(void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

static void memory_barrier(void)
{
    __asm__ __volatile__("memw" ::: "memory");
}

static int string_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static airplay_instance_t *instance_from_lua(lua_State *L)
{
    if (!s_host || !s_host->lua.touserdata || !s_host->lua.upvalue_index) return NULL;
    return (airplay_instance_t *)s_host->lua.touserdata(L, s_host->lua.upvalue_index(1));
}

static void set_function_field(lua_State *L,
                               const module_host_api_v1 *host,
                               const char *key,
                               module_lua_cfunction_t fn,
                               airplay_instance_t *inst)
{
    host->lua.pushlightuserdata(L, inst);
    host->lua.pushcclosure(L, fn, 1);
    host->lua.setfield(L, -2, key);
}

static int push_error(lua_State *L, const module_host_api_v1 *host, const char *message)
{
    if (!host) return 0;
    host->lua.pushnil(L);
    host->lua.pushstring(L, message ? message : "airplay core error");
    return 2;
}

static void set_error(airplay_instance_t *inst, const char *message)
{
    if (inst) inst->last_error = message;
}

static int base64_value(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static int base64_decode(const char *text, size_t text_len,
                         uint8_t *output, size_t capacity, size_t *output_len)
{
    uint32_t accumulator = 0;
    unsigned bits = 0;
    size_t produced = 0;
    size_t i;
    if (!text || !output || !output_len) return 0;
    for (i = 0; i < text_len; ++i) {
        int value;
        unsigned char ch = (unsigned char)text[i];
        if (ch == '=') break;
        value = base64_value(ch);
        if (value < 0) {
            if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
            return 0;
        }
        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (produced >= capacity) return 0;
            output[produced++] = (uint8_t)(accumulator >> bits);
            if (bits) accumulator &= (1u << bits) - 1u;
            else accumulator = 0;
        }
    }
    *output_len = produced;
    return 1;
}

static size_t base64_encode_unpadded(const uint8_t *input, size_t len, char *output, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, produced = 0;
    while (i < len) {
        size_t remain = len - i;
        uint32_t value = (uint32_t)input[i] << 16;
        if (remain > 1) value |= (uint32_t)input[i + 1] << 8;
        if (remain > 2) value |= input[i + 2];
        if (produced + 4 >= capacity) return 0;
        output[produced++] = alphabet[(value >> 18) & 63u];
        output[produced++] = alphabet[(value >> 12) & 63u];
        if (remain > 1) output[produced++] = alphabet[(value >> 6) & 63u];
        if (remain > 2) output[produced++] = alphabet[value & 63u];
        i += remain > 3 ? 3 : remain;
    }
    output[produced] = '\0';
    return produced;
}

static int parse_ipv4(const char *text, uint8_t output[4])
{
    unsigned part = 0, count = 0, digits = 0;
    if (!text) return 0;
    while (*text) {
        if (*text >= '0' && *text <= '9') {
            part = part * 10u + (unsigned)(*text - '0');
            if (part > 255u) return 0;
            digits++;
        } else if (*text == '.' && digits && count < 3u) {
            output[count++] = (uint8_t)part; part = 0; digits = 0;
        } else return 0;
        ++text;
    }
    if (!digits || count != 3u) return 0;
    output[3] = (uint8_t)part;
    return 1;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int parse_mac(const char *text, uint8_t output[6])
{
    int high = -1;
    unsigned count = 0;
    if (!text) return 0;
    while (*text) {
        int value = hex_value(*text++);
        if (value < 0) continue;
        if (high < 0) high = value;
        else {
            if (count >= 6u) return 0;
            output[count++] = (uint8_t)((high << 4) | value);
            high = -1;
        }
    }
    return count == 6u && high < 0;
}

static int32_t sequence_distance(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b);
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}

static int table_integer(lua_State *L, const module_host_api_v1 *host,
                         const char *key, int64_t default_value, int64_t *value)
{
    host->lua.getfield(L, 1, key);
    if (host->lua.isnumber(L, -1)) *value = host->lua.tointeger(L, -1);
    else *value = default_value;
    host->lua.settop(L, 1);
    return 1;
}

static int parse_fmtp(const char *text, uint32_t values[11])
{
    unsigned count = 0;
    if (!text) return 0;
    while (*text && count < 11u) {
        uint32_t value = 0;
        int digits = 0;
        while (*text == ' ' || *text == '\t') ++text;
        while (*text >= '0' && *text <= '9') {
            value = value * 10u + (uint32_t)(*text - '0'); ++text; digits = 1;
        }
        if (!digits) break;
        values[count++] = value;
        while (*text == ' ' || *text == '\t') ++text;
    }
    return count == 11u;
}

static int ensure_buffers(airplay_instance_t *inst)
{
    const uint32_t psram = MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT;
    const uint32_t internal = MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT;
    if (!inst->slots) inst->slots = (airplay_packet_t *)inst->host.heap.calloc(AIRPLAY_PACKET_SLOTS, sizeof(airplay_packet_t), psram);
    if (!inst->pcm_slots && !inst->pcm_queue_chunked) {
        size_t internal_free = inst->host.heap.free_size ? inst->host.heap.free_size(internal) : 0u;
        size_t queue_bytes = AIRPLAY_PCM_SLOTS * sizeof(airplay_pcm_packet_t);
        unsigned chunk;
        if (internal_free > queue_bytes + AIRPLAY_INTERNAL_HEAP_RESERVE) {
            for (chunk = 0; chunk < AIRPLAY_PCM_CHUNKS; ++chunk) {
                inst->pcm_chunks[chunk] = (airplay_pcm_packet_t *)inst->host.heap.calloc(
                    AIRPLAY_PCM_CHUNK_SLOTS, sizeof(airplay_pcm_packet_t), internal);
                if (!inst->pcm_chunks[chunk]) break;
            }
            if (chunk == AIRPLAY_PCM_CHUNKS) inst->pcm_queue_chunked = 1u;
            else {
                while (chunk > 0u) {
                    --chunk;
                    inst->host.heap.free(inst->pcm_chunks[chunk]);
                    inst->pcm_chunks[chunk] = NULL;
                }
            }
        }
        inst->pcm_queue_internal = inst->pcm_queue_chunked ? 1u : 0u;
        if (!inst->pcm_queue_chunked && !inst->pcm_slots) {
            inst->pcm_slots = (airplay_pcm_packet_t *)inst->host.heap.calloc(
                AIRPLAY_PCM_SLOTS, sizeof(airplay_pcm_packet_t), psram);
        }
    }
    if (!inst->encoded_buf && !inst->decrypt_buf && !inst->pcm_buf) {
        inst->encoded_buf = (uint8_t *)inst->host.heap.malloc(AIRPLAY_PACKET_MAX, internal);
        inst->decrypt_buf = (uint8_t *)inst->host.heap.malloc(AIRPLAY_PACKET_MAX, internal);
        inst->pcm_buf = (uint8_t *)inst->host.heap.malloc(AIRPLAY_PCM_MAX, internal);
        inst->workspace_internal = (inst->encoded_buf && inst->decrypt_buf && inst->pcm_buf) ? 1u : 0u;
        if (!inst->workspace_internal) {
            if (inst->encoded_buf) inst->host.heap.free(inst->encoded_buf);
            if (inst->decrypt_buf) inst->host.heap.free(inst->decrypt_buf);
            if (inst->pcm_buf) inst->host.heap.free(inst->pcm_buf);
            inst->encoded_buf = NULL; inst->decrypt_buf = NULL; inst->pcm_buf = NULL;
        }
    }
    if (!inst->encoded_buf) inst->encoded_buf = (uint8_t *)inst->host.heap.malloc(AIRPLAY_PACKET_MAX, psram);
    if (!inst->decrypt_buf) inst->decrypt_buf = (uint8_t *)inst->host.heap.malloc(AIRPLAY_PACKET_MAX, psram);
    if (!inst->pcm_buf) inst->pcm_buf = (uint8_t *)inst->host.heap.malloc(AIRPLAY_PCM_MAX, psram);
    if (!inst->slots || (!inst->pcm_slots && !inst->pcm_queue_chunked) ||
        !inst->encoded_buf || !inst->decrypt_buf || !inst->pcm_buf) {
        set_error(inst, "airplay: not enough memory for jitter/decode buffers");
        return 0;
    }
    return 1;
}

static airplay_pcm_packet_t *pcm_slot_at(airplay_instance_t *inst, uint32_t serial)
{
    uint32_t index;
    if (!inst) return NULL;
    index = serial & (AIRPLAY_PCM_SLOTS - 1u);
    if (inst->pcm_queue_chunked) {
        uint32_t chunk = index / AIRPLAY_PCM_CHUNK_SLOTS;
        uint32_t offset = index & (AIRPLAY_PCM_CHUNK_SLOTS - 1u);
        return inst->pcm_chunks[chunk] ? &inst->pcm_chunks[chunk][offset] : NULL;
    }
    return inst->pcm_slots ? &inst->pcm_slots[index] : NULL;
}

static void clear_slots(airplay_instance_t *inst)
{
    unsigned i;
    if (!inst) return;
    inst->flush_serial++;
    if (inst->slots) for (i = 0; i < AIRPLAY_PACKET_SLOTS; ++i) inst->slots[i].valid = 0;
    inst->packet_count = 0;
    inst->first_valid = 0;
    inst->expected_valid = 0;
    inst->newest_valid = 0;
    inst->missing_ticks = 0;
    inst->missing_burst_active = 0;
    inst->playing = 0;
    inst->buffering = inst->task_running ? 1u : 0u;
    inst->left_peak = 0;
    inst->right_peak = 0;
    inst->resend_pending_valid = 0;
    inst->resend_priority = 0;
}

static uint32_t pcm_packets_used(const airplay_instance_t *inst)
{
    return inst ? (uint32_t)(inst->pcm_write_serial - inst->pcm_read_serial) : 0u;
}

static void clear_pcm_slots(airplay_instance_t *inst)
{
    unsigned i;
    if (!inst) return;
    if (inst->pcm_slots || inst->pcm_queue_chunked) {
        for (i = 0; i < AIRPLAY_PCM_SLOTS; ++i) {
            airplay_pcm_packet_t *slot = pcm_slot_at(inst, i);
            if (slot) slot->valid = 0;
        }
    }
    inst->pcm_write_serial = 0;
    inst->pcm_read_serial = 0;
}

static void reset_counters(airplay_instance_t *inst)
{
    if (!inst) return;
    inst->resend_sequence = 0; inst->resend_count = 0; inst->resend_serial = 0;
    inst->resend_batch_max = 0; inst->resend_packets_requested = 0;
    inst->resend_requests_published = 0; inst->last_resend_publish_us = 0;
    inst->early_resend_requests = 0; inst->early_resend_packets = 0;
    inst->packets_received = 0; inst->packets_ingested = 0; inst->bytes_ingested = 0;
    inst->packets_dropped = 0;
    inst->packets_invalid = 0; inst->packets_payload_dropped = 0;
    inst->packets_slot_collision = 0; inst->packets_duplicate = 0;
    inst->packets_late = 0; inst->packets_lost = 0;
    inst->loss_bursts = 0; inst->burst_loss_packets = 0;
    inst->packets_decoded = 0; inst->bytes_written = 0;
    inst->decode_errors = 0; inst->buffer_resyncs = 0; inst->buffer_resync_discarded = 0;
    inst->pcm_underruns = 0;
    inst->packet_buffer_high_water = 0; inst->packet_span_high_water = 0;
    inst->pcm_buffer_high_water = 0;
    inst->aes_us_total = 0; inst->decode_us_total = 0; inst->packet_us_total = 0;
    inst->i2s_write_us_total = 0;
    inst->last_rtp_us = 0; inst->last_producer_us = 0; inst->last_output_us = 0;
    inst->aes_calls = 0; inst->i2s_write_calls = 0; inst->aes_us_max = 0;
    inst->decode_us_max = 0; inst->packet_us_max = 0;
    inst->i2s_write_us_max = 0; inst->rtp_gap_us_max = 0;
    inst->rtp_gap_over_20ms = 0; inst->rtp_gap_over_100ms = 0; inst->rtp_gap_over_500ms = 0;
    inst->producer_gap_us_max = 0; inst->output_gap_us_max = 0;
}

static uint64_t profile_now_us(const airplay_instance_t *inst)
{
    return inst && inst->host.time.micros ? inst->host.time.micros() : 0;
}

static uint32_t profile_elapsed_us(uint64_t started, uint64_t finished)
{
    uint64_t elapsed = finished >= started ? finished - started : 0;
    return elapsed > 0xffffffffu ? 0xffffffffu : (uint32_t)elapsed;
}

static void profile_record(uint64_t *total, volatile uint32_t *maximum, uint32_t elapsed)
{
    *total += elapsed;
    if (elapsed > *maximum) *maximum = elapsed;
}

static uint32_t discard_before(airplay_instance_t *inst, uint16_t sequence)
{
    unsigned i;
    uint32_t discarded = 0;
    if (!inst || !inst->slots) return 0;
    for (i = 0; i < AIRPLAY_PACKET_SLOTS; ++i) {
        airplay_packet_t *slot = &inst->slots[i];
        if (slot->valid && sequence_distance(slot->sequence, sequence) < 0) {
            slot->valid = 0;
            if (inst->packet_count) inst->packet_count--;
            discarded++;
        }
    }
    return discarded;
}

static int slot_is_present(const airplay_instance_t *inst, uint16_t sequence)
{
    const airplay_packet_t *slot;
    if (!inst || !inst->slots) return 0;
    slot = &inst->slots[sequence & (AIRPLAY_PACKET_SLOTS - 1u)];
    return slot->valid && slot->sequence == sequence;
}

static int request_resend(airplay_instance_t *inst, uint16_t sequence,
                          uint16_t count, uint8_t priority)
{
    if (!inst) return 0;
    if (!count) count = 1u;
    if (count > AIRPLAY_RESEND_MAX_COUNT) count = AIRPLAY_RESEND_MAX_COUNT;
    if (inst->resend_pending_valid && inst->resend_sequence == sequence) {
        int changed = 0;
        if (priority > inst->resend_priority) {
            inst->resend_priority = priority;
            changed = 1;
        }
        if (count <= inst->resend_count) return changed;
        inst->resend_count = count;
        return 1;
    }
    if (inst->resend_pending_valid && priority < inst->resend_priority) return 0;
    inst->resend_sequence = sequence;
    inst->resend_count = count;
    inst->resend_priority = priority;
    inst->resend_pending_valid = 1;
    return 1;
}

static int publish_resend_if_due(airplay_instance_t *inst)
{
    uint64_t now;
    if (!inst || !inst->resend_pending_valid) return 0;
    now = profile_now_us(inst);
    if (inst->last_resend_publish_us && now &&
        profile_elapsed_us(inst->last_resend_publish_us, now) <
            AIRPLAY_RESEND_MIN_INTERVAL_US) {
        return 0;
    }
    inst->last_resend_publish_us = now;
    inst->resend_serial++;
    inst->resend_requests_published++;
    inst->resend_packets_requested += inst->resend_count;
    if (inst->resend_count > inst->resend_batch_max) {
        inst->resend_batch_max = inst->resend_count;
    }
    return 1;
}

static void request_buffer_gap(airplay_instance_t *inst)
{
    int32_t ahead;
    uint16_t offset;
    uint16_t limit;
    if (!inst || !inst->expected_valid || !inst->newest_valid) return;
    ahead = sequence_distance(inst->newest_sequence, inst->expected_sequence);
    if (ahead <= (int32_t)AIRPLAY_RESEND_TAIL_GUARD) {
        inst->resend_pending_valid = 0;
        inst->resend_priority = 0;
        return;
    }
    limit = (uint16_t)(ahead - (int32_t)AIRPLAY_RESEND_TAIL_GUARD);
    if (limit > inst->prebuffer_packets) limit = inst->prebuffer_packets;
    for (offset = 0; offset <= limit; ++offset) {
        uint16_t sequence = (uint16_t)(inst->expected_sequence + offset);
        if (!slot_is_present(inst, sequence)) {
            uint16_t count = 1u;
            while (count < AIRPLAY_RESEND_MAX_COUNT &&
                   (uint16_t)(offset + count) <= limit &&
                   !slot_is_present(inst, (uint16_t)(sequence + count))) {
                count++;
            }
            request_resend(inst, sequence, count, AIRPLAY_RESEND_PRIORITY_CRITICAL);
            return;
        }
    }
    inst->resend_pending_valid = 0;
    inst->resend_priority = 0;
}

static void advance_expected_sequence(airplay_instance_t *inst)
{
    if (!inst) return;
    inst->expected_sequence++;
    if (inst->resend_pending_valid &&
        sequence_distance(inst->resend_sequence, inst->expected_sequence) < 0) {
        inst->resend_pending_valid = 0;
        inst->resend_priority = 0;
    }
}

static void close_decoder(airplay_instance_t *inst)
{
    if (inst && inst->decoder) {
        esp_alac_dec_close(inst->decoder);
        inst->decoder = NULL;
    }
}

static int decode_alac(airplay_instance_t *inst, const uint8_t *data, size_t len, size_t *pcm_len)
{
    esp_audio_dec_in_raw_t raw;
    esp_audio_dec_out_frame_t frame;
    esp_audio_dec_info_t info;
    esp_audio_err_t result;
    zero_bytes(&raw, sizeof(raw)); zero_bytes(&frame, sizeof(frame)); zero_bytes(&info, sizeof(info));
    raw.buffer = (uint8_t *)data; raw.len = (uint32_t)len;
    frame.buffer = inst->pcm_buf; frame.len = AIRPLAY_PCM_MAX;
    result = esp_alac_dec_decode(inst->decoder, &raw, &frame, &info);
    if (result != ESP_AUDIO_ERR_OK || frame.decoded_size == 0 || frame.decoded_size > AIRPLAY_PCM_MAX) return 0;
    *pcm_len = frame.decoded_size;
    return 1;
}

static int decode_l16(airplay_instance_t *inst, const uint8_t *data, size_t len, size_t *pcm_len)
{
    size_t i;
    len &= ~(size_t)1u;
    if (len > AIRPLAY_PCM_MAX) return 0;
    for (i = 0; i < len; i += 2) {
        inst->pcm_buf[i] = data[i + 1];
        inst->pcm_buf[i + 1] = data[i];
    }
    *pcm_len = len;
    return len > 0;
}

static void apply_volume_and_levels(airplay_instance_t *inst, size_t pcm_len)
{
    size_t samples = pcm_len / 2u;
    size_t i;
    uint16_t left = 0, right = 0;
    unsigned channel = 0;
    if (inst->gain_q15 == 32768u) {
        size_t step = (size_t)inst->channels * 4u;
        if (!step) step = 4u;
        for (i = 0; i < samples; i += step) {
            const uint8_t *p = inst->pcm_buf + i * 2u;
            int32_t sample = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            uint32_t magnitude = sample < 0 ? (uint32_t)(-sample) : (uint32_t)sample;
            if (magnitude > 32767u) magnitude = 32767u;
            if (magnitude > left) left = (uint16_t)magnitude;
            if (inst->channels > 1u && i + 1u < samples) {
                p += 2;
                sample = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
                magnitude = sample < 0 ? (uint32_t)(-sample) : (uint32_t)sample;
                if (magnitude > 32767u) magnitude = 32767u;
                if (magnitude > right) right = (uint16_t)magnitude;
            }
        }
        if (inst->channels == 1u) right = left;
        inst->left_peak = left; inst->right_peak = right;
        return;
    }
    for (i = 0; i < samples; ++i) {
        uint8_t *p = inst->pcm_buf + i * 2u;
        int32_t sample = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        int32_t scaled = (sample * (int32_t)inst->gain_q15) >> 15;
        uint32_t magnitude;
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;
        p[0] = (uint8_t)scaled; p[1] = (uint8_t)((uint16_t)scaled >> 8);
        magnitude = scaled < 0 ? (uint32_t)(-scaled) : (uint32_t)scaled;
        if (magnitude > 32767u) magnitude = 32767u;
        if (channel == 0) { if (magnitude > left) left = (uint16_t)magnitude; }
        else { if (magnitude > right) right = (uint16_t)magnitude; }
        channel++;
        if (channel >= inst->channels) channel = 0;
    }
    if (inst->channels == 1) right = left;
    inst->left_peak = left; inst->right_peak = right;
}

static int write_pcm(airplay_instance_t *inst, const uint8_t *pcm, size_t pcm_len)
{
    size_t position = 0;
    while (position < pcm_len && !inst->task_stop) {
        size_t written = 0;
        int32_t result = inst->host.i2s.write(inst->i2s_stream, pcm + position,
                                              pcm_len - position, &written, 20);
        if (result != MODULE_OK) return 0;
        if (written == 0) {
            if (inst->host.task.delay) inst->host.task.delay(1);
            continue;
        }
        position += written;
        inst->bytes_written += (uint32_t)written;
    }
    return position == pcm_len;
}

static void play_task_exit(airplay_instance_t *inst)
{
    module_task_api_t task = {0};
    module_time_api_t time = {0};
    void *handle = NULL;
    if (inst) {
        task = inst->host.task; time = inst->host.time; handle = inst->play_task;
        inst->task_running = 0; inst->play_task = NULL;
    } else if (s_host) { task = s_host->task; time = s_host->time; }
    if (task.remove) task.remove(handle);
    for (;;) {
        if (task.delay) task.delay(1000);
        else if (time.delay) time.delay(1000);
        else if (task.yield) task.yield();
    }
}

static void output_task_exit(airplay_instance_t *inst)
{
    module_task_api_t task = {0};
    module_time_api_t time = {0};
    void *handle = NULL;
    if (inst) {
        task = inst->host.task; time = inst->host.time; handle = inst->output_task;
        inst->output_task_running = 0; inst->output_task = NULL;
        inst->playing = 0; inst->buffering = 0;
    } else if (s_host) { task = s_host->task; time = s_host->time; }
    if (task.remove) task.remove(handle);
    for (;;) {
        if (task.delay) task.delay(1000);
        else if (time.delay) time.delay(1000);
        else if (task.yield) task.yield();
    }
}

static void play_task_entry(void *arg)
{
    airplay_instance_t *inst = (airplay_instance_t *)arg;
    if (!inst) play_task_exit(NULL);
    inst->task_running = 1;
    inst->buffering = 1;
    while (!inst->task_stop) {
        airplay_packet_t *slot;
        airplay_pcm_packet_t *pcm_slot;
        size_t encoded_len = 0, pcm_len = 0;
        uint32_t generation = inst->flush_serial;
        uint64_t packet_started = 0;

        if (pcm_packets_used(inst) >= AIRPLAY_PCM_SLOTS) {
            if (inst->host.task.delay) inst->host.task.delay(1);
            continue;
        }

        if (!inst->expected_valid) {
            if (!inst->first_valid || inst->packet_count < inst->prebuffer_packets) {
                if (inst->host.task.delay) inst->host.task.delay(2);
                continue;
            }
            inst->expected_sequence = inst->first_sequence;
            inst->expected_valid = 1;
        }

        /* Never synthesize packets the sender has not been observed sending.
         * Advancing past newest_sequence turns resumed traffic into "late"
         * packets and creates a self-sustaining dropout after a callback stall.
         */
        if (inst->newest_valid &&
            sequence_distance(inst->expected_sequence, inst->newest_sequence) > 0) {
            if (inst->host.task.delay) inst->host.task.delay(2);
            continue;
        }

        if (inst->newest_valid) {
            int32_t span = sequence_distance(inst->newest_sequence,
                                             inst->expected_sequence);
            if (span > 0 && span > (int32_t)inst->packet_span_high_water) {
                inst->packet_span_high_water = span > 0xffff ? 0xffffu : (uint16_t)span;
            }
        }

        /* A lossy buffer can have only a few valid slots while its sequence
         * span is already larger than the ring.  Gating this recovery on
         * packet_count lets old slots block every packet one ring later.
         */
        if (inst->newest_valid &&
            sequence_distance(inst->newest_sequence, inst->expected_sequence) >
                (int32_t)AIRPLAY_RESYNC_HIGH_WATER) {
            uint16_t target = (uint16_t)(inst->newest_sequence - inst->prebuffer_packets);
            inst->buffer_resync_discarded += discard_before(inst, target);
            inst->expected_sequence = target;
            inst->missing_ticks = 0;
            inst->missing_burst_active = 0;
            inst->buffer_resyncs++;
        }

        request_buffer_gap(inst);

        slot = &inst->slots[inst->expected_sequence & (AIRPLAY_PACKET_SLOTS - 1u)];
        if (!slot->valid || slot->sequence != inst->expected_sequence) {
            if (!inst->missing_burst_active) {
                inst->missing_ticks++;
                if (inst->missing_ticks == 1u) {
                    request_resend(inst, inst->expected_sequence, 1u,
                                   AIRPLAY_RESEND_PRIORITY_CRITICAL);
                }
                if (inst->missing_ticks < inst->missing_wait_ticks &&
                    pcm_packets_used(inst) > AIRPLAY_PCM_RETRANSMIT_FLOOR) {
                    if (inst->host.task.delay) inst->host.task.delay(2);
                    continue;
                }
                inst->missing_burst_active = 1;
                inst->loss_bursts++;
            }
            inst->missing_ticks = 0;
            inst->packets_lost++;
            inst->burst_loss_packets++;
            advance_expected_sequence(inst);
            pcm_len = (size_t)inst->frame_length * inst->channels * 2u;
            if (pcm_len > AIRPLAY_PCM_MAX) pcm_len = AIRPLAY_PCM_MAX;
            zero_bytes(inst->pcm_buf, pcm_len);
        } else {
            packet_started = profile_now_us(inst);
            if (inst->last_producer_us) {
                uint32_t gap = profile_elapsed_us(inst->last_producer_us, packet_started);
                if (gap > inst->producer_gap_us_max) inst->producer_gap_us_max = gap;
            }
            inst->last_producer_us = packet_started;
            encoded_len = slot->len;
            copy_bytes(inst->encoded_buf, slot->data, encoded_len);
            slot->valid = 0;
            if (inst->packet_count) inst->packet_count--;
            inst->missing_ticks = 0;
            inst->missing_burst_active = 0;
            /* Publish the consumed sequence before AES/ALAC work.  Otherwise
             * a retransmit arriving during the decode window can refill the
             * slot we just cleared and remain there for an entire ring turn.
             */
            advance_expected_sequence(inst);

            if (inst->encrypted) {
                size_t encrypted_len = encoded_len & ~(size_t)15u;
                if (encrypted_len) {
                    uint64_t started = profile_now_us(inst);
                    uint32_t elapsed;
                    airplay_aes128_cbc_decrypt_ctx(&inst->aes_ctx, inst->aes_iv,
                                                   inst->encoded_buf, inst->decrypt_buf,
                                                   encrypted_len);
                    elapsed = profile_elapsed_us(started, profile_now_us(inst));
                    profile_record(&inst->aes_us_total, &inst->aes_us_max, elapsed);
                    inst->aes_calls++;
                }
                if (encrypted_len < encoded_len) copy_bytes(inst->decrypt_buf + encrypted_len,
                                                             inst->encoded_buf + encrypted_len,
                                                             encoded_len - encrypted_len);
            } else copy_bytes(inst->decrypt_buf, inst->encoded_buf, encoded_len);

            if (inst->codec == AIRPLAY_CODEC_ALAC) {
                uint64_t started = profile_now_us(inst);
                uint32_t elapsed;
                if (!decode_alac(inst, inst->decrypt_buf, encoded_len, &pcm_len)) pcm_len = 0;
                elapsed = profile_elapsed_us(started, profile_now_us(inst));
                profile_record(&inst->decode_us_total, &inst->decode_us_max, elapsed);
            } else {
                if (!decode_l16(inst, inst->decrypt_buf, encoded_len, &pcm_len)) pcm_len = 0;
            }
            if (generation != inst->flush_serial) pcm_len = 0;
            if (!pcm_len) {
                inst->decode_errors++;
                continue;
            }
            inst->packets_decoded++;
        }

        apply_volume_and_levels(inst, pcm_len);
        if (pcm_len > AIRPLAY_PCM_PACKET_MAX) {
            set_error(inst, "airplay: decoded packet exceeds PCM queue slot");
            break;
        }
        pcm_slot = pcm_slot_at(inst, inst->pcm_write_serial);
        if (!pcm_slot) {
            set_error(inst, "airplay: PCM queue slot unavailable");
            break;
        }
        copy_bytes(pcm_slot->data, inst->pcm_buf, pcm_len);
        pcm_slot->len = (uint16_t)pcm_len;
        pcm_slot->generation = generation;
        memory_barrier();
        pcm_slot->valid = 1;
        memory_barrier();
        inst->pcm_write_serial++;
        {
            uint32_t used = pcm_packets_used(inst);
            if (used > inst->pcm_buffer_high_water) inst->pcm_buffer_high_water = (uint16_t)used;
        }
        if (packet_started) {
            uint32_t elapsed = profile_elapsed_us(packet_started, profile_now_us(inst));
            profile_record(&inst->packet_us_total, &inst->packet_us_max, elapsed);
        }
        if (inst->host.task.yield) inst->host.task.yield();
    }
    play_task_exit(inst);
}

static void output_task_entry(void *arg)
{
    airplay_instance_t *inst = (airplay_instance_t *)arg;
    uint8_t started = 0;
    if (!inst) output_task_exit(NULL);
    inst->output_task_running = 1;
    while (!inst->task_stop) {
        airplay_pcm_packet_t *slot;
        uint32_t used = pcm_packets_used(inst);
        if (!started) {
            if (used < AIRPLAY_PCM_START_PACKETS) {
                if (inst->host.task.delay) inst->host.task.delay(1);
                continue;
            }
            started = 1;
            inst->buffering = 0;
            inst->playing = 1;
        }
        if (used == 0) {
            inst->pcm_underruns++;
            started = 0;
            inst->playing = 0;
            inst->buffering = 1;
            continue;
        }
        slot = pcm_slot_at(inst, inst->pcm_read_serial);
        if (!slot) {
            set_error(inst, "airplay: PCM output slot unavailable");
            break;
        }
        if (!slot->valid) {
            if (inst->host.task.yield) inst->host.task.yield();
            continue;
        }
        memory_barrier();
        if (slot->generation == inst->flush_serial) {
            uint64_t started_us = profile_now_us(inst);
            uint32_t elapsed;
            if (inst->last_output_us) {
                uint32_t gap = profile_elapsed_us(inst->last_output_us, started_us);
                if (gap > inst->output_gap_us_max) inst->output_gap_us_max = gap;
            }
            inst->last_output_us = started_us;
            if (!write_pcm(inst, slot->data, slot->len)) {
                set_error(inst, "airplay: i2s write failed");
                inst->task_stop = 1;
                break;
            }
            elapsed = profile_elapsed_us(started_us, profile_now_us(inst));
            profile_record(&inst->i2s_write_us_total, &inst->i2s_write_us_max, elapsed);
            inst->i2s_write_calls++;
        }
        slot->valid = 0;
        memory_barrier();
        inst->pcm_read_serial++;
        if (inst->host.task.yield) inst->host.task.yield();
    }
    output_task_exit(inst);
}

static void stop_internal(airplay_instance_t *inst)
{
    uint32_t waited = 0;
    if (!inst) return;
    if (inst->play_task || inst->task_running || inst->output_task || inst->output_task_running) {
        inst->task_stop = 1;
        while ((inst->task_running || inst->output_task_running) &&
               waited < AIRPLAY_TASK_STOP_WAIT_MS) {
            if (inst->host.task.delay) inst->host.task.delay(1);
            else if (inst->host.time.delay) inst->host.time.delay(1);
            waited++;
        }
        if (inst->play_task && inst->host.task.remove) {
            inst->host.task.remove(inst->play_task);
            inst->task_running = 0;
        }
        if (inst->output_task && inst->host.task.remove) {
            inst->host.task.remove(inst->output_task);
            inst->output_task_running = 0;
        }
        if (!inst->task_running) inst->play_task = NULL;
        if (!inst->output_task_running) inst->output_task = NULL;
    }
    if (inst->i2s_stream && inst->host.i2s.end) {
        inst->host.i2s.end(inst->i2s_stream);
        inst->i2s_stream = NULL;
    }
    inst->task_stop = 0; inst->playing = 0; inst->buffering = 0;
    clear_pcm_slots(inst);
}

static int l_capabilities(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    if (!host) return 0;
    host->lua.newtable(L);
    host->lua.pushboolean(L, 1); host->lua.setfield(L, -2, "apple_challenge");
    host->lua.pushboolean(L, 1); host->lua.setfield(L, -2, "aes");
    host->lua.pushboolean(L, 1); host->lua.setfield(L, -2, "alac");
    host->lua.pushboolean(L, 1); host->lua.setfield(L, -2, "l16");
    host->lua.pushboolean(L, 1); host->lua.setfield(L, -2, "rtp_ingest");
    return 1;
}

static int l_apple_response(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    const char *challenge, *ip, *mac;
    size_t challenge_text_len = 0, challenge_len = 0;
    uint8_t data[32], signature[AIRPLAY_RSA_BYTES], ip_bytes[4], mac_bytes[6];
    char encoded[345];
    if (!host) return 0;
    challenge = host->lua.tolstring(L, 1, &challenge_text_len);
    ip = host->lua.tostring(L, 2); mac = host->lua.tostring(L, 3);
    zero_bytes(data, sizeof(data));
    if (!challenge || !base64_decode(challenge, challenge_text_len, data, 16, &challenge_len) ||
        challenge_len == 0 || challenge_len + 10u > sizeof(data) ||
        !parse_ipv4(ip, ip_bytes) || !parse_mac(mac, mac_bytes)) {
        return push_error(L, host, "airplay: invalid Apple-Challenge/IP/MAC");
    }
    copy_bytes(data + challenge_len, ip_bytes, sizeof(ip_bytes));
    copy_bytes(data + challenge_len + sizeof(ip_bytes), mac_bytes, sizeof(mac_bytes));
    if (!airplay_rsa_pkcs1_sign_raw(data, sizeof(data), signature) ||
        !base64_encode_unpadded(signature, sizeof(signature), encoded, sizeof(encoded))) {
        return push_error(L, host, "airplay: Apple-Challenge RSA failed");
    }
    host->lua.pushstring(L, encoded);
    zero_bytes(data, sizeof(data)); zero_bytes(signature, sizeof(signature));
    return 1;
}

static int l_configure(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    const char *codec;
    int64_t value;
    if (!inst || !host || !host->lua.istable(L, 1)) return push_error(L, host, "airplay: configure expects a table");
    if (inst->task_running) return push_error(L, host, "airplay: cannot configure while playing");
    close_decoder(inst); clear_slots(inst); reset_counters(inst);
    inst->configured = 0; inst->encrypted = 0; inst->codec = AIRPLAY_CODEC_NONE;
    inst->sample_rate = 44100; inst->channels = 2; inst->bits = 16;
    inst->frame_length = 352; inst->payload_type = 96; inst->last_error = NULL;

    host->lua.getfield(L, 1, "codec"); codec = host->lua.tostring(L, -1);
    if (string_equal(codec, "alac")) inst->codec = AIRPLAY_CODEC_ALAC;
    else if (string_equal(codec, "l16")) inst->codec = AIRPLAY_CODEC_L16;
    host->lua.settop(L, 1);
    if (inst->codec == AIRPLAY_CODEC_NONE) return push_error(L, host, "airplay: unsupported codec");

    table_integer(L, host, "sample_rate", 44100, &value); if (value > 0 && value <= 192000) inst->sample_rate = (uint32_t)value;
    table_integer(L, host, "channels", 2, &value); if (value == 1 || value == 2) inst->channels = (uint16_t)value;
    table_integer(L, host, "payload_type", 96, &value); if (value >= 0 && value <= 127) inst->payload_type = (uint16_t)value;
    if (!ensure_buffers(inst)) return push_error(L, host, inst->last_error);

    if (inst->codec == AIRPLAY_CODEC_ALAC) {
        const char *fmtp;
        uint32_t values[11];
        esp_alac_dec_cfg_t cfg;
        host->lua.getfield(L, 1, "fmtp"); fmtp = host->lua.tostring(L, -1);
        if (!parse_fmtp(fmtp, values)) { host->lua.settop(L, 1); return push_error(L, host, "airplay: invalid ALAC fmtp"); }
        inst->frame_length = values[0]; inst->bits = (uint16_t)values[2];
        inst->channels = (uint16_t)values[6]; inst->sample_rate = values[10];
        write_be32(inst->magic_cookie, values[0]); inst->magic_cookie[4] = (uint8_t)values[1];
        inst->magic_cookie[5] = (uint8_t)values[2]; inst->magic_cookie[6] = (uint8_t)values[3];
        inst->magic_cookie[7] = (uint8_t)values[4]; inst->magic_cookie[8] = (uint8_t)values[5];
        inst->magic_cookie[9] = (uint8_t)values[6]; write_be16(inst->magic_cookie + 10, (uint16_t)values[7]);
        write_be32(inst->magic_cookie + 12, values[8]); write_be32(inst->magic_cookie + 16, values[9]);
        write_be32(inst->magic_cookie + 20, values[10]);
        host->lua.settop(L, 1);

        host->lua.getfield(L, 1, "rsaaeskey");
        if (!host->lua.isnil(L, -1)) {
            const char *key_text; size_t key_text_len = 0, cipher_len = 0, key_len = 0;
            uint8_t cipher[AIRPLAY_RSA_BYTES], key[32];
            key_text = host->lua.tolstring(L, -1, &key_text_len);
            if (!key_text || !base64_decode(key_text, key_text_len, cipher, sizeof(cipher), &cipher_len) ||
                cipher_len != AIRPLAY_RSA_BYTES ||
                !airplay_rsa_oaep_decrypt(cipher, key, sizeof(key), &key_len) || key_len != 16u) {
                host->lua.settop(L, 1); return push_error(L, host, "airplay: RSA AES-key unwrap failed");
            }
            copy_bytes(inst->aes_key, key, 16); inst->encrypted = 1;
            airplay_aes128_init(&inst->aes_ctx, inst->aes_key);
            zero_bytes(cipher, sizeof(cipher)); zero_bytes(key, sizeof(key));
        }
        host->lua.settop(L, 1);
        host->lua.getfield(L, 1, "aesiv");
        if (!host->lua.isnil(L, -1)) {
            const char *iv_text; size_t iv_text_len = 0, iv_len = 0;
            iv_text = host->lua.tolstring(L, -1, &iv_text_len);
            if (!iv_text || !base64_decode(iv_text, iv_text_len, inst->aes_iv, 16, &iv_len) || iv_len != 16u) {
                host->lua.settop(L, 1); return push_error(L, host, "airplay: invalid AES IV");
            }
        } else if (inst->encrypted) {
            host->lua.settop(L, 1); return push_error(L, host, "airplay: encrypted stream has no AES IV");
        }
        host->lua.settop(L, 1);

        zero_bytes(&cfg, sizeof(cfg)); cfg.codec_spec_info = inst->magic_cookie; cfg.spec_info_len = sizeof(inst->magic_cookie);
        if (esp_alac_dec_open(&cfg, sizeof(cfg), &inst->decoder) != ESP_AUDIO_ERR_OK || !inst->decoder) {
            return push_error(L, host, "airplay: ALAC decoder open failed");
        }
    }
    inst->configured = 1;
    host->lua.pushboolean(L, 1);
    return 1;
}

static int l_start(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    module_i2s_config_t cfg;
    int64_t value;
    int32_t result;
    uint32_t task_priority = AIRPLAY_TASK_PRIORITY;
    uint32_t output_priority;
    int32_t task_core = -1;
    int32_t output_core = 0;
    if (!inst || !host || !inst->configured) return push_error(L, host, "airplay: core is not configured");
    if (!host->i2s.begin || !host->i2s.write || !host->i2s.end || !host->task.create) return push_error(L, host, "airplay: firmware audio/task ABI unavailable");
    if (inst->task_running || inst->output_task_running) { host->lua.pushboolean(L, 1); return 1; }
    stop_internal(inst); clear_slots(inst); clear_pcm_slots(inst);
    zero_bytes(&cfg, sizeof(cfg));
    cfg.size = sizeof(cfg); cfg.port = 0; cfg.mode = MODULE_I2S_MODE_TX;
    cfg.sample_rate = inst->sample_rate; cfg.bits = 16; cfg.channels = inst->channels;
    cfg.format = MODULE_I2S_FORMAT_I2S;
    cfg.channel_mode = inst->channels > 1 ? MODULE_I2S_CHANNEL_STEREO : MODULE_I2S_CHANNEL_MONO_LEFT;
    cfg.bclk_pin = -1; cfg.ws_pin = -1; cfg.dout_pin = 48; cfg.din_pin = -1; cfg.mclk_pin = -1;
    cfg.dma_buf_count = 12; cfg.dma_buf_len = 512; cfg.flags = MODULE_I2S_FLAG_AUTO_CLEAR_TX;
    inst->prebuffer_packets = AIRPLAY_PREBUFFER_PACKETS;
    inst->missing_wait_ticks = AIRPLAY_MISSING_TICKS;
    if (host->lua.istable(L, 1)) {
        table_integer(L, host, "i2s_port", 0, &value); if (value >= 0 && value < 4) cfg.port = (uint8_t)value;
        table_integer(L, host, "data_out_pin", 48, &value); cfg.dout_pin = (int16_t)value;
        table_integer(L, host, "buffer_count", 12, &value); if (value > 0 && value <= 32) cfg.dma_buf_count = (uint16_t)value;
        table_integer(L, host, "buffer_len", 512, &value); if (value > 0 && value <= 4096) cfg.dma_buf_len = (uint16_t)value;
        table_integer(L, host, "prebuffer_packets", AIRPLAY_PREBUFFER_PACKETS, &value);
        if (value > 0 && value <= 192) inst->prebuffer_packets = (uint16_t)value;
        table_integer(L, host, "missing_wait_ticks", AIRPLAY_MISSING_TICKS, &value);
        if (value >= 1 && value <= 192) inst->missing_wait_ticks = (uint8_t)value;
        table_integer(L, host, "task_priority", AIRPLAY_TASK_PRIORITY, &value);
        if (value > 0 && value <= 24) task_priority = (uint32_t)value;
        table_integer(L, host, "task_core", -1, &value);
        if (value >= -1 && value <= 1) task_core = (int32_t)value;
        table_integer(L, host, "output_core", 0, &value);
        if (value >= -1 && value <= 1) output_core = (int32_t)value;
    }
    result = host->i2s.begin(&cfg, &inst->i2s_stream);
    if (result != MODULE_OK || !inst->i2s_stream) return push_error(L, host, "airplay: i2s begin failed");
    inst->task_stop = 0; inst->buffering = 1;
    output_priority = task_priority < 24u ? task_priority + 1u : task_priority;
    result = host->task.create("airplay_i2s", output_task_entry, inst, 4096u,
                               output_priority, output_core, &inst->output_task);
    if (result != MODULE_OK || !inst->output_task) {
        host->i2s.end(inst->i2s_stream); inst->i2s_stream = NULL;
        return push_error(L, host, "airplay: i2s task create failed");
    }
    result = host->task.create("airplay_audio", play_task_entry, inst, AIRPLAY_TASK_STACK,
                               task_priority, task_core, &inst->play_task);
    if (result != MODULE_OK || !inst->play_task) {
        inst->task_stop = 1;
        if (inst->host.task.delay) inst->host.task.delay(1);
        if (inst->output_task && inst->host.task.remove) {
            inst->host.task.remove(inst->output_task);
        }
        inst->output_task = NULL; inst->output_task_running = 0; inst->task_stop = 0;
        host->i2s.end(inst->i2s_stream); inst->i2s_stream = NULL;
        return push_error(L, host, "airplay: audio task create failed");
    }
    host->lua.pushboolean(L, 1);
    return 1;
}

enum {
    AIRPLAY_INGEST_ACCEPTED = 1,
    AIRPLAY_INGEST_DUPLICATE = 2,
    AIRPLAY_INGEST_INVALID = -1,
    AIRPLAY_INGEST_PAYLOAD = -2,
    AIRPLAY_INGEST_LATE = -3,
    AIRPLAY_INGEST_FULL = -4,
};

static int ingest_rtp_packet(airplay_instance_t *inst, const uint8_t *packet, size_t len)
{
    size_t offset, payload_len;
    uint8_t first, second;
    uint16_t sequence;
    int32_t ahead_of_newest = 0;
    airplay_packet_t *slot;
    uint64_t now;
    if (!inst || !inst->configured || !inst->slots || !packet || len < 12u) {
        if (inst) { inst->packets_dropped++; inst->packets_invalid++; }
        return AIRPLAY_INGEST_INVALID;
    }

    inst->packets_ingested++;
    inst->bytes_ingested += (uint32_t)len;
    now = profile_now_us(inst);
    if (inst->last_rtp_us) {
        uint32_t gap = profile_elapsed_us(inst->last_rtp_us, now);
        if (gap > inst->rtp_gap_us_max) inst->rtp_gap_us_max = gap;
        if (gap > 20000u) inst->rtp_gap_over_20ms++;
        if (gap > 100000u) inst->rtp_gap_over_100ms++;
        if (gap > 500000u) inst->rtp_gap_over_500ms++;
    }
    inst->last_rtp_us = now;

    first = packet[0]; second = packet[1];
    if ((first >> 6) != 2u) {
        inst->packets_dropped++;
        inst->packets_invalid++;
        return AIRPLAY_INGEST_INVALID;
    }
    offset = 12u + (size_t)(first & 15u) * 4u;
    if (offset > len) {
        inst->packets_dropped++;
        inst->packets_invalid++;
        return AIRPLAY_INGEST_INVALID;
    }
    if (first & 0x10u) {
        size_t extension;
        if (offset + 4u > len) {
            inst->packets_dropped++;
            inst->packets_invalid++;
            return AIRPLAY_INGEST_INVALID;
        }
        extension = 4u + (size_t)read_be16(packet + offset + 2u) * 4u;
        if (offset + extension > len) {
            inst->packets_dropped++;
            inst->packets_invalid++;
            return AIRPLAY_INGEST_INVALID;
        }
        offset += extension;
    }
    payload_len = len - offset;
    if (first & 0x20u) {
        uint8_t padding = packet[len - 1u];
        if (!padding || padding > payload_len) {
            inst->packets_dropped++;
            inst->packets_invalid++;
            return AIRPLAY_INGEST_INVALID;
        }
        payload_len -= padding;
    }
    if (!payload_len || payload_len > AIRPLAY_PACKET_MAX ||
        (second & 0x7fu) != inst->payload_type) {
        inst->packets_dropped++;
        inst->packets_payload_dropped++;
        return AIRPLAY_INGEST_PAYLOAD;
    }

    sequence = read_be16(packet + 2);
    if (inst->newest_valid) {
        ahead_of_newest = sequence_distance(sequence, inst->newest_sequence);
    }
    if (inst->expected_valid && sequence_distance(sequence, inst->expected_sequence) < 0) {
        if (!inst->newest_valid || ahead_of_newest > 0) {
            inst->newest_sequence = sequence;
            inst->newest_valid = 1;
        }
        inst->packets_late++;
        return AIRPLAY_INGEST_LATE;
    }
    if (inst->newest_valid && ahead_of_newest > 1) {
        uint16_t count = (uint16_t)(ahead_of_newest - 1);
        if (count > AIRPLAY_RESEND_MAX_COUNT) count = AIRPLAY_RESEND_MAX_COUNT;
        if (request_resend(inst, (uint16_t)(inst->newest_sequence + 1u), count,
                           AIRPLAY_RESEND_PRIORITY_EARLY)) {
            inst->early_resend_requests++;
            inst->early_resend_packets += count;
        }
    }
    if (!inst->newest_valid || ahead_of_newest > 0) {
        inst->newest_sequence = sequence;
        inst->newest_valid = 1;
    }
    if (inst->expected_valid) {
        int32_t span = sequence_distance(inst->newest_sequence, inst->expected_sequence);
        if (span > 0 && span > (int32_t)inst->packet_span_high_water) {
            inst->packet_span_high_water = span > 0xffff ? 0xffffu : (uint16_t)span;
        }
    }
    slot = &inst->slots[sequence & (AIRPLAY_PACKET_SLOTS - 1u)];
    if (slot->valid) {
        if (slot->sequence == sequence) {
            inst->packets_duplicate++;
            return AIRPLAY_INGEST_DUPLICATE;
        }
        inst->packets_dropped++;
        inst->packets_slot_collision++;
        return AIRPLAY_INGEST_FULL;
    }

    slot->sequence = sequence; slot->timestamp = read_be32(packet + 4);
    slot->payload_type = second & 0x7fu; slot->len = (uint16_t)payload_len;
    copy_bytes(slot->data, packet + offset, payload_len);
    memory_barrier();
    slot->valid = 1;
    memory_barrier();
    inst->packet_count++; inst->packets_received++;
    if (inst->packet_count > inst->packet_buffer_high_water) {
        inst->packet_buffer_high_water = inst->packet_count;
    }
    if (inst->resend_pending_valid && inst->resend_sequence == sequence) {
        inst->resend_pending_valid = 0;
        inst->resend_priority = 0;
    }
    if (!inst->first_valid) { inst->first_sequence = sequence; inst->first_valid = 1; }
    return AIRPLAY_INGEST_ACCEPTED;
}

static int l_ingest_rtp(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    const uint8_t *packet;
    size_t len = 0;
    uint32_t observed_resend_serial = 0;
    if (!inst || !host) return 0;
    if (host->lua.isnumber(L, 2)) {
        observed_resend_serial = (uint32_t)host->lua.tointeger(L, 2);
    }
    packet = (const uint8_t *)host->lua.tolstring(L, 1, &len);
    (void)ingest_rtp_packet(inst, packet, len);
    (void)publish_resend_if_due(inst);
    if (inst->resend_pending_valid && inst->resend_serial != observed_resend_serial) {
        host->lua.pushinteger(L, (int64_t)inst->resend_sequence);
        host->lua.pushinteger(L, (int64_t)inst->resend_count);
        host->lua.pushinteger(L, (int64_t)inst->resend_serial);
        return 3;
    }
    return 0;
}

static int l_push_rtp(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    const uint8_t *packet;
    size_t len = 0;
    int result;
    if (!inst || !host || !inst->configured || !inst->slots) {
        return push_error(L, host, "airplay: core is not configured");
    }
    packet = (const uint8_t *)host->lua.tolstring(L, 1, &len);
    result = ingest_rtp_packet(inst, packet, len);
    if (result > 0) {
        host->lua.pushboolean(L, 1);
        return 1;
    }
    if (result == AIRPLAY_INGEST_LATE) return push_error(L, host, "airplay: late RTP packet");
    if (result == AIRPLAY_INGEST_FULL) return push_error(L, host, "airplay: jitter buffer full");
    if (result == AIRPLAY_INGEST_PAYLOAD) return push_error(L, host, "airplay: unexpected RTP payload");
    return push_error(L, host, "airplay: invalid RTP packet");
}

static int l_flush(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    if (inst) clear_slots(inst);
    if (host) host->lua.pushboolean(L, 1);
    return host ? 1 : 0;
}

static int l_stop(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    if (inst) stop_internal(inst);
    if (host) host->lua.pushboolean(L, 1);
    return host ? 1 : 0;
}

static int l_set_volume(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    double db = 0, linear = -1;
    if (!inst || !host) return 0;
    if (host->lua.isnumber(L, 1)) db = host->lua.tonumber(L, 1);
    if (host->lua.isnumber(L, 2)) linear = host->lua.tonumber(L, 2);
    if (db <= -144.0) inst->gain_q15 = 0;
    else if (linear >= 0.0) {
        if (linear > 1.0) linear = 1.0;
        inst->gain_q15 = (uint16_t)(linear * 32768.0 + 0.5);
    } else {
        int whole = (int)(db < 0 ? -db : 0);
        uint32_t gain = 32768;
        while (whole-- > 0) gain = (gain * 29199u) >> 15; /* about -1 dB */
        inst->gain_q15 = (uint16_t)gain;
    }
    host->lua.pushboolean(L, 1);
    return 1;
}

static void push_integer_field(lua_State *L, const module_host_api_v1 *host, const char *key, uint32_t value)
{
    host->lua.pushinteger(L, value); host->lua.setfield(L, -2, key);
}

static void push_u64_field(lua_State *L, const module_host_api_v1 *host, const char *key, uint64_t value)
{
    host->lua.pushinteger(L, (int64_t)value); host->lua.setfield(L, -2, key);
}

static int l_state(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    if (!inst || !host) return 0;
    host->lua.newtable(L);
    host->lua.pushboolean(L, inst->playing); host->lua.setfield(L, -2, "playing");
    host->lua.pushboolean(L, inst->buffering); host->lua.setfield(L, -2, "buffering");
    host->lua.pushnumber(L, (double)inst->left_peak / 32768.0); host->lua.setfield(L, -2, "left");
    host->lua.pushnumber(L, (double)inst->right_peak / 32768.0); host->lua.setfield(L, -2, "right");
    host->lua.pushstring(L, inst->codec == AIRPLAY_CODEC_ALAC ? "alac" : (inst->codec == AIRPLAY_CODEC_L16 ? "l16" : "none"));
    host->lua.setfield(L, -2, "codec");
    push_integer_field(L, host, "buffered_packets", inst->packet_count);
    push_integer_field(L, host, "ingested", inst->packets_ingested);
    push_integer_field(L, host, "ingested_bytes", inst->bytes_ingested);
    push_integer_field(L, host, "received", inst->packets_received);
    push_integer_field(L, host, "dropped", inst->packets_dropped);
    push_integer_field(L, host, "invalid", inst->packets_invalid);
    push_integer_field(L, host, "payload_dropped", inst->packets_payload_dropped);
    push_integer_field(L, host, "slot_collisions", inst->packets_slot_collision);
    push_integer_field(L, host, "duplicates", inst->packets_duplicate);
    push_integer_field(L, host, "late", inst->packets_late);
    push_integer_field(L, host, "lost", inst->packets_lost);
    push_integer_field(L, host, "loss_bursts", inst->loss_bursts);
    push_integer_field(L, host, "burst_loss_packets", inst->burst_loss_packets);
    push_integer_field(L, host, "decoded", inst->packets_decoded);
    push_integer_field(L, host, "written_bytes", inst->bytes_written);
    push_integer_field(L, host, "decode_errors", inst->decode_errors);
    push_integer_field(L, host, "buffer_resyncs", inst->buffer_resyncs);
    push_integer_field(L, host, "buffer_resync_discarded", inst->buffer_resync_discarded);
    push_integer_field(L, host, "pcm_buffered_packets", pcm_packets_used(inst));
    push_integer_field(L, host, "pcm_underruns", inst->pcm_underruns);
    push_integer_field(L, host, "packet_buffer_high_water", inst->packet_buffer_high_water);
    push_integer_field(L, host, "packet_span_high_water", inst->packet_span_high_water);
    push_integer_field(L, host, "pcm_buffer_high_water", inst->pcm_buffer_high_water);
    push_integer_field(L, host, "aes_calls", inst->aes_calls);
    push_u64_field(L, host, "aes_us_total", inst->aes_us_total);
    push_integer_field(L, host, "aes_us_avg", inst->aes_calls ? (uint32_t)(inst->aes_us_total / inst->aes_calls) : 0u);
    push_integer_field(L, host, "aes_us_max", inst->aes_us_max);
    push_u64_field(L, host, "decode_us_total", inst->decode_us_total);
    push_integer_field(L, host, "decode_us_avg", inst->packets_decoded ? (uint32_t)(inst->decode_us_total / inst->packets_decoded) : 0u);
    push_integer_field(L, host, "decode_us_max", inst->decode_us_max);
    push_u64_field(L, host, "packet_us_total", inst->packet_us_total);
    push_integer_field(L, host, "packet_us_avg", inst->packets_decoded ? (uint32_t)(inst->packet_us_total / inst->packets_decoded) : 0u);
    push_integer_field(L, host, "packet_us_max", inst->packet_us_max);
    push_u64_field(L, host, "i2s_write_us_total", inst->i2s_write_us_total);
    push_integer_field(L, host, "i2s_write_us_avg", inst->i2s_write_calls ? (uint32_t)(inst->i2s_write_us_total / inst->i2s_write_calls) : 0u);
    push_integer_field(L, host, "i2s_write_us_max", inst->i2s_write_us_max);
    push_integer_field(L, host, "rtp_gap_us_max", inst->rtp_gap_us_max);
    push_integer_field(L, host, "rtp_gap_over_20ms", inst->rtp_gap_over_20ms);
    push_integer_field(L, host, "rtp_gap_over_100ms", inst->rtp_gap_over_100ms);
    push_integer_field(L, host, "rtp_gap_over_500ms", inst->rtp_gap_over_500ms);
    push_integer_field(L, host, "producer_gap_us_max", inst->producer_gap_us_max);
    push_integer_field(L, host, "output_gap_us_max", inst->output_gap_us_max);
    push_integer_field(L, host, "workspace_internal", inst->workspace_internal);
    push_integer_field(L, host, "pcm_queue_internal", inst->pcm_queue_internal);
    push_integer_field(L, host, "pcm_queue_slots", AIRPLAY_PCM_SLOTS);
    push_integer_field(L, host, "pcm_queue_bytes", AIRPLAY_PCM_SLOTS * sizeof(airplay_pcm_packet_t));
    if (host->heap.free_size) {
        push_u64_field(L, host, "internal_free_bytes", host->heap.free_size(MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT));
        push_u64_field(L, host, "psram_free_bytes", host->heap.free_size(MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT));
    }
    if (host->heap.largest_free_block) {
        push_u64_field(L, host, "internal_largest_block", host->heap.largest_free_block(MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT));
    }
    host->lua.pushstring(L, "software-cached-tables"); host->lua.setfield(L, -2, "crypto_backend");
    push_integer_field(L, host, "expected_sequence", inst->expected_sequence);
    push_integer_field(L, host, "newest_sequence", inst->newest_sequence);
    push_integer_field(L, host, "prebuffer_packets", inst->prebuffer_packets);
    push_integer_field(L, host, "missing_wait_ticks", inst->missing_wait_ticks);
    push_integer_field(L, host, "audio_task_stack_bytes", AIRPLAY_TASK_STACK);
    push_integer_field(L, host, "resend_serial", inst->resend_serial);
    push_integer_field(L, host, "resend_sequence", inst->resend_sequence);
    push_integer_field(L, host, "resend_count", inst->resend_count);
    push_integer_field(L, host, "resend_batch_max", inst->resend_batch_max);
    push_integer_field(L, host, "resend_packets_requested", inst->resend_packets_requested);
    push_integer_field(L, host, "resend_requests_published", inst->resend_requests_published);
    push_integer_field(L, host, "early_resend_requests", inst->early_resend_requests);
    push_integer_field(L, host, "early_resend_packets", inst->early_resend_packets);
    if (inst->last_error) { host->lua.pushstring(L, inst->last_error); host->lua.setfield(L, -2, "error"); }
    return 1;
}

/*
 * Allocation-light 100 Hz service poll. Keep full state() for diagnostics,
 * but do not build its large Lua table or query heap statistics in the UDP
 * event loop just to update meters and relay a resend request.
 */
static int l_poll(lua_State *L)
{
    airplay_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    if (!inst || !host) return 0;
    (void)publish_resend_if_due(inst);
    host->lua.pushnumber(L, (double)inst->left_peak / 32768.0);
    host->lua.pushnumber(L, (double)inst->right_peak / 32768.0);
    host->lua.pushboolean(L, inst->playing);
    host->lua.pushinteger(L, (int64_t)inst->resend_serial);
    host->lua.pushinteger(L, (int64_t)inst->resend_sequence);
    host->lua.pushinteger(L, (int64_t)inst->resend_count);
    return 6;
}

AIRPLAY_EXPORT const module_manifest_t *module_query_v1(void)
{
    return &s_manifest;
}

AIRPLAY_EXPORT int32_t module_create_v2(module_host_resolve_v1_fn resolve,
                                        void *resolve_ctx,
                                        const module_open_info_t *info,
                                        void **out_instance)
{
    module_host_api_v1 host;
    airplay_instance_t *inst;
    int32_t err;
    (void)info;
    if (!out_instance) return MODULE_ERR_INVALID_ARG;
    *out_instance = NULL;
    module_sdk_zero_host_v1(&host);
    err = module_sdk_resolve_host_v1(resolve, resolve_ctx, &host);
    if (err != MODULE_OK) return err;
    inst = (airplay_instance_t *)host.heap.calloc(1, sizeof(*inst), MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
    if (!inst) return MODULE_ERR_NO_MEMORY;
    inst->host = host; inst->sample_rate = 44100; inst->channels = 2;
    inst->bits = 16; inst->frame_length = 352;
    inst->prebuffer_packets = AIRPLAY_PREBUFFER_PACKETS;
    inst->missing_wait_ticks = AIRPLAY_MISSING_TICKS;
    inst->gain_q15 = 32768u;
    s_host = &inst->host;
    *out_instance = inst;
    return MODULE_OK;
}

AIRPLAY_EXPORT int32_t module_luaopen_v1(void *instance, lua_State *L)
{
    airplay_instance_t *inst = (airplay_instance_t *)instance;
    const module_host_api_v1 *host = inst ? &inst->host : s_host;
    if (!inst || !host) return MODULE_ERR_INVALID_ARG;
    s_host = host;
    host->lua.newtable(L);
    host->lua.pushstring(L, AIRPLAY_CORE_VERSION); host->lua.setfield(L, -2, "VERSION");
    set_function_field(L, host, "capabilities", l_capabilities, inst);
    set_function_field(L, host, "apple_response", l_apple_response, inst);
    set_function_field(L, host, "configure", l_configure, inst);
    set_function_field(L, host, "start", l_start, inst);
    set_function_field(L, host, "ingest_rtp", l_ingest_rtp, inst);
    set_function_field(L, host, "push_rtp", l_push_rtp, inst);
    set_function_field(L, host, "flush", l_flush, inst);
    set_function_field(L, host, "stop", l_stop, inst);
    set_function_field(L, host, "set_volume", l_set_volume, inst);
    set_function_field(L, host, "poll", l_poll, inst);
    set_function_field(L, host, "state", l_state, inst);
    return MODULE_OK;
}

AIRPLAY_EXPORT void module_destroy_v1(void *instance)
{
    airplay_instance_t *inst = (airplay_instance_t *)instance;
    if (!inst) return;
    stop_internal(inst); close_decoder(inst);
    if (inst->slots) inst->host.heap.free(inst->slots);
    if (inst->pcm_slots) inst->host.heap.free(inst->pcm_slots);
    if (inst->pcm_queue_chunked) {
        unsigned chunk;
        for (chunk = 0; chunk < AIRPLAY_PCM_CHUNKS; ++chunk) {
            if (inst->pcm_chunks[chunk]) inst->host.heap.free(inst->pcm_chunks[chunk]);
        }
    }
    if (inst->encoded_buf) inst->host.heap.free(inst->encoded_buf);
    if (inst->decrypt_buf) inst->host.heap.free(inst->decrypt_buf);
    if (inst->pcm_buf) inst->host.heap.free(inst->pcm_buf);
    if (s_host == &inst->host) s_host = NULL;
    inst->host.heap.free(inst);
}

/* libc/IDF compatibility shims used by Espressif's statically linked ALAC. */
void *malloc(size_t size)
{
    if (!s_host || !s_host->heap.malloc) return NULL;
    return s_host->heap.malloc(size, MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
}

void *calloc(size_t n, size_t size)
{
    if (!s_host || !s_host->heap.calloc) return NULL;
    return s_host->heap.calloc(n, size, MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
}

void *realloc(void *ptr, size_t size)
{
    if (!s_host || !s_host->heap.realloc) return NULL;
    return s_host->heap.realloc(ptr, size, MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
}

void free(void *ptr)
{
    if (s_host && s_host->heap.free) s_host->heap.free(ptr);
}

/* The ALAC adapter is C++, but its only C++ runtime needs are scalar new/delete. */
void *_Znwj(unsigned int size) { return malloc(size); }
void _ZdlPvj(void *ptr, unsigned int size) { (void)size; free(ptr); }

void *memcpy(void *dst, const void *src, size_t n) { copy_bytes(dst, src, n); return dst; }

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    if (d < s) copy_bytes(d, s, n);
    else if (d > s) while (n--) d[n] = s[n];
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    uint8_t *d = (uint8_t *)dst; while (n--) *d++ = (uint8_t)value; return dst;
}

size_t strlen(const char *s)
{
    size_t n = 0; if (s) while (s[n]) ++n; return n;
}

char *strcpy(char *dst, const char *src)
{
    char *out = dst; while ((*dst++ = *src++) != '\0') {} return out;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n--) {
        unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca != cb || !ca || !cb) return (int)ca - (int)cb;
    }
    return 0;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
    while (n--) { if (*pa != *pb) return (int)*pa - (int)*pb; ++pa; ++pb; }
    return 0;
}

uint32_t esp_log_timestamp(void) { return 0; }

void esp_chip_info(esp_chip_info_t *out_info)
{
    if (!out_info) return;
    out_info->model = CHIP_ESP32S3;
    out_info->features = CHIP_FEATURE_WIFI_BGN | CHIP_FEATURE_BLE | CHIP_FEATURE_EMB_PSRAM;
    out_info->revision = 0;
    out_info->cores = 2;
}
