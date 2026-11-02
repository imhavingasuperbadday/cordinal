#include "gpod/plist.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include "gpod/error.hpp"

namespace gpod::plist {
namespace {

[[noreturn]] void parse_failure(std::string message) {
    fail(ErrorCode::Corrupt, "plist: " + std::move(message));
}

[[nodiscard]] bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// ---------------------------------------------------------------------------
// XML documents
// ---------------------------------------------------------------------------

// The plist format only needs elements, their text and nesting, so the parser
// below builds exactly that and drops attributes, comments and declarations.
struct XmlElement {
    std::string name;
    std::string text;
    std::vector<XmlElement> children;
};

constexpr int kMaxElementDepth = 64;

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

void append_character_reference(std::string& out, std::string_view reference) {
    const bool hexadecimal = reference.size() > 1 && (reference[1] == 'x' || reference[1] == 'X');
    const unsigned base = hexadecimal ? 16u : 10u;
    const std::string_view digits = reference.substr(hexadecimal ? 2 : 1);
    if (digits.empty()) {
        parse_failure("empty character reference");
    }
    std::uint32_t code_point = 0;
    for (const char c : digits) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (hexadecimal && c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(10 + (c - 'a'));
        } else if (hexadecimal && c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(10 + (c - 'A'));
        } else {
            parse_failure("invalid character reference");
        }
        code_point = code_point * base + digit;
        if (code_point > 0x10FFFF) {
            parse_failure("character reference out of range");
        }
    }
    append_utf8(out, code_point);
}

void append_decoded(std::string& out, std::string_view raw) {
    std::size_t position = 0;
    while (position < raw.size()) {
        const std::size_t ampersand = raw.find('&', position);
        if (ampersand == std::string_view::npos) {
            out.append(raw.substr(position));
            return;
        }
        out.append(raw.substr(position, ampersand - position));
        const std::size_t semicolon = raw.find(';', ampersand + 1);
        if (semicolon == std::string_view::npos) {
            out.append(raw.substr(ampersand));
            return;
        }
        const std::string_view entity = raw.substr(ampersand + 1, semicolon - ampersand - 1);
        if (entity == "amp") {
            out.push_back('&');
        } else if (entity == "lt") {
            out.push_back('<');
        } else if (entity == "gt") {
            out.push_back('>');
        } else if (entity == "quot") {
            out.push_back('"');
        } else if (entity == "apos") {
            out.push_back('\'');
        } else if (!entity.empty() && entity.front() == '#') {
            append_character_reference(out, entity);
        } else {
            // Unknown named entities are kept verbatim instead of failing, so a
            // stray "&" in device-written files does not reject the whole file.
            out.push_back('&');
            out.append(entity);
            out.push_back(';');
        }
        position = semicolon + 1;
    }
}

class XmlParser {
public:
    explicit XmlParser(std::string_view input) : input_(input) {}

    XmlElement parse_document() {
        skip_byte_order_mark();
        skip_misc();
        if (pos_ >= input_.size() || input_[pos_] != '<') {
            parse_failure("no root element");
        }
        XmlElement root = parse_element(0);
        skip_misc();
        if (pos_ < input_.size()) {
            parse_failure("trailing data after the root element");
        }
        return root;
    }

private:
    [[nodiscard]] bool starts_with(std::string_view token) const {
        return pos_ <= input_.size() && token.size() <= input_.size() - pos_ &&
               input_.substr(pos_, token.size()) == token;
    }

