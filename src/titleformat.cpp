// Title formatting: turns a pattern such as "%artist% - %title%" into a string for one track.
// The grammar is documented in titleformat.h; this file is the recursive-descent evaluator for it.
// Every field lookup is routed through the caller's FieldLookup, so nothing here knows about the
// library. Depth and work caps keep a pathological pattern from running away.
#include "titleformat.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace {

using Functions = std::shared_ptr<const std::vector<TitleFunction>>;
std::mutex g_functionsLock;
Functions g_functions;

// The built-in functions, which a user function can't replace.
const wchar_t* const kBuiltins[] = {L"if", L"if2", L"if3", L"ifequal", L"ifgreater", L"iflonger", L"and", L"or", L"not",
    L"xor", L"greater", L"strcmp", L"stricmp", L"upper", L"lower", L"caps", L"num", L"left", L"cut", L"right", L"pad",
    L"pad_right", L"trim", L"len", L"repeat", L"replace", L"add", L"sub", L"mul", L"div", L"mod", L"min", L"max"};

// One formatted fragment: `text` is what gets emitted; `truth` records whether any field or
// function inside it produced a value, which is what [ ... ] and $if test.
struct Value {
    std::wstring text;
    bool truth = false;
};

// Caps: total output length, and nesting depth of [ ] / $func() recursion.
constexpr size_t kMaxText = 65536;
constexpr unsigned kMaxDepth = 64;
long long ToInt(const std::wstring& s) { return wcstoll(s.c_str(), nullptr, 10); }

// Saturating arithmetic: an overflowing add/mul clamps to the type's limit instead of wrapping, so a
// pattern with huge numbers yields a value rather than undefined behavior.
long long Add(long long a, long long b) {
    constexpr auto hi = std::numeric_limits<long long>::max(), lo = std::numeric_limits<long long>::min();
    if (b > 0 && a > hi - b) return hi;
    if (b < 0 && a < lo - b) return lo;
    return a + b;
}

