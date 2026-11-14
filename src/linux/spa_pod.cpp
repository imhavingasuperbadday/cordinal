// SPA POD building and bounds-checked reading (spa_pod.h).
#include "spa_pod.h"

#include <algorithm>
#include <cstring>

namespace lx::spa {

namespace {

constexpr size_t Round8(size_t n) { return (n + 7) & ~(size_t)7; }

uint32_t Load32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

}  // namespace

// ---- Builder

void Builder::Header(uint32_t size, uint32_t type) {
    uint32_t h[2] = {size, type};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(h);
    bytes.insert(bytes.end(), p, p + 8);
}

void Builder::Pad() { bytes.resize(Round8(bytes.size()), 0); }

void Builder::Scalar(uint32_t type, const void* body, uint32_t size) {
    Header(size, type);
    if (size) {
        const uint8_t* p = static_cast<const uint8_t*>(body);
        bytes.insert(bytes.end(), p, p + size);
    }
    Pad();
}

void Builder::Str(std::string_view s) {
    Header((uint32_t)s.size() + 1, Type::String);
    bytes.insert(bytes.end(), s.begin(), s.end());
    bytes.push_back(0);
    Pad();
}

void Builder::Raw(const uint8_t* pod, size_t size) {
    bytes.insert(bytes.end(), pod, pod + size);
    Pad();
}

void Builder::ChoiceInt(ChoiceType type, std::initializer_list<int32_t> values) {
    Header(16 + 4 * (uint32_t)values.size(), Type::Choice);
    uint32_t head[4] = {type, 0, 4, Type::Int};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(head);
    bytes.insert(bytes.end(), p, p + 16);
    for (int32_t v : values) {
        p = reinterpret_cast<const uint8_t*>(&v);
        bytes.insert(bytes.end(), p, p + 4);
    }
    Pad();
}

void Builder::ChoiceId(ChoiceType type, std::initializer_list<uint32_t> values) {
    Header(16 + 4 * (uint32_t)values.size(), Type::Choice);
    uint32_t head[4] = {type, 0, 4, Type::Id};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(head);
    bytes.insert(bytes.end(), p, p + 16);
    for (uint32_t v : values) {
        p = reinterpret_cast<const uint8_t*>(&v);
        bytes.insert(bytes.end(), p, p + 4);
    }
    Pad();
}

void Builder::BeginStruct() {
    open_.push_back(bytes.size());
    Header(0, Type::Struct);
}

void Builder::BeginObject(uint32_t type, uint32_t id) {
    open_.push_back(bytes.size());
    Header(0, Type::Object);
    uint32_t body[2] = {type, id};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(body);
    bytes.insert(bytes.end(), p, p + 8);
}

void Builder::Prop(uint32_t key, uint32_t flags) {
    uint32_t body[2] = {key, flags};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(body);
    bytes.insert(bytes.end(), p, p + 8);
}

void Builder::End() {
    if (open_.empty()) return;
    size_t at = open_.back();
    open_.pop_back();
    uint32_t size = (uint32_t)(bytes.size() - at - 8);
    memcpy(bytes.data() + at, &size, 4);
}

void Builder::Dict(const std::vector<std::pair<std::string, std::string>>& items) {
    BeginStruct();
    Int((int32_t)items.size());
    for (const auto& [k, v] : items) {
        Str(k);
        Str(v);
    }
    End();
}

// ---- Reader

size_t PodSize(const uint8_t* data, size_t size) {
    if (size < 8) return 0;
    size_t body = Load32(data);
    if (body > size - 8) return 0;
    // The last POD of a message may come without its padding.
    return std::min(size, 8 + Round8(body));
}

Value Reader::Next() {
    if (!ok_ || p_ >= end_) {
        ok_ = false;
        return {};
    }
    size_t left = (size_t)(end_ - p_);
    size_t total = PodSize(p_, left);
    if (!total) {
        ok_ = false;
        return {};
    }
    Value v{Load32(p_ + 4), Load32(p_), p_ + 8};
    if (v.type == 0) {
        ok_ = false;
        return {};
    }
    p_ += total;
    return v;
}

uint32_t Reader::PeekType() const {
    if (!ok_ || end_ - p_ < 8) return 0;
    return Load32(p_ + 4);
}

bool Reader::Scalar(uint32_t type, void* out, uint32_t size) {
    Value v = Next();
    if (!ok_ || v.type != type || v.size < size) {
        ok_ = false;
        memset(out, 0, size);
        return false;
    }
    memcpy(out, v.body, size);
    return true;
}

int32_t Reader::Int() {
    int32_t v;
    Scalar(Type::Int, &v, 4);
    return v;
}

uint32_t Reader::IdV() {
    uint32_t v;
    Scalar(Type::Id, &v, 4);
    return v;
}

bool Reader::BoolV() {
    int32_t v;
    Scalar(Type::Bool, &v, 4);
    return v != 0;
}

int64_t Reader::LongV() {
    int64_t v;
    Scalar(Type::Long, &v, 8);
    return v;
}

float Reader::FloatV() {
    float v;
    Scalar(Type::Float, &v, 4);
    return v;
}

double Reader::DoubleV() {
    double v;
    Scalar(Type::Double, &v, 8);
    return v;
}

int64_t Reader::FdV() {
    int64_t v;
    Scalar(Type::Fd, &v, 8);
    return v;
}

bool Reader::StrOrNull(std::string& out) {
    out.clear();
    Value v = Next();
    if (!ok_) return false;
    if (v.type == Type::None) return false;
    // A String's body ends in a NUL inside its size.
    if (v.type != Type::String || v.size == 0 || v.body[v.size - 1] != 0) {
        ok_ = false;
        return false;
    }
    out.assign(reinterpret_cast<const char*>(v.body), strnlen(reinterpret_cast<const char*>(v.body), v.size));
    return true;
}

std::string Reader::Str() {
    std::string s;
    StrOrNull(s);
    return s;
}

Reader Reader::Struct() {
    Value v = Next();
    if (!ok_ || v.type != Type::Struct) {
        ok_ = false;
        Reader r;
        r.ok_ = false;
        return r;
    }
    return Reader(v);
}

Value Reader::ObjectV() {
    Value v = Next();
    if (!ok_) return {};
    if (v.type == Type::None) return {};
    if (v.type != Type::Object || v.size < 8) {
        ok_ = false;
        return {};
    }
    return v;
}

void Reader::Dict(std::vector<std::pair<std::string, std::string>>& items) {
    items.clear();
    Reader d = Struct();
    int32_t n = d.Int();
    // Each item is at least two 16-byte PODs: a count beyond what fits is a lie.
    if (!d.Ok() || n < 0 || (size_t)n > (size_t)(d.end_ - d.p_) / 32) {
        ok_ = false;
        return;
    }
    items.reserve((size_t)n);
    for (int32_t i = 0; i < n; ++i) {
        std::string k, v;
        d.StrOrNull(k);
        d.StrOrNull(v);
        if (!d.Ok()) {
            ok_ = false;
            items.clear();
            return;
        }
        items.emplace_back(std::move(k), std::move(v));
    }
}

// ---- Objects

ObjectView ParseObject(const Value& v) {
    ObjectView o;
    if (v.type != Type::Object || v.size < 8) return o;
    o.type = Load32(v.body);
    o.id = Load32(v.body + 4);
    o.props = v.body + 8;
    o.propsSize = v.size - 8;
    o.ok = true;
    return o;
}

Value FindProp(const ObjectView& o, uint32_t key) {
    if (!o.ok) return {};
    const uint8_t* p = o.props;
    const uint8_t* end = o.props + o.propsSize;
    while (end - p >= 16) {
        uint32_t k = Load32(p);
        size_t total = PodSize(p + 8, (size_t)(end - p - 8));
        if (!total) return {};
        if (k == key) return Value{Load32(p + 12), Load32(p + 8), p + 16};
        p += 8 + total;
    }
    return {};
}

namespace {

// A plain value of `type` and `size`, or the first (default) value of a Choice of it.
bool Single(const Value& v, uint32_t type, uint32_t size, void* out) {
    if (v.type == type && v.size >= size) {
        memcpy(out, v.body, size);
        return true;
    }
    if (v.type == Type::Choice && v.size >= 16 + size) {
        uint32_t childSize = Load32(v.body + 8), childType = Load32(v.body + 12);
        if (childType != type || childSize != size) return false;
        memcpy(out, v.body + 16, size);
        return true;
    }
    return false;
}

}  // namespace

bool PropInt(const ObjectView& o, uint32_t key, int32_t& out) { return Single(FindProp(o, key), Type::Int, 4, &out); }
bool PropId(const ObjectView& o, uint32_t key, uint32_t& out) { return Single(FindProp(o, key), Type::Id, 4, &out); }

}  // namespace lx::spa