    void skip_byte_order_mark() {
        if (input_.size() >= 3 && static_cast<unsigned char>(input_[0]) == 0xEF &&
            static_cast<unsigned char>(input_[1]) == 0xBB &&
            static_cast<unsigned char>(input_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    void skip_whitespace() {
        while (pos_ < input_.size() && is_space(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
    }

    void skip_past(std::string_view terminator, const char* message) {
        const std::size_t found = input_.find(terminator, pos_);
        if (found == std::string_view::npos) {
            parse_failure(message);
        }
        pos_ = found + terminator.size();
    }

    // Comments, processing instructions and the document type declaration may
    // appear anywhere between elements; none of them carries plist data.
    void skip_misc() {
        for (;;) {
            skip_whitespace();
            if (starts_with("<!--")) {
                skip_past("-->", "unterminated comment");
            } else if (starts_with("<?")) {
                skip_past("?>", "unterminated processing instruction");
            } else if (starts_with("<!")) {
                skip_declaration();
            } else {
                return;
            }
        }
    }

    void skip_declaration() {
        pos_ += 2;
        int brackets = 0;
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '[') {
                ++brackets;
            } else if (c == ']') {
                if (brackets > 0) {
                    --brackets;
                }
            } else if (c == '>' && brackets == 0) {
                return;
            }
        }
        parse_failure("unterminated declaration");
    }

    [[nodiscard]] std::string_view parse_name() {
        const std::size_t begin = pos_;
        if (pos_ >= input_.size()) {
            parse_failure("expected an element name");
        }
        const unsigned char first = static_cast<unsigned char>(input_[pos_]);
        if (std::isalpha(first) == 0 && first != '_' && first != ':') {
            parse_failure("expected an element name");
        }
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_]);
            if (std::isalnum(c) == 0 && c != '_' && c != ':' && c != '-' && c != '.') {
                break;
            }
            ++pos_;
        }
        return input_.substr(begin, pos_ - begin);
    }

    void skip_attribute_value() {
        if (pos_ >= input_.size()) {
            parse_failure("unexpected end of document");
        }
        const char quote = input_[pos_];
        if (quote != '"' && quote != '\'') {
            parse_failure("unquoted attribute value");
        }
        ++pos_;
        const std::size_t end = input_.find(quote, pos_);
        if (end == std::string_view::npos) {
            parse_failure("unterminated attribute value");
        }
        pos_ = end + 1;
    }

    XmlElement parse_element(int depth) {
        if (depth > kMaxElementDepth) {
            parse_failure("elements nested too deeply");
        }
        if (pos_ >= input_.size() || input_[pos_] != '<') {
            parse_failure("expected '<'");
        }
        ++pos_;
        XmlElement element;
        element.name = std::string(parse_name());

        for (;;) {
            skip_whitespace();
            if (pos_ >= input_.size()) {
                parse_failure("unterminated start tag <" + element.name + ">");
            }
            if (input_[pos_] == '>') {
                ++pos_;
                break;
            }
            if (input_[pos_] == '/') {
                if (!starts_with("/>")) {
                    parse_failure("malformed empty-element tag");
                }
                pos_ += 2;
                return element;
            }
            static_cast<void>(parse_name());
            skip_whitespace();
            if (pos_ < input_.size() && input_[pos_] == '=') {
                ++pos_;
                skip_whitespace();
                skip_attribute_value();
            }
        }

        for (;;) {
            if (pos_ >= input_.size()) {
                parse_failure("unterminated element <" + element.name + ">");
            }
            if (input_[pos_] != '<') {
                const std::size_t begin = pos_;
                while (pos_ < input_.size() && input_[pos_] != '<') {
                    ++pos_;
                }
                append_decoded(element.text, input_.substr(begin, pos_ - begin));
                continue;
            }
            if (starts_with("</")) {
                pos_ += 2;
                const std::string_view name = parse_name();
                if (name != element.name) {
                    parse_failure("mismatched end tag </" + std::string(name) + ">");
                }
                skip_whitespace();
                if (pos_ >= input_.size() || input_[pos_] != '>') {
                    parse_failure("malformed end tag");
                }
                ++pos_;
                return element;
            }
            if (starts_with("<!--")) {
                skip_past("-->", "unterminated comment");
                continue;
            }
            if (starts_with("<![CDATA[")) {
                const std::size_t begin = pos_ + 9;
                const std::size_t end = input_.find("]]>", begin);
                if (end == std::string_view::npos) {
                    parse_failure("unterminated CDATA section");
                }
                element.text.append(input_.substr(begin, end - begin));
                pos_ = end + 3;
                continue;
            }
            if (starts_with("<?")) {
                skip_past("?>", "unterminated processing instruction");
                continue;
            }
            if (starts_with("<!")) {
                skip_declaration();
                continue;
            }
            element.children.push_back(parse_element(depth + 1));
        }
    }