long long Multiply(long long a, long long b) {
    constexpr auto hi = std::numeric_limits<long long>::max(), lo = std::numeric_limits<long long>::min();
    if (!a || !b) return 0;
    if (a > 0 ? (b > 0 ? a > hi / b : b < lo / a) : (b > 0 ? a < lo / b : a < hi / b))
        return (a < 0) != (b < 0) ? lo : hi;
    return a * b;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// A user function's parameters while its body runs: %param% is the argument as it was evaluated.
struct Bound {
    const TitleFunction* f = nullptr;
    const std::vector<Value>* args = nullptr;
};

// What one FormatTitle call may spend, shared by the evaluators of the user functions it calls, and
// the user functions it saw when it first needed them.
struct Budget {
    unsigned depth = 0;
    size_t work = 0;
    bool fetched = false;
    Functions functions;
};

class Evaluator {
public:
    Evaluator(const std::wstring& pattern, const FieldLookup& lookup, Budget& budget, Bound bound = {})
        : s_(pattern), lookup_(lookup), depth_(budget.depth), work_(budget.work), budget_(budget), bound_(bound) {}

    std::wstring Run() { return Sequence(0, 0).text; }
    Value RunValue() { return Sequence(0, 0); }

private:
    const std::wstring& s_;
    const FieldLookup& lookup_;
    size_t i_ = 0;
    unsigned& depth_;
    size_t& work_;
    Budget& budget_;
    Bound bound_;

    const Value* Param(const std::wstring& name) const {
        if (!bound_.f) return nullptr;
        for (size_t k = 0; k < bound_.f->params.size(); ++k)
            if (bound_.f->params[k] == name) {
                static const Value none;
                return k < bound_.args->size() ? &(*bound_.args)[k] : &none;
            }
        return nullptr;
    }

    // Reads a run of characters up to `stopA` / `stopB` (either may be 0 for "no stop").
    Value Sequence(wchar_t stopA, wchar_t stopB) {
        Value out;
        if (depth_ >= kMaxDepth) { i_ = s_.size(); return out; }
        ++depth_;
        while (i_ < s_.size()) {
            if (out.text.size() >= kMaxText || ++work_ > kMaxText * 16) { i_ = s_.size(); break; }
            wchar_t c = s_[i_];
            if ((stopA && c == stopA) || (stopB && c == stopB)) break;
            if (c == L'%') {
                size_t end = s_.find(L'%', i_ + 1);
                if (end == std::wstring::npos) {
                    out.text += s_.substr(i_);
                    i_ = s_.size();
                    break;
                }
                std::wstring name = Lower(s_.substr(i_ + 1, end - i_ - 1));
                i_ = end + 1;
                if (name.empty()) {
                    out.text += L'%';
                    continue;
                }
                // Missing or empty fields become "?" and leave truth unset, so [ ... ] drops them. A
                // parameter is its argument's text, with that argument's truth.
                std::wstring value;
                if (const Value* arg = Param(name)) {
                    out.text += arg->text;
                    out.truth = out.truth || arg->truth;
                } else if (lookup_(name, value) && !value.empty()) {
                    out.text += value;
                    out.truth = true;
                } else {
                    out.text += L'?';
                }
            } else if (c == L'[') {
                ++i_;
                Value inner = Sequence(L']', 0);
                if (i_ < s_.size()) ++i_;
                if (inner.truth) {
                    out.text += inner.text;
                    out.truth = true;
                }
            } else if (c == L'\'') {
                size_t end = s_.find(L'\'', i_ + 1);
                if (end == std::wstring::npos) {
                    out.text += s_.substr(i_ + 1);
                    i_ = s_.size();
                    break;
                }
                out.text += end == i_ + 1 ? L"'" : s_.substr(i_ + 1, end - i_ - 1);
                i_ = end + 1;
            } else if (c == L'$') {
                Value v = Function();
                out.text += v.text;
                out.truth = out.truth || v.truth;
            } else {
                out.text += c;
                ++i_;
            }
        }
        out.text.resize(std::min(out.text.size(), kMaxText));
        --depth_;
        return out;
    }

    // Parses "$name" and, if a '(' follows, its comma-separated arguments, then invokes it.
    Value Function() {
        ++i_;  // '$'
        size_t start = i_;
        while (i_ < s_.size() && (iswalnum(s_[i_]) || s_[i_] == L'_')) ++i_;
        std::wstring name = Lower(s_.substr(start, i_ - start));
        if (i_ >= s_.size() || s_[i_] != L'(') return {L"$" + name, false};
        ++i_;
        std::vector<Value> args;
        if (i_ < s_.size() && s_[i_] == L')') {
            ++i_;
        } else {
            for (;;) {
                args.push_back(Sequence(L',', L')'));
                if (i_ >= s_.size()) break;
                if (s_[i_++] == L')') break;
            }
        }
        return Call(name, args);
    }

    // A user function: its body evaluated with %param% bound to each argument, inside this call's
    // depth and work budget.
    Value CallUser(const TitleFunction& f, const std::vector<Value>& a) {
        if (depth_ >= kMaxDepth) return {};
        ++depth_;
        Value v = Evaluator(f.body, lookup_, budget_, {&f, &a}).RunValue();
        --depth_;
        return v;
    }

    Value Call(const std::wstring& n, const std::vector<Value>& a) {
        auto arg = [&](size_t k) { return k < a.size() ? a[k] : Value{}; };
        auto num = [&](size_t k) { return ToInt(arg(k).text); };
        auto boolean = [](bool b) { return Value{L"", b}; };
        auto text = [&](std::wstring s) { return Value{std::move(s), arg(0).truth}; };
        auto number = [&](long long v) { return Value{std::to_wstring(v), arg(0).truth}; };

        if (n == L"if") return arg(0).truth ? arg(1) : arg(2);
        if (n == L"if2") return arg(0).truth ? arg(0) : arg(1);
        if (n == L"if3") {
            for (size_t k = 0; k + 1 < a.size(); ++k)
                if (a[k].truth) return a[k];
            return a.empty() ? Value{} : a.back();
        }
        if (n == L"ifequal") return num(0) == num(1) ? arg(2) : arg(3);
        if (n == L"ifgreater") return num(0) > num(1) ? arg(2) : arg(3);
        if (n == L"iflonger") return (long long)arg(0).text.size() > num(1) ? arg(2) : arg(3);
        if (n == L"and") return boolean(std::all_of(a.begin(), a.end(), [](const Value& v) { return v.truth; }));
        if (n == L"or") return boolean(std::any_of(a.begin(), a.end(), [](const Value& v) { return v.truth; }));
        if (n == L"not") return boolean(!arg(0).truth);
        if (n == L"xor") return boolean(arg(0).truth != arg(1).truth);
        if (n == L"greater") return boolean(num(0) > num(1));
        if (n == L"strcmp") return boolean(arg(0).text == arg(1).text);
        if (n == L"stricmp") return boolean(_wcsicmp(arg(0).text.c_str(), arg(1).text.c_str()) == 0);
        if (n == L"upper" || n == L"lower") {
            std::wstring s = arg(0).text;
            for (auto& c : s) c = (wchar_t)(n == L"upper" ? towupper(c) : towlower(c));
            return text(s);
        }
        if (n == L"caps") {
            std::wstring s = arg(0).text;
            bool start = true;
            for (auto& c : s) {
                c = (wchar_t)(start ? towupper(c) : towlower(c));
                start = iswspace(c) != 0;
            }
            return text(s);
        }
        if (n == L"num") {
            wchar_t buf[64];
            swprintf_s(buf, L"%0*lld", (int)std::clamp(num(1), 0LL, 32LL), num(0));
            return text(buf);
        }
        if (n == L"left" || n == L"cut") {
            const std::wstring& s = arg(0).text;
            return text(s.substr(0, (size_t)std::clamp(num(1), 0LL, (long long)s.size())));
        }
        if (n == L"right") {
            const std::wstring& s = arg(0).text;
            size_t k = (size_t)std::clamp(num(1), 0LL, (long long)s.size());
            return text(s.substr(s.size() - k));
        }
        if (n == L"pad" || n == L"pad_right") {
            std::wstring s = arg(0).text;
            size_t len = (size_t)std::clamp(num(1), 0LL, 4096LL);
            wchar_t ch = arg(2).text.empty() ? L' ' : arg(2).text[0];
            if (s.size() < len) {
                if (n == L"pad")
                    s.append(len - s.size(), ch);
                else
                    s.insert(0, len - s.size(), ch);
            }
            return text(s);
        }
        if (n == L"trim") {
            const std::wstring& s = arg(0).text;
            size_t b = s.find_first_not_of(L" \t\r\n"), e = s.find_last_not_of(L" \t\r\n");
            return text(b == std::wstring::npos ? L"" : s.substr(b, e - b + 1));
        }
        if (n == L"len") return number((long long)arg(0).text.size());
        if (n == L"repeat") {
            std::wstring s;
            const std::wstring unit = arg(0).text;
            for (long long k = 0; k < std::clamp(num(1), 0LL, 1024LL) && s.size() < kMaxText; ++k)
                s.append(unit, 0, std::min(unit.size(), kMaxText - s.size()));
            return text(s);
        }
        if (n == L"replace") {
            std::wstring s = arg(0).text;
            for (size_t k = 1; k + 1 < a.size(); k += 2) {
                const std::wstring& from = a[k].text;
                if (from.empty()) continue;
                for (size_t pos = 0; (pos = s.find(from, pos)) != std::wstring::npos; pos += a[k + 1].text.size()) {
                    if (a[k + 1].text.size() > kMaxText - (s.size() - from.size())) break;
                    s.replace(pos, from.size(), a[k + 1].text);
                }
            }
            return text(s);
        }
        if (n == L"add" || n == L"sub" || n == L"mul" || n == L"div" || n == L"mod" || n == L"min" || n == L"max") {
            long long r = num(0);
            for (size_t k = 1; k < a.size(); ++k) {
                long long v = num(k);
                if (n == L"add") r = Add(r, v);
                else if (n == L"sub") r = v == std::numeric_limits<long long>::min()
                    ? (r >= 0 ? std::numeric_limits<long long>::max() : Add(Add(r, std::numeric_limits<long long>::max()), 1)) : Add(r, -v);
                else if (n == L"mul") r = Multiply(r, v);
                else if (n == L"div") r = v ? (r == std::numeric_limits<long long>::min() && v == -1 ? std::numeric_limits<long long>::max() : r / v) : r;
                else if (n == L"mod") r = v ? (r == std::numeric_limits<long long>::min() && v == -1 ? 0 : r % v) : r;
                else if (n == L"min") r = std::min(r, v);
                else r = std::max(r, v);
            }
            return number(r);
        }
        if (!budget_.fetched) {
            std::lock_guard lock(g_functionsLock);
            budget_.functions = g_functions;
            budget_.fetched = true;
        }
        if (budget_.functions)
            for (const TitleFunction& f : *budget_.functions)
                if (f.name == n) return CallUser(f, a);
        return {L"[UNKNOWN FUNCTION]", false};
    }
};

bool IsName(const std::wstring& s) {
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](wchar_t c) { return iswalnum(c) || c == L'_'; });
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n"), e = s.find_last_not_of(L" \t\r\n");
    return b == std::wstring::npos ? L"" : s.substr(b, e - b + 1);
}

}  // namespace

