// SPA POD values, the self-describing binary format PipeWire's native protocol carries: a builder
// that appends them to a byte vector and a bounds-checked reader. Every POD is a 32-bit body size
// and a 32-bit type followed by the body, padded to 8 bytes. Struct and Object nest PODs; an
// Object's properties are a key, flags and a POD each; a Choice is a choice type, flags, one child
// header and the packed child values. The reader never reads outside the bytes it was given and
// reports a malformed or mistyped value as a failure instead (pw_pod_fuzz in core_test feeds it
// arbitrary bytes).
#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lx::spa {

enum Type : uint32_t {
    None = 1, Bool, Id, Int, Long, Float, Double, String, Bytes, Rectangle, Fraction, Bitmap, Array,
    Struct, Object, Sequence, Pointer, Fd, Choice, Pod,
};
enum ChoiceType : uint32_t { ChoiceNone = 0, ChoiceRange, ChoiceStep, ChoiceEnum, ChoiceFlags };

// Appends PODs. Begin/End pairs nest (End patches the size of the innermost open container).
class Builder {
public:
    std::vector<uint8_t> bytes;

    void Int(int32_t v) { Scalar(Type::Int, &v, 4); }
    void Uint(uint32_t v) { Scalar(Type::Int, &v, 4); }
    void IdV(uint32_t v) { Scalar(Type::Id, &v, 4); }
    void BoolV(bool b) { int32_t v = b; Scalar(Type::Bool, &v, 4); }
    void LongV(int64_t v) { Scalar(Type::Long, &v, 8); }
    void FloatV(float v) { Scalar(Type::Float, &v, 4); }
    void FdV(int64_t index) { Scalar(Type::Fd, &index, 8); }  // an index into the message's fds
    void NoneV() { Scalar(Type::None, nullptr, 0); }
    void Str(std::string_view s);
    void FractionV(uint32_t num, uint32_t denom) { uint32_t v[2] = {num, denom}; Scalar(Type::Fraction, v, 8); }
    void Raw(const uint8_t* pod, size_t size);  // an already encoded POD, copied as is
    // A Choice of Int values: Range is {default, min, max}, Step {default, min, max, step}.
    void ChoiceInt(ChoiceType type, std::initializer_list<int32_t> values);
    void ChoiceId(ChoiceType type, std::initializer_list<uint32_t> values);

    void BeginStruct();
    void BeginObject(uint32_t type, uint32_t id);
    void Prop(uint32_t key, uint32_t flags = 0);  // the next POD is this property's value
    void End();
    // A dict as PipeWire sends one: Struct{Int n, (String key, String value)*}.
    void Dict(const std::vector<std::pair<std::string, std::string>>& items);

private:
    void Header(uint32_t size, uint32_t type);
    void Scalar(uint32_t type, const void* body, uint32_t size);
    void Pad();
    std::vector<size_t> open_;  // offsets of the open containers' headers
};

// A view of one POD inside a byte range. Default-constructed: absent (type 0).
struct Value {
    uint32_t type = 0, size = 0;
    const uint8_t* body = nullptr;
    bool Valid() const { return type != 0; }
};

// Reads PODs one after another from a byte range (a message, or a Struct's body). A read of the
// wrong type, or past the end, fails and every later read fails too: check Ok() once at the end.
class Reader {
public:
    Reader() = default;
    Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    explicit Reader(const Value& v) : p_(v.body), end_(v.body + v.size) {}

    bool Ok() const { return ok_; }
    bool AtEnd() const { return p_ >= end_; }
    void Fail() { ok_ = false; }

    Value Next();                 // any POD
    uint32_t PeekType() const;    // 0 at the end
    int32_t Int();
    uint32_t Uint() { return (uint32_t)Int(); }
    uint32_t IdV();
    bool BoolV();
    int64_t LongV();
    float FloatV();
    double DoubleV();
    int64_t FdV();
    std::string Str();            // a String, or "" for a None (PipeWire sends None for a null string)
    bool StrOrNull(std::string& out);  // false for None
    Reader Struct();              // the next Struct's body
    Value ObjectV();              // the next Object (or None: an absent object)
    void Dict(std::vector<std::pair<std::string, std::string>>& items);  // Struct{Int n, (k, v)*}

private:
    bool Scalar(uint32_t type, void* out, uint32_t size);
    const uint8_t* p_ = nullptr;
    const uint8_t* end_ = nullptr;
    bool ok_ = true;
};

// An Object's type, id and properties.
struct ObjectView {
    uint32_t type = 0, id = 0;
    const uint8_t* props = nullptr;
    size_t propsSize = 0;
    bool ok = false;
};
ObjectView ParseObject(const Value& v);
// The value of property `key`, absent when the object has none (or is malformed).
Value FindProp(const ObjectView& o, uint32_t key);
// A property's single value: a plain POD of `type`, or a Choice's default of that type.
bool PropInt(const ObjectView& o, uint32_t key, int32_t& out);
bool PropId(const ObjectView& o, uint32_t key, uint32_t& out);

// Total encoded size of the POD at `data` (header + padded body), 0 when it doesn't fit in `size`.
size_t PodSize(const uint8_t* data, size_t size);

}  // namespace lx::spa