    std::string_view input_;
    std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Text helpers used by the plist value builders
// ---------------------------------------------------------------------------

[[nodiscard]] std::string trimmed(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && is_space(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && is_space(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

[[nodiscard]] plist::Bytes decode_base64(std::string_view text) {
    static constexpr std::array<std::int8_t, 256> table = [] {
        std::array<std::int8_t, 256> values{};
        values.fill(-1);
        const std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t index = 0; index < alphabet.size(); ++index) {
            values[static_cast<std::size_t>(
                static_cast<unsigned char>(alphabet[index]))] = static_cast<std::int8_t>(index);
        }
        return values;
    }();

    plist::Bytes decoded;
    decoded.reserve(text.size() * 3 / 4);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (is_space(byte)) {
            continue;
        }
        if (c == '=') {
            break;
        }
        const std::int8_t value = table[byte];
        if (value < 0) {
            parse_failure("invalid base64 data");
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return decoded;
}

[[nodiscard]] std::int64_t parse_integer_text(std::string_view text) {
    const std::string value = trimmed(text);
    if (value.empty()) {
        parse_failure("empty integer value");
    }
    std::string_view digits = value;
    int base = 10;
    bool negative = false;
    if (digits.front() == '-' || digits.front() == '+') {
        negative = digits.front() == '-';
        digits.remove_prefix(1);
    }
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits.remove_prefix(2);
        base = 16;
    }
    std::int64_t magnitude = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), magnitude, base);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
        parse_failure("invalid integer value: " + value);
    }
    return negative ? -magnitude : magnitude;
}

[[nodiscard]] double parse_real_text(std::string_view text) {
    const std::string value = trimmed(text);
    if (value.empty()) {
        parse_failure("empty real value");
    }
    // from_chars for floating point is available in C++20 but its support is
    // still uneven, so use strtod on the trimmed copy.
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        parse_failure("invalid real value: " + value);
    }
    return parsed;
}

// ---------------------------------------------------------------------------
// XML plist -> Value
// ---------------------------------------------------------------------------

[[nodiscard]] Value value_from_element(const XmlElement& element);

[[nodiscard]] Value value_from_dictionary(const XmlElement& element) {
    std::map<std::string, Value, std::less<>> dictionary;
    const std::vector<XmlElement>& children = element.children;
    std::size_t index = 0;
    while (index < children.size()) {
        if (children[index].name != "key") {
            // Early SysInfoExtended files wrapped keys in unrelated tags; the
            // original parser skipped such nodes instead of failing.
            ++index;
            continue;
        }
        const std::string key = children[index].text;
        ++index;
        if (index >= children.size()) {
            parse_failure("<key> " + key + " with no corresponding value node");
        }
        const XmlElement& value_element = children[index];
        ++index;
        try {
            dictionary.insert_or_assign(key, value_from_element(value_element));
        } catch (const Error&) {
            // A value that cannot be parsed is dropped, matching libgpod's
            // "warn and continue" behaviour for individual dictionary entries.
        }
    }
    return Value(std::move(dictionary));
}

[[nodiscard]] Value value_from_array(const XmlElement& element) {
    std::vector<Value> items;
    for (const XmlElement& child : element.children) {
        try {
            items.push_back(value_from_element(child));
        } catch (const Error&) {
            // iPhones put <string> entries next to artwork format dictionaries;
            // anything that is not a plist value is ignored.
        }
    }
    return Value(std::move(items));
}

