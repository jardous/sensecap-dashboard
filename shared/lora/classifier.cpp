#include "protocol.hpp"
#include <algorithm>
namespace sniffer {
namespace {
bool nonzero(const uint8_t* p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i]) return true; return false; }
class Meshtastic final : public PacketClassifier {
public:
    Classification classify(const uint8_t* p, size_t n, const RadioProfile& r) const override {
        // Documented 16-byte radio header; channel hash/hop fields are not proof.
        if (r.sync_word == 0x2B && n >= 17 && nonzero(p + 4, 4) && nonzero(p + 8, 4) &&
            !(p[4] == 255 && p[5] == 255 && p[6] == 255 && p[7] == 255))
            return {Protocol::Meshtastic, Confidence::Probable};
        return {};
    }
};
class Reticulum final : public PacketClassifier {
public:
    Classification classify(const uint8_t* p, size_t n, const RadioProfile& r) const override {
        // Unauthenticated H1 announce: header/hops, destination16, context,
        // public key64, name hash10, random hash10, signature64, optional app data.
        // Deliberately do not classify generic DATA or IFAC-protected packets.
        if (r.sync_word == 0x12 && n >= 167 && (p[0] & 0xCF) == 0x01 && p[1] <= 128 &&
            p[18] == 0 && nonzero(p + 2, 16) && nonzero(p + 19, 64))
            return {Protocol::Reticulum, Confidence::Probable};
        return {};
    }
};
class MeshCore final : public PacketClassifier {
public:
    Classification classify(const uint8_t* p, size_t n, const RadioProfile&) const override {
        // Documented payload version0 ADVERT, with optional transport codes and
        // encoded path metadata (hop count * hash size), no signature verification.
        if (n < 103 || (p[0] >> 6)!=0 || ((p[0] >> 2)&15)!=4) return {};
        const size_t at=((p[0]&3)==0 || (p[0]&3)==3)?5:1;
        const size_t size_code=p[at]>>6;
        const size_t path_bytes=(p[at]&63)*(size_code+1);
        const size_t payload_at=at+1+path_bytes;
        if (size_code<3 && path_bytes<=64 && n>=payload_at+101 && n-payload_at<=184 && nonzero(p+payload_at,32))
            return {Protocol::MeshCore, Confidence::Probable};
        return {};
    }
};
}
Classification classify(const uint8_t* p, size_t n, const RadioProfile& r) {
    if (!p || !n || n > max_payload) return {};
    static const Meshtastic mt; static const Reticulum rns; static const MeshCore mc;
    const PacketClassifier* parsers[] = {&mt, &rns, &mc};
    Classification result; unsigned matches = 0;
    for (const auto* parser : parsers) {
        const auto candidate = parser->classify(p, n, r);
        if (candidate.protocol != Protocol::Unknown) { result = candidate; ++matches; }
    }
    return matches == 1 ? result : Classification{}; // ambiguity is Unknown
}
const char* protocol_name(Protocol p) {
    switch (p) { case Protocol::Reticulum: return "RNS?"; case Protocol::Meshtastic: return "Meshtastic?";
        case Protocol::MeshCore: return "MeshCore?"; default: return "Unknown"; }
}
const char* mode_name(Mode m) { return m == Mode::Scan ? "scan" : m == Mode::Cad ? "cad" : "fixed"; }
} // namespace sniffer
