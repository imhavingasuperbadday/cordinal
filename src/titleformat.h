// A compact title-formatting language in the style of foobar2000's public syntax:
//   %field%      field value, or "?" when missing
//   [ ... ]      shown only if a field or function inside it has a value
//   'text'       literal text ('' is a single quote)
//   $func(a,b)   functions: if, if2, if3, ifequal, ifgreater, iflonger, and, or, not, xor,
//                greater, strcmp, stricmp, upper, lower, caps, num, left, cut, right, pad,
//                pad_right, trim, len, repeat, replace, add, sub, mul, div, mod, min, max,
//                and the user's own functions (SetTitleFunctions)
// A missing field emits "?"; an unknown function emits "[UNKNOWN FUNCTION]". Evaluation is
// self-contained and bounded, so FormatTitle is safe to call with user-typed text.
#pragma once
#include <functional>
#include <string>
#include <vector>

using FieldLookup = std::function<bool(const std::wstring& name, std::wstring& out)>;

std::wstring FormatTitle(const std::wstring& pattern, const FieldLookup& lookup);

// A user function, written $define(name, param..., body) in a pack's functions.tf. Inside the body
// %param% is the argument's value; a call is bounded by the same depth and work caps as the rest
// of the pattern, so a function that calls itself forever just stops.
struct TitleFunction {
    std::wstring name;
    std::vector<std::wstring> params;
    std::wstring body;
};
// Reads every $define in `source` ('//' starts a comment line). A definition that can't be used
// (bad name, a built-in's name, a missing body) is left out and described in `errors`.
std::vector<TitleFunction> ParseTitleFunctions(const std::wstring& source, std::vector<std::wstring>& errors);
// Replaces the user functions every later FormatTitle call can use, on any thread; a call already
// running keeps the set it started with. Built-in functions always win over a user function.
void SetTitleFunctions(std::vector<TitleFunction> functions);