[[nodiscard]] Value value_from_element(const XmlElement& element) {
    const std::string_view name = element.name;
    if (name == "dict") {
        return value_from_dictionary(element);
    }
    if (name == "array") {
        return value_from_array(element);
    }
    if (name == "string") {
        return Value(std::string(element.text));
    }
    if (name == "integer") {
        return Value(parse_integer_text(element.text));
    }
    if (name == "real") {
        return Value(parse_real_text(element.text));
    }
    if (name == "true") {
        return Value(true);
    }
    if (name == "false") {
        return Value(false);
    }
    if (name == "data") {
        return Value(decode_base64(element.text));
    }
    parse_failure("unknown plist element <" + std::string(name) + ">");
}

[[nodiscard]] Value value_from_xml(std::string_view xml) {
    XmlParser parser(xml);
    const XmlElement root = parser.parse_document();
    if (root.name != "plist") {
        parse_failure("document does not start with <plist>");
    }
    if (root.children.empty()) {
        parse_failure("empty plist document");
    }
    return value_from_element(root.children.front());
}

// ---------------------------------------------------------------------------
// Binary plist -> Value
// ---------------------------------------------------------------------------

class BinaryParser {
public:
    explicit BinaryParser(std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] Value parse_document() {
        if (data_.size() < 8 + kTrailerSize ||
            std::memcmp(data_.data(), "bplist00", 8) != 0) {
            parse_failure("not a binary plist");
        }
        const std::size_t trailer = data_.size() - kTrailerSize;
        offset_size_ = data_[trailer + 6];
        ref_size_ = data_[trailer + 7];
        object_count_ = read_big_endian(trailer + 8, 8);
        top_object_ = read_big_endian(trailer + 16, 8);
        offset_table_ = static_cast<std::size_t>(read_big_endian(trailer + 24, 8));
        if (offset_size_ == 0 || offset_size_ > 8 || ref_size_ == 0 || ref_size_ > 8) {
            parse_failure("invalid binary plist trailer");
        }
        if (object_count_ == 0 || top_object_ >= object_count_) {
            parse_failure("invalid binary plist object table");
        }
        if (offset_table_ >= trailer ||
            object_count_ > (trailer - offset_table_) / offset_size_) {
            parse_failure("binary plist offset table out of range");
        }
        return object(top_object_, 0);
    }

private:
    static constexpr std::size_t kTrailerSize = 32;
    static constexpr int kMaxObjectDepth = 64;

    [[nodiscard]] std::uint64_t read_big_endian(std::size_t pos, unsigned width) const {
        if (width > 8 || pos > data_.size() || width > data_.size() - pos) {
            parse_failure("truncated binary plist");
        }
        std::uint64_t value = 0;
        for (unsigned index = 0; index < width; ++index) {
            value = (value << 8) | data_[pos + index];
        }
        return value;
    }

    [[nodiscard]] std::size_t object_offset(std::uint64_t index) const {
        const std::uint64_t offset = read_big_endian(
            offset_table_ + static_cast<std::size_t>(index) * offset_size_, offset_size_);
        if (offset >= data_.size()) {
            parse_failure("binary plist object offset out of range");
        }
        return static_cast<std::size_t>(offset);
    }

    // Reads the element count of a variable-length object, returning the
    // position that follows it.
    [[nodiscard]] std::uint64_t read_count(std::size_t pos, unsigned info,
                                           std::size_t& next) const {
        if (info != 0x0F) {
            next = pos;
            return info;
        }
        if (pos >= data_.size() || (data_[pos] >> 4) != 0x1) {
            parse_failure("binary plist length is not an integer");
        }
        const unsigned exponent = data_[pos] & 0x0F;
        if (exponent > 3) {
            parse_failure("binary plist length is too large");
        }
        const unsigned width = 1u << exponent;
        const std::uint64_t count = read_big_endian(pos + 1, width);
        next = pos + 1 + width;
        return count;
    }

