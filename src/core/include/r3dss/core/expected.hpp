// Minimal Expected<T> / Error for explicit error propagation across module boundaries.
// No exceptions cross a module boundary; every fallible call returns Expected<T>.
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <cassert>

namespace r3dss {

enum class ErrorCode {
    Ok = 0,
    InvalidArgument,
    Io,
    Format,          // malformed or unexpected file content
    Unsupported,     // valid but not supported by this build
    CodecUnavailable,// module is a stub or the SDK is missing / unlicensed
    Cancelled,
    Verification,    // output failed verification
    Internal,
};

inline const char* errorCodeName(ErrorCode c) {
    switch (c) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::Io: return "Io";
    case ErrorCode::Format: return "Format";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::CodecUnavailable: return "CodecUnavailable";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::Verification: return "Verification";
    case ErrorCode::Internal: return "Internal";
    }
    return "?";
}

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
    std::string toString() const { return std::string(errorCodeName(code)) + ": " + message; }
};

struct Unexpected {
    Error error;
};
inline Unexpected makeError(ErrorCode c, std::string msg) { return Unexpected{Error(c, std::move(msg))}; }

template <class T> class Expected {
public:
    Expected(T value) : v_(std::move(value)) {}               // NOLINT(google-explicit-constructor)
    Expected(Unexpected u) : v_(std::move(u.error)) {}        // NOLINT(google-explicit-constructor)
    Expected(Error e) : v_(std::move(e)) {}                   // NOLINT(google-explicit-constructor)

    bool hasValue() const { return v_.index() == 0; }
    explicit operator bool() const { return hasValue(); }
    T& value() & { assert(hasValue()); return std::get<0>(v_); }
    const T& value() const& { assert(hasValue()); return std::get<0>(v_); }
    T&& value() && { assert(hasValue()); return std::get<0>(std::move(v_)); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    const Error& error() const { assert(!hasValue()); return std::get<1>(v_); }
    Error& error() { assert(!hasValue()); return std::get<1>(v_); }

private:
    std::variant<T, Error> v_;
};

template <> class Expected<void> {
public:
    Expected() = default;
    Expected(Unexpected u) : err_(std::move(u.error)), ok_(false) {} // NOLINT
    Expected(Error e) : err_(std::move(e)), ok_(false) {}            // NOLINT
    bool hasValue() const { return ok_; }
    explicit operator bool() const { return ok_; }
    const Error& error() const { assert(!ok_); return err_; }

private:
    Error err_;
    bool ok_ = true;
};

// Propagate an error out of the enclosing function if `expr` failed.
#define R3DSS_TRY(expr)                                                                            \
    do {                                                                                           \
        auto&& r3dss_try_result_ = (expr);                                                         \
        if (!r3dss_try_result_) return ::r3dss::Unexpected{r3dss_try_result_.error()};             \
    } while (0)

// Assign `var` from the value of `expr`, or propagate its error.
#define R3DSS_TRY_ASSIGN(var, expr)                                                                \
    auto&& var##_result_ = (expr);                                                                 \
    if (!var##_result_) return ::r3dss::Unexpected{var##_result_.error()};                         \
    auto&& var = *var##_result_

} // namespace r3dss