std::wstring FormatTitle(const std::wstring& pattern, const FieldLookup& lookup) {
    Budget budget;
    return Evaluator(pattern, lookup, budget).Run();
}

// Each $define's arguments are split at the commas outside parentheses, brackets and quotes, and
// kept as written: the body is evaluated only when the function is called.
std::vector<TitleFunction> ParseTitleFunctions(const std::wstring& source, std::vector<std::wstring>& errors) {
    std::vector<TitleFunction> out;
    size_t i = 0;
    auto line = [&](size_t at) { return std::to_wstring(1 + std::count(source.begin(), source.begin() + at, L'\n')); };
    while (i < source.size()) {
        if (source.compare(i, 2, L"//") == 0) {
            i = source.find(L'\n', i);
            if (i == std::wstring::npos) break;
            continue;
        }
        if (source.compare(i, 8, L"$define(") != 0) {
            ++i;
            continue;
        }
        size_t start = i;
        i += 8;
        std::vector<std::wstring> args(1);
        int depth = 0;
        bool quoted = false, closed = false;
        for (; i < source.size(); ++i) {
            wchar_t c = source[i];
            if (c == L'\'') quoted = !quoted;
            else if (!quoted && (c == L'(' || c == L'[')) ++depth;
            else if (!quoted && c == L']') --depth;
            else if (!quoted && c == L')' && depth-- == 0) { closed = true; ++i; break; }
            else if (!quoted && c == L',' && depth == 0) { args.emplace_back(); continue; }
            args.back() += c;
        }
        TitleFunction f;
        f.name = Lower(Trim(args[0]));
        for (size_t k = 1; k + 1 < args.size(); ++k) f.params.push_back(Lower(Trim(args[k])));
        f.body = args.size() > 1 ? Trim(args.back()) : L"";
        std::wstring problem;
        if (!closed) problem = L"has no closing parenthesis";
        else if (!IsName(f.name)) problem = L"needs a name made of letters, digits and _";
        else if (std::find(std::begin(kBuiltins), std::end(kBuiltins), f.name) != std::end(kBuiltins))
            problem = L"would replace the built-in $" + f.name;
        else if (args.size() < 2) problem = L"has no body";
        else if (f.params.size() > 16 || !std::all_of(f.params.begin(), f.params.end(), IsName)) problem = L"has a bad parameter name";
        else if (f.body.size() > 4096) problem = L"has a body longer than 4096 characters";
        else if (out.size() >= 256) problem = L"is past the limit of 256 functions";
        if (problem.empty()) {
            std::erase_if(out, [&](const TitleFunction& o) { return o.name == f.name; });  // a later definition wins
            out.push_back(std::move(f));
        } else {
            errors.push_back(L"$define on line " + line(start) + L" " + problem);
        }
    }
    return out;
}

void SetTitleFunctions(std::vector<TitleFunction> functions) {
    Functions next = functions.empty() ? nullptr : std::make_shared<const std::vector<TitleFunction>>(std::move(functions));
    std::lock_guard lock(g_functionsLock);
    g_functions = std::move(next);
}