    [[nodiscard]] Value object(std::uint64_t index, int depth) const {
        if (depth > kMaxObjectDepth) {
            parse_failure("binary plist objects nested too deeply");
        }
        if (index >= object_count_) {
            parse_failure("binary plist object index out of range");
        }
        const std::size_t offset = object_offset(index);
        const std::uint8_t marker = data_[offset];
        const unsigned type = marker >> 4;
        const unsigned info = marker & 0x0F;

        switch (type) {
            case 0x0:
                if (info == 0x8) {
                    return Value(false);
                }
                if (info == 0x9) {
                    return Value(true);
                }
                return Value();
            case 0x1: {
                const unsigned width = info == 0 ? 1u : (1u << info);
                if (width > 8 || offset + 1 > data_.size() ||
                    width > data_.size() - offset - 1) {
                    parse_failure("truncated binary plist integer");
                }
                const std::uint64_t raw = read_big_endian(offset + 1, width);
                // 8-byte integers are signed; shorter ones never overflow.
                return Value(static_cast<std::int64_t>(raw));
            }
            case 0x2: {
                if (info == 2) {
                    const std::uint32_t bits =
                        static_cast<std::uint32_t>(read_big_endian(offset + 1, 4));
                    float number = 0.0F;
                    std::memcpy(&number, &bits, sizeof number);
                    return Value(static_cast<double>(number));
                }
                if (info == 3) {
                    const std::uint64_t bits = read_big_endian(offset + 1, 8);
                    double number = 0.0;
                    std::memcpy(&number, &bits, sizeof number);
                    return Value(number);
                }
                parse_failure("unsupported binary plist real width");
            }
            case 0x3:
                // <date> values are seconds since 2001-01-01; callers do not
                // need them, so keep the raw offset as a real.
                return Value(static_cast<double>(read_big_endian(offset + 1, 8)));
            case 0x4: {
                std::size_t next = 0;
                const std::uint64_t count = read_count(offset + 1, info, next);
                if (count > data_.size() - next) {
                    parse_failure("truncated binary plist data");
                }
                const std::span<const std::uint8_t> bytes =
                    data_.subspan(next, static_cast<std::size_t>(count));
                return Value(plist::Bytes(bytes.begin(), bytes.end()));
            }
            case 0x5: {
                std::size_t next = 0;
                const std::uint64_t count = read_count(offset + 1, info, next);
                if (count > data_.size() - next) {
                    parse_failure("truncated binary plist string");
                }
                return Value(std::string_view(
                    reinterpret_cast<const char*>(data_.data() + next),
                    static_cast<std::size_t>(count)));
            }
            case 0x6: {
                std::size_t next = 0;
                const std::uint64_t units = read_count(offset + 1, info, next);
                const std::uint64_t bytes = units * 2;
                if (bytes > data_.size() - next) {
                    parse_failure("truncated binary plist UTF-16 string");
                }
                std::string utf8;
                utf8.reserve(static_cast<std::size_t>(units));
                for (std::uint64_t unit_index = 0; unit_index < units; ++unit_index) {
                    const std::uint64_t position = next + unit_index * 2;
                    std::uint32_t code_unit = static_cast<std::uint32_t>(read_big_endian(
                        static_cast<std::size_t>(position), 2));
                    if (code_unit >= 0xD800 && code_unit <= 0xDBFF &&
                        unit_index + 1 < units) {
                        const std::uint64_t low_position = position + 2;
                        const std::uint32_t low = static_cast<std::uint32_t>(read_big_endian(
                            static_cast<std::size_t>(low_position), 2));
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            code_unit = 0x10000 + ((code_unit - 0xD800) << 10) + (low - 0xDC00);
                            ++unit_index;
                        }
                    }
                    append_utf8(utf8, code_unit);
                }
                return Value(std::move(utf8));
            }
            case 0x8:
                // UIDs are only used by keyed archives; expose them as integers.
                return Value(static_cast<std::int64_t>(read_big_endian(offset + 1, info + 1)));
            case 0xA:
            case 0xC: {
                std::size_t next = 0;
                const std::uint64_t count = read_count(offset + 1, info, next);
                if (count > (data_.size() - next) / ref_size_) {
                    parse_failure("truncated binary plist array");
                }
                std::vector<Value> items;
                items.reserve(static_cast<std::size_t>(count));
                for (std::uint64_t item = 0; item < count; ++item) {
                    const std::uint64_t reference = read_big_endian(
                        next + static_cast<std::size_t>(item) * ref_size_, ref_size_);
                    items.push_back(object(reference, depth + 1));
                }
                return Value(std::move(items));
            }
            case 0xD: {
                std::size_t next = 0;
                const std::uint64_t count = read_count(offset + 1, info, next);
                if (count > (data_.size() - next) / (2 * ref_size_)) {
                    parse_failure("truncated binary plist dictionary");
                }
                const std::size_t keys = next;
                const std::size_t values = next + static_cast<std::size_t>(count) * ref_size_;
                std::map<std::string, Value, std::less<>> dictionary;
                for (std::uint64_t entry = 0; entry < count; ++entry) {
                    const std::uint64_t key_reference = read_big_endian(
                        keys + static_cast<std::size_t>(entry) * ref_size_, ref_size_);
                    const std::uint64_t value_reference = read_big_endian(
                        values + static_cast<std::size_t>(entry) * ref_size_, ref_size_);
                    const Value key = object(key_reference, depth + 1);
                    if (!key.is_string()) {
                        continue;
                    }
                    dictionary.insert_or_assign(
                        std::string(key.text()), object(value_reference, depth + 1));
                }
                return Value(std::move(dictionary));
            }
            default:
                parse_failure("unsupported binary plist object type");
        }
    }

    std::span<const std::uint8_t> data_;
    unsigned offset_size_ = 0;
    unsigned ref_size_ = 0;
    std::uint64_t object_count_ = 0;
    std::uint64_t top_object_ = 0;
    std::size_t offset_table_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

