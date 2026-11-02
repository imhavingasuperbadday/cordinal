// Apple property list reader for the pure C++ libgpod rewrite.
//
// SysInfoExtended, iTunes metadata and iPhone manifests are property lists.
// The original libgpod used libxml2 plus GLib's GValue container hierarchy;
// this version parses both plist dialects itself and hands back an immutable
// tree of Value nodes, so no external library is needed.
//
// Supported input:
//   * XML plists (<plist><dict>...</dict></plist>), including comments,
//     processing instructions, CDATA sections and character references
//   * binary plists ("bplist00"), which SysInfo files on newer devices use
//
// Parsed values are immutable; copies share the same underlying node.

#ifndef GPOD_PLIST_HPP
#define GPOD_PLIST_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gpod::plist {

using Bytes = std::vector<std::uint8_t>;

class Value;

// Value is a lightweight handle.  The constructors below are what the parsers
// use; the accessors convert with an explicit fallback instead of throwing so
// that "key missing" and "wrong type" both read naturally by default.
class Value {
public:
    enum class Type : std::uint8_t {
        Null,
        Boolean,
        Integer,
        Real,
        String,
        Data,
        Array,
        Dictionary,
    };

    Value() noexcept;
    explicit Value(bool value);
    explicit Value(std::int64_t value);
    explicit Value(double value);
    explicit Value(std::string value);
    explicit Value(std::string_view value);
    explicit Value(plist::Bytes value);
    explicit Value(std::vector<Value> value);
    explicit Value(std::map<std::string, Value, std::less<>> value);

    Value(const Value& other) noexcept;
    Value(Value&& other) noexcept;
    Value& operator=(const Value& other) noexcept;
    Value& operator=(Value&& other) noexcept;
    ~Value();

    [[nodiscard]] Type type() const noexcept;
    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_boolean() const noexcept;
    [[nodiscard]] bool is_integer() const noexcept;
    [[nodiscard]] bool is_real() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_data() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_dictionary() const noexcept;

    [[nodiscard]] bool boolean(bool fallback = false) const noexcept;
    [[nodiscard]] std::int64_t integer(std::int64_t fallback = 0) const noexcept;
    [[nodiscard]] double real(double fallback = 0.0) const noexcept;
    [[nodiscard]] std::string_view text(std::string_view fallback = {}) const noexcept;
    [[nodiscard]] const plist::Bytes& data() const noexcept;
    [[nodiscard]] const std::vector<Value>& array() const noexcept;
    [[nodiscard]] const std::map<std::string, Value, std::less<>>& dictionary() const noexcept;

    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    [[nodiscard]] std::string_view text_of(std::string_view key,
                                          std::string_view fallback = {}) const noexcept;
    [[nodiscard]] std::int64_t integer_of(std::string_view key,
                                          std::int64_t fallback = 0) const noexcept;
    [[nodiscard]] bool boolean_of(std::string_view key, bool fallback = false) const noexcept;

    // Number of elements for arrays and dictionaries, 0 for scalar values.
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    struct Node;
    std::shared_ptr<Node> node_;
};

[[nodiscard]] std::string_view to_string(Value::Type type) noexcept;

// Parse an XML plist document.  Throws gpod::Error(ErrorCode::Corrupt) when
// the document is not well-formed or is not a plist.
[[nodiscard]] Value parse_xml(std::string_view xml);

// Parse a "bplist00" binary plist.
[[nodiscard]] Value parse_binary(std::span<const std::uint8_t> data);

// Parse either dialect, choosing from the "bplist00" signature.
[[nodiscard]] Value parse(std::span<const std::uint8_t> data);

[[nodiscard]] Value parse_file(const std::filesystem::path& path);

// Non-throwing parse used when reading device files that may be absent or
// damaged.  Returns false when the file cannot be read or does not parse.
[[nodiscard]] bool try_parse_file(const std::filesystem::path& path, Value& out) noexcept;

}  // namespace gpod::plist

#endif  // GPOD_PLIST_HPP
