#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace sniffer {
constexpr size_t max_payload = 255, max_message = 2048, max_profiles = 4, max_packets = 32;
constexpr uint32_t protocol_version = 1;
enum class Mode : uint8_t { Fixed, Scan, Cad };
enum class Crc : uint8_t { Ok, Disabled, Unavailable };
enum class Protocol : uint8_t { Unknown, Reticulum, Meshtastic, MeshCore };
enum class Confidence : uint8_t { Unknown, Probable, Confirmed };
struct RadioProfile {
    char name[24] = "Custom 867.5";
    uint32_t frequency_hz = 867500000, bw_hz = 125000;
    uint16_t preamble = 8;
    uint8_t sf = 8, cr = 5, sync_word = 0x12, crc_enabled = 1;
};
struct RadioConfig {
    Mode mode = Mode::Fixed;
    uint8_t profile_count = 1;
    uint32_t dwell_ms = 3000;
    std::array<RadioProfile, max_profiles> profiles{};
};
struct ClientSettings {
    char host[96] = "lora-sniffer.local", token[96] = "";
    uint16_t port = 8080, history_size = 24;
    uint32_t timeout_ms = 8000, refresh_ms = 500;
    uint8_t enabled = 0, apply_on_connect = 0;
    RadioConfig radio{};
};
struct Classification {
    Protocol protocol = Protocol::Unknown;
    Confidence confidence = Confidence::Unknown;
};
class PacketClassifier {
public:
    virtual ~PacketClassifier() = default;
    virtual Classification classify(const uint8_t*, size_t, const RadioProfile&) const = 0;
};
Classification classify(const uint8_t* payload, size_t size, const RadioProfile& profile);
const char* protocol_name(Protocol protocol);
const char* mode_name(Mode mode);
bool valid(const RadioProfile& profile);
bool valid(const RadioConfig& config);
bool valid(const ClientSettings& settings);
bool safe_token(const char* token, size_t capacity, bool required);
bool bearer_matches(const char* header, size_t length, const char* token, size_t capacity);
// Physical USB provisioning only; never accepted by the WebSocket command parser.
struct Provision {
    uint32_t version=1;
    char ssid[33]="", password[65]="", token[96]="";
};
bool valid(const Provision& provision);
bool decode_provision(const char* json, size_t length, Provision& provision);
// Physical dashboard USB only. Updates link fields atomically, preserving all
// radio profiles, refresh/history and other saved client preferences.
bool decode_link_setup(const char* json, size_t length, ClientSettings& settings);
bool is_link_setup_probe(const char* json, size_t length);
class SetupLine {
public:
    enum class Result { More, Complete, Discarded };
    Result feed(uint8_t byte);
    const char* data() const { return buffer_.data(); }
    size_t size() const { return size_; }
    void clear();
private:
    std::array<char,512> buffer_{};
    size_t used_=0,size_=0;
    bool discard_=false;
};

struct Packet {
    uint64_t timestamp_ms = 0, sequence = 0;
    RadioProfile profile{};
    float rssi_dbm = NAN, snr_db = NAN;
    uint16_t length = 0;
    Crc crc = Crc::Unavailable;
    Classification classification{};
    std::array<uint8_t, max_payload> payload{};
};
enum class EventType : uint8_t { Packet, Status, RadioConfig, Statistics, Error, ScanStatus, Cad };
struct Event {
    EventType type = EventType::Status;
    char boot_id[17] = "", message[80] = "";
    Packet packet{};
    Mode mode = Mode::Fixed;
    uint8_t active_profile = 0;
    bool radio_ready = false, cad_detected = false;
    uint64_t rx_packets = 0, crc_errors = 0, queue_drops = 0, send_drops = 0;
    uint64_t cad_checks = 0, cad_detections = 0;
};
// Strict, size/depth/duplicate bounded schema-1 JSON. No null numeric substitutions.
bool decode(const char* json, size_t length, Event& event);
bool decode_config(const char* json, size_t length, RadioConfig& config);
size_t encode(const Event& event, char* output, size_t capacity);
size_t encode_config(const RadioConfig& config, char* output, size_t capacity);

// Reassembles library buffer chunks and RFC6455 continuation frames, including
// interleaved ping/pong. Rejects oversize, missing offsets and unexpected binary.
class MessageAssembler {
public:
    bool feed(uint8_t opcode, bool fin, size_t frame_length, size_t offset,
              const char* data, size_t length);
    const char* data() const { return buffer_.data(); }
    size_t size() const { return used_; }
    bool rejected() const { return rejected_; }
    void reset();
private:
    std::array<char, max_message + 1> buffer_{};
    size_t used_ = 0, frame_used_ = 0, expected_frame_ = 0;
    bool active_ = false, complete_ = false, frame_open_ = false, rejected_ = false;
    uint8_t frame_opcode_ = 0;
    bool frame_fin_ = false;
};

class State {
public:
    bool connected = false, enabled = false, radio_ready = false;
    char diagnostic[80] = "Sniffer disabled", boot_id[17] = "";
    Mode mode = Mode::Fixed;
    RadioProfile active{};
    uint8_t active_index = 0;
    uint64_t last_message_ms = 0, last_packet_ms = 0, revision = 0;
    uint64_t rx_packets = 0, crc_errors = 0, queue_drops = 0, send_drops = 0;
    uint64_t cad_checks = 0, cad_detections = 0, local_drops = 0, malformed = 0;
    float last_rssi = NAN, last_snr = NAN;
    bool apply(const Event& event, uint64_t received_ms, size_t capacity);
    const Packet* recent(size_t index) const;
    void activity(uint64_t now, uint32_t seconds, uint16_t* packets, uint16_t* cad, size_t count) const;
    float observed_rate(uint64_t now) const;
    size_t count() const { return count_; }
private:
    struct Bin { uint64_t second_plus_one = 0; uint16_t packets = 0, cad = 0; };
    std::array<Bin, 900> bins_{};
    std::array<Packet, max_packets> packets_{};
    size_t head_ = 0, count_ = 0;
    uint64_t last_sequence_ = 0;
};
} // namespace sniffer