struct Value::Node {
    Type type = Type::Null;
    bool boolean = false;
    std::int64_t integer = 0;
    double real = 0.0;
    std::string string;
    plist::Bytes data;
    std::vector<Value> array;
    std::map<std::string, Value, std::less<>> dictionary;
};

Value::Value() noexcept = default;
Value::~Value() = default;
Value::Value(const Value& other) noexcept = default;
Value::Value(Value&& other) noexcept = default;
Value& Value::operator=(const Value& other) noexcept = default;
Value& Value::operator=(Value&& other) noexcept = default;

Value::Value(bool value) : node_(std::make_shared<Node>()) {
    node_->type = Type::Boolean;
    node_->boolean = value;
}

Value::Value(std::int64_t value) : node_(std::make_shared<Node>()) {
    node_->type = Type::Integer;
    node_->integer = value;
}

Value::Value(double value) : node_(std::make_shared<Node>()) {
    node_->type = Type::Real;
    node_->real = value;
}

Value::Value(std::string value) : node_(std::make_shared<Node>()) {
    node_->type = Type::String;
    node_->string = std::move(value);
}

Value::Value(std::string_view value) : Value(std::string(value)) {}

Value::Value(plist::Bytes value) : node_(std::make_shared<Node>()) {
    node_->type = Type::Data;
    node_->data = std::move(value);
}

Value::Value(std::vector<Value> value) : node_(std::make_shared<Node>()) {
    node_->type = Type::Array;
    node_->array = std::move(value);
}

Value::Value(std::map<std::string, Value, std::less<>> value)
    : node_(std::make_shared<Node>()) {
    node_->type = Type::Dictionary;
    node_->dictionary = std::move(value);
}

Value::Type Value::type() const noexcept {
    return node_ ? node_->type : Type::Null;
}

