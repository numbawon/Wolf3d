// w3d/opl2.cpp — YM3812 (OPL2) chip, implementation.
//
// Every table is generated from the maths at first use (nothing transcribed),
// matching the documented behaviour of the part. See opl2.hpp for the model.
#include "w3d/opl2.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace w3d {
namespace {

// Frequency multiplier, doubled so 0.5 stays an integer.
const std::array<std::uint16_t, 16>& mult2() {
    static const std::array<std::uint16_t, 16> m{
        1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
    return m;
}

// Quarter sine in the log domain: logsin[i] = -256 * log2(sin(...)).
const std::array<std::uint16_t, 256>& logsin() {
    static const std::array<std::uint16_t, 256> t = [] {
        std::array<std::uint16_t, 256> a{};
        for (int i = 0; i < 256; ++i) {
            const double s = std::sin((i + 0.5) * std::numbers::pi / 512.0);
            a[i] = static_cast<std::uint16_t>(std::llround(-std::log(s) / std::log(2.0) * 256.0));
        }
        return a;
    }();
    return t;
}

// exp2 lookup: expTab[f] = 4096 * 2^(-f/256), so 4096 down to ~2048.
const std::array<std::uint16_t, 256>& expTab() {
    static const std::array<std::uint16_t, 256> t = [] {
        std::array<std::uint16_t, 256> a{};
        for (int i = 0; i < 256; ++i)
            a[i] = static_cast<std::uint16_t>(
                std::llround(4096.0 * std::pow(2.0, -i / 256.0)));
        return a;
    }();
    return t;
}

// Sustain level: 3 dB per step, and 15 means "all the way down".
const std::array<std::uint16_t, 16>& slTab() {
    static const std::array<std::uint16_t, 16> t = [] {
        std::array<std::uint16_t, 16> a{};
        for (int i = 0; i < 16; ++i)
            a[i] = (i == 15) ? 496 : static_cast<std::uint16_t>(i * 16);
        return a;
    }();
    return t;
}

// Key scale level: higher notes get quieter. Base attenuation per
// block / fnum-high.
const std::array<std::array<std::uint16_t, 16>, 8>& kslTab() {
    static const std::array<std::array<std::uint16_t, 16>, 8> t = [] {
        static const int rom[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56,
                                    58, 59, 60, 61, 62, 63, 64};
        std::array<std::array<std::uint16_t, 16>, 8> a{};
        for (int b = 0; b < 8; ++b)
            for (int f = 0; f < 16; ++f) {
                const int v = rom[f] - 8 * (7 - b);
                a[static_cast<std::size_t>(b)][static_cast<std::size_t>(f)] =
                    static_cast<std::uint16_t>(v < 0 ? 0 : v);
            }
        return a;
    }();
    return t;
}

// Key-scale shift by register value (0 = off: shift it fully away).
const std::array<int, 4>& kslShift() {
    static const std::array<int, 4> s{8, 2, 1, 0};
    return s;
}

// Envelope step patterns: the hardware's way of getting fractional rates out
// of an integer counter.
const std::array<std::array<std::uint8_t, 8>, 4>& egPattern() {
    static const std::array<std::array<std::uint8_t, 8>, 4> p{{
        {0, 1, 0, 1, 0, 1, 0, 1},
        {0, 1, 0, 1, 1, 1, 0, 1},
        {0, 1, 1, 1, 0, 1, 1, 1},
        {0, 1, 1, 1, 1, 1, 1, 1},
    }};
    return p;
}

// Tremolo: a 3.7 Hz triangle, 26 units deep (~4.8 dB) or a quarter of that.
const std::array<std::uint8_t, 210>& tremTab() {
    static const std::array<std::uint8_t, 210> t = [] {
        std::array<std::uint8_t, 210> a{};
        for (int i = 0; i < 105; ++i)
            a[static_cast<std::size_t>(i)] =
            a[static_cast<std::size_t>(209 - i)] = static_cast<std::uint8_t>(i >> 2);
        return a;
    }();
    return t;
}

// Vibrato: 6.1 Hz, +/- a fraction of the note.
const std::array<std::int8_t, 4>& vibTab() {
    static const std::array<std::int8_t, 4> v{0, 1, 0, -1};
    return v;
}

// Turn a total attenuation (units of 1/256 of an octave) into an amplitude.
std::int32_t attToAmp(std::int32_t total) {
    if (total >= 0x1FFF)
        return 0;
    return static_cast<std::int32_t>(
        (expTab()[static_cast<std::size_t>(total & 0xFF)] >> (total >> 8)));
}

}  // namespace

void Opl2::reset() {
    reg_.fill(0);
    for (auto& ch : ch_) {
        ch.fnum = 0;
        ch.block = 0;
        ch.kon = false;
        ch.fb = 0;
        ch.cnt = 0;
        for (auto& op : ch.ops) {
            op = Op{};
            op.env = 511;
            op.state = State::Off;
        }
    }
    egCounter_ = 0;
    tremPos_ = 0;
    tremolo_ = 0;
    vibPos_ = 0;
    dam_ = 0;
    dvb_ = 0;
    waveSelect_ = 0;
}

// The packed map: which operator of which channel each register offset
// (reg & 0x1F) addresses.
void Opl2::buildOpMap() {
    static const std::array<std::array<std::uint8_t, 2>, 9> kOpOff{
        {{0x00, 0x03}, {0x01, 0x04}, {0x02, 0x05},
         {0x08, 0x0B}, {0x09, 0x0C}, {0x0A, 0x0D},
         {0x10, 0x13}, {0x11, 0x14}, {0x12, 0x15}}};
    opCh_.fill(-1);
    opIdx_.fill(-1);
    for (std::size_t c = 0; c < 9; ++c)
        for (std::size_t o = 0; o < 2; ++o) {
            const auto off = kOpOff[c][o];
            opCh_[off] = static_cast<std::int8_t>(c);
            opIdx_[off] = static_cast<std::int8_t>(o);
        }
}

void Opl2::write(std::uint8_t reg, std::uint8_t val) {
    reg &= 0xFF;
    val &= 0xFF;
    reg_[reg] = val;
    const auto hi = reg & 0xF0;
    const auto lo = reg & 0x1F;

    if (reg == 0x01) {  // wave-select enable
        waveSelect_ = (val & 0x20) ? 1 : 0;
        return;
    }
    if (reg == 0xBD) {  // depth flags (rhythm channel ignored)
        dam_ = (val & 0x80) ? 1 : 0;
        dvb_ = (val & 0x40) ? 1 : 0;
        return;
    }

    if (hi == 0x20 || hi == 0x30) {  // KSR / multiplier
        if (opCh_[lo] < 0) return;
        Op& op = ch_[opCh_[lo]].ops[opIdx_[lo]];
        op.am = (val & 0x80) != 0;
        op.vib = (val & 0x40) != 0;
        op.egt = (val & 0x20) != 0;
        op.ksr = (val & 0x10) != 0;
        op.mult = val & 0x0F;
        retune(&ch_[opCh_[lo]]);
        return;
    }
    if (hi == 0x40 || hi == 0x50) {  // KSL / total level
        if (opCh_[lo] < 0) return;
        Op& op = ch_[opCh_[lo]].ops[opIdx_[lo]];
        op.kslBits = (val >> 6) & 3;
        op.tl = val & 0x3F;
        retune(&ch_[opCh_[lo]]);
        return;
    }
    if (hi == 0x60 || hi == 0x70) {  // attack / decay
        if (opCh_[lo] < 0) return;
        Op& op = ch_[opCh_[lo]].ops[opIdx_[lo]];
        op.ar = (val >> 4) & 0x0F;
        op.dr = val & 0x0F;
        return;
    }
    if (hi == 0x80 || hi == 0x90) {  // sustain / release
        if (opCh_[lo] < 0) return;
        Op& op = ch_[opCh_[lo]].ops[opIdx_[lo]];
        op.sl = (val >> 4) & 0x0F;
        op.rr = val & 0x0F;
        return;
    }
    if (hi == 0xE0 || hi == 0xF0) {  // waveform select
        if (opCh_[lo] < 0) return;
        Op& op = ch_[opCh_[lo]].ops[opIdx_[lo]];
        op.wave = val & 0x03;
        return;
    }
    if (hi == 0xA0) {  // FNUM low
        const auto c = reg & 0x0F;
        if (c > 8) return;
        Ch& ch = ch_[c];
        ch.fnum = (ch.fnum & 0x300) | val;
        retune(&ch);
        return;
    }
    if (hi == 0xB0) {  // FNUM high + block + key on/off
        const auto c = reg & 0x0F;
        if (c > 8) return;
        Ch& ch = ch_[c];
        ch.fnum = (ch.fnum & 0xFF) | (static_cast<std::uint32_t>(val & 0x03) << 8);
        ch.block = (val >> 2) & 0x07;
        const bool kon = (val & 0x20) != 0;
        if (kon && !ch.kon)
            keyOn(ch);
        else if (!kon && ch.kon)
            keyOff(ch);
        ch.kon = kon;
        retune(&ch);
        return;
    }
    if (hi == 0xC0) {  // feedback + connect
        const auto c = reg & 0x0F;
        if (c > 8) return;
        Ch& ch = ch_[c];
        ch.fb = (val >> 1) & 0x07;
        ch.cnt = val & 0x01;
        return;
    }
}

void Opl2::keyOn(Ch& ch) {
    for (auto& op : ch.ops) {
        op.state = State::Attack;
        op.phase = 0;  // key-on restarts the waveform
        op.out = op.prev = 0;
        if (effRate(ch, op, op.ar) >= 60)
            op.env = 0;  // instant attack
    }
}

void Opl2::keyOff(Ch& ch) {
    for (auto& op : ch.ops)
        if (op.state != State::Off)
            op.state = State::Release;
}

// Phase increment and key-scale attenuation both follow from
// block / fnum / multiplier.
void Opl2::retune(Ch* ch) {
    if (!ch) return;
    for (auto& op : ch->ops) {
        op.inc = ((ch->fnum << ch->block) * mult2()[op.mult]) >> 1;
        const auto base = kslTab()[ch->block][ch->fnum >> 6];
        op.ksl = static_cast<std::int32_t>(base) >> kslShift()[op.kslBits & 3];
    }
}

// Effective envelope rate: the register nibble, sped up for higher notes.
std::uint8_t Opl2::effRate(const Ch& ch, const Op& op, std::uint8_t r) const {
    if (r == 0) return 0;
    const auto ksrVal = static_cast<std::uint8_t>((ch.block << 1) | ((ch.fnum >> 9) & 1));
    const auto rof = op.ksr ? ksrVal : (ksrVal >> 2);
    const auto rate = static_cast<std::uint8_t>(r * 4 + rof);
    return rate > 63 ? 63 : rate;
}

void Opl2::advanceEnv(Ch& ch, Op& op) {
    std::uint8_t r;
    switch (op.state) {
        case State::Attack: r = op.ar; break;
        case State::Decay: r = op.dr; break;
        case State::Sustain:
            if (op.egt) return;  // non-percussive: hold
            r = op.rr;
            break;
        case State::Release: r = op.rr; break;
        default: return;
    }
    const auto rate = effRate(ch, op, r);
    if (rate == 0) return;

    const auto hi = rate >> 2;
    const auto lo = rate & 3;
    const auto shift = 13 - hi;
    std::int32_t step;
    if (shift > 0) {
        if (egCounter_ & ((1u << shift) - 1u)) return;  // not on this sample
        step = egPattern()[lo][(egCounter_ >> shift) & 7];
    } else {
        step = static_cast<std::int32_t>(egPattern()[lo][egCounter_ & 7]) << (hi - 13);
    }
    if (!step) return;

    if (op.state == State::Attack) {
        // Attenuation falls fastest while it is still large — the
        // characteristic OPL attack curve.
        op.env += ((~op.env) * step) >> 3;
        if (op.env <= 0) {
            op.env = 0;
            op.state = State::Decay;
        }
    } else {
        op.env += step;
        if (op.env >= 511) {
            op.env = 511;
            if (op.state == State::Release)
                op.state = State::Off;
        }
        if (op.state == State::Decay && op.env >= static_cast<std::int32_t>(slTab()[op.sl])) {
            op.env = static_cast<std::int32_t>(slTab()[op.sl]);
            op.state = State::Sustain;
        }
    }
}

bool Opl2::waveSample(const Op& op, std::uint32_t phaseIdx, std::uint32_t& idx, bool& neg) const {
    neg = false;
    const auto w = op.wave;
    if (w == 0) {
        neg = (phaseIdx & 0x200) != 0;
        idx = (phaseIdx & 0x100) ? (0xFFu - (phaseIdx & 0xFFu)) : (phaseIdx & 0xFFu);
    } else if (w == 1) {
        if (phaseIdx & 0x200) return false;  // bottom half removed
        idx = (phaseIdx & 0x100) ? (0xFFu - (phaseIdx & 0xFFu)) : (phaseIdx & 0xFFu);
    } else if (w == 2) {
        idx = (phaseIdx & 0x100) ? (0xFFu - (phaseIdx & 0xFFu)) : (phaseIdx & 0xFFu);
    } else {  // w == 3
        if (phaseIdx & 0x100) return false;  // every other quarter removed
        idx = phaseIdx & 0xFFu;
    }
    return true;
}

// One operator sample. `mod` is added to the phase (FM) — the whole trick.
std::int32_t Opl2::opSample(Ch& ch, Op& op, std::int32_t mod) {
    if (op.state == State::Off) {
        op.prev = op.out;
        op.out = 0;
        return 0;
    }

    std::uint32_t inc = op.inc;
    if (op.vib) {  // gentle pitch wobble
        const auto d = static_cast<std::int32_t>(static_cast<std::int8_t>(
            (ch.fnum >> 7) & 7)) * vibTab()[((vibPos_ >> 10) & 3)];
        std::int32_t dd = d;
        if (!dvb_) dd >>= 1;
        inc += static_cast<std::uint32_t>((dd << ch.block) * mult2()[op.mult] >> 1);
    }
    op.phase = op.phase + inc;  // wraps modulo 2^32

    const auto phaseIdx = ((op.phase >> 10) + static_cast<std::uint32_t>(mod)) & 0x3FFu;
    std::uint32_t idx = 0;
    bool neg = false;
    if (!waveSample(op, phaseIdx, idx, neg)) {
        op.prev = op.out;
        op.out = 0;
        return 0;
    }

    std::int32_t att = op.env + (static_cast<std::int32_t>(op.tl) << 2) + op.ksl;
    if (op.am)
        att += dam_ ? tremolo_ : (tremolo_ >> 2);

    std::int32_t amp = attToAmp(static_cast<std::int32_t>(logsin()[idx]) + (att << 3));
    if (neg) amp = -amp;

    op.prev = op.out;
    op.out = amp;
    return amp;
}

float Opl2::sample() {
    // LFOs
    tremPos_ = (tremPos_ + 1) % (210u * 64u);
    tremolo_ = static_cast<std::int32_t>(tremTab()[tremPos_ / 64u]);
    vibPos_ = (vibPos_ + 1) & 0xFFFFu;

    std::int32_t acc = 0;
    for (auto& ch : ch_) {
        Op& op1 = ch.ops[0];
        Op& op2 = ch.ops[1];
        if (op1.state == State::Off && op2.state == State::Off)
            continue;

        advanceEnv(ch, op1);
        advanceEnv(ch, op2);

        // Operator 1 can feed itself: the average of its last two outputs,
        // scaled by the feedback setting.
        std::int32_t fbMod = 0;
        if (ch.fb)
            fbMod = (op1.out + op1.prev) >> (9 - ch.fb);
        const auto o1 = opSample(ch, op1, fbMod);

        if (ch.cnt) {
            acc += o1 + opSample(ch, op2, 0);  // additive: both are heard
        } else {
            acc += opSample(ch, op2, o1 >> 1);  // FM: op1 bends op2's phase
        }
    }
    ++egCounter_;
    // Nine carriers at full tilt can sum to about three times full scale. The
    // real part saturates rather than wraps, so clamp.
    const auto v = static_cast<float>(acc) / 12000.0f;
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

}  // namespace w3d