bool Value::is_null() const noexcept { return type() == Type::Null; }
bool Value::is_boolean() const noexcept { return type() == Type::Boolean; }
bool Value::is_integer() const noexcept { return type() == Type::Integer; }
bool Value::is_real() const noexcept { return type() == Type::Real; }
bool Value::is_string() const noexcept { return type() == Type::String; }
bool Value::is_data() const noexcept { return type() == Type::Data; }
bool Value::is_array() const noexcept { return type() == Type::Array; }
bool Value::is_dictionary() const noexcept { return type() == Type::Dictionary; }

bool Value::boolean(bool fallback) const noexcept {
    return is_boolean() ? node_->boolean : fallback;
}

std::int64_t Value::integer(std::int64_t fallback) const noexcept {
    return is_integer() ? node_->integer : fallback;
}

double Value::real(double fallback) const noexcept {
    return is_real() ? node_->real : fallback;
}

std::string_view Value::text(std::string_view fallback) const noexcept {
    return is_string() ? std::string_view(node_->string) : fallback;
}

const plist::Bytes& Value::data() const noexcept {
    static const plist::Bytes empty;
    return is_data() ? node_->data : empty;
}

const std::vector<Value>& Value::array() const noexcept {
    static const std::vector<Value> empty;
    return is_array() ? node_->array : empty;
}

const std::map<std::string, Value, std::less<>>& Value::dictionary() const noexcept {
    static const std::map<std::string, Value, std::less<>> empty;
    return is_dictionary() ? node_->dictionary : empty;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (!is_dictionary()) {
        return nullptr;
    }
    const auto entry = node_->dictionary.find(key);
    return entry == node_->dictionary.end() ? nullptr : &entry->second;
}

bool Value::contains(std::string_view key) const noexcept { return find(key) != nullptr; }

std::string_view Value::text_of(std::string_view key, std::string_view fallback) const noexcept {
    const Value* entry = find(key);
    return entry == nullptr ? fallback : entry->text(fallback);
}

std::int64_t Value::integer_of(std::string_view key, std::int64_t fallback) const noexcept {
    const Value* entry = find(key);
    return entry == nullptr ? fallback : entry->integer(fallback);
}

bool Value::boolean_of(std::string_view key, bool fallback) const noexcept {
    const Value* entry = find(key);
    return entry == nullptr ? fallback : entry->boolean(fallback);
}

std::size_t Value::size() const noexcept {
    switch (type()) {
        case Type::Array:
            return node_->array.size();
        case Type::Dictionary:
            return node_->dictionary.size();
        default:
            return 0;
    }
}

bool Value::empty() const noexcept { return size() == 0; }

std::string_view to_string(Value::Type type) noexcept {
    switch (type) {
        case Value::Type::Null:
            return "null";
        case Value::Type::Boolean:
            return "boolean";
        case Value::Type::Integer:
            return "integer";
        case Value::Type::Real:
            return "real";
        case Value::Type::String:
            return "string";
        case Value::Type::Data:
            return "data";
        case Value::Type::Array:
            return "array";
        case Value::Type::Dictionary:
            return "dictionary";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

Value parse_xml(std::string_view xml) { return value_from_xml(xml); }

Value parse_binary(std::span<const std::uint8_t> data) {
    BinaryParser parser(data);
    return parser.parse_document();
}

Value parse(std::span<const std::uint8_t> data) {
    if (data.size() >= 8 && std::memcmp(data.data(), "bplist00", 8) == 0) {
        return parse_binary(data);
    }
    if (data.empty()) {
        return parse_xml(std::string_view{});
    }
    return parse_xml(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

Value parse_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(ErrorCode::NotFound, "plist: cannot open '" + path.string() + "'");
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    if (!input.eof() && input.fail()) {
        fail(ErrorCode::Io, "plist: cannot read '" + path.string() + "'");
    }
    return parse(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(content.data()), content.size()));
}

bool try_parse_file(const std::filesystem::path& path, Value& out) noexcept {
    try {
        out = parse_file(path);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace gpod::plist
