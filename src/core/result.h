// src/core/result.h
// DevDesk 统一错误处理（不抛异常，对非 C++ 用户友好）
#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace core {

// 错误码枚举（**唯一权威定义**，见 spec §11.2）
enum class ErrorCode {
    Ok = 0,
    FileNotFound,
    FilePermissionDenied,
    FileWriteFailed,
    FileReadFailed,
    DatabaseOpenFailed,
    DatabaseCorrupted,
    DatabaseMigrationFailed,
    NetworkUnreachable,
    NetworkTimeout,
    NetworkTlsFailed,
    HttpError,
    JsonParseError,
    MarkdownParseError,
    ProfileNotFound,
    ProfileAlreadyExists,
    NoActiveProfile,
    InvalidApiKey,
    ValidationError,
    NotFound,
    AlreadyExists,
    NotImplemented,
    Internal,
};

inline const char* errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "OK";
        case ErrorCode::FileNotFound: return "FileNotFound";
        case ErrorCode::FilePermissionDenied: return "FilePermissionDenied";
        case ErrorCode::FileWriteFailed: return "FileWriteFailed";
        case ErrorCode::FileReadFailed: return "FileReadFailed";
        case ErrorCode::DatabaseOpenFailed: return "DatabaseOpenFailed";
        case ErrorCode::DatabaseCorrupted: return "DatabaseCorrupted";
        case ErrorCode::DatabaseMigrationFailed: return "DatabaseMigrationFailed";
        case ErrorCode::NetworkUnreachable: return "NetworkUnreachable";
        case ErrorCode::NetworkTimeout: return "NetworkTimeout";
        case ErrorCode::NetworkTlsFailed: return "NetworkTlsFailed";
        case ErrorCode::HttpError: return "HttpError";
        case ErrorCode::JsonParseError: return "JsonParseError";
        case ErrorCode::MarkdownParseError: return "MarkdownParseError";
        case ErrorCode::ProfileNotFound: return "ProfileNotFound";
        case ErrorCode::ProfileAlreadyExists: return "ProfileAlreadyExists";
        case ErrorCode::NoActiveProfile: return "NoActiveProfile";
        case ErrorCode::InvalidApiKey: return "InvalidApiKey";
        case ErrorCode::ValidationError: return "ValidationError";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::NotImplemented: return "NotImplemented";
        case ErrorCode::Internal: return "Internal";
    }
    return "Unknown";
}

template<typename T>
class Result {
public:
    // 构造成功
    static Result ok(T value) {
        Result r;
        r.data_.template emplace<1>(std::move(value));
        return r;
    }

    // 构造错误
    static Result fail(ErrorCode code, std::string message) {
        Result r;
        r.code_ = code;
        r.data_.template emplace<2>(std::move(message));
        return r;
    }

    bool isOk() const { return data_.index() == 1; }
    bool isFail() const { return !isOk(); }

    // 安全取值
    T valueOr(T defaultValue) const {
        if (isOk()) return std::get<1>(data_);
        return defaultValue;
    }

    // 显式取值（错误时 LOG_FATAL + 抛 logic_error）
    const T& value() const {
        if (!isOk()) {
            throw std::logic_error("Result::value() called on error: " + errorMessage());
        }
        return std::get<1>(data_);
    }

    // 非 const 版本（需要修改 result 中的对象，如 sqlite3_stmt）
    T& value() {
        if (!isOk()) {
            throw std::logic_error("Result::value() called on error: " + errorMessage());
        }
        return std::get<1>(data_);
    }

    // Rust 风格 expect
    const T& expect(const std::string& context) const {
        if (!isOk()) {
            throw std::logic_error(context + ": " + errorMessage());
        }
        return std::get<1>(data_);
    }

    T& expect(const std::string& context) {
        if (!isOk()) {
            throw std::logic_error(context + ": " + errorMessage());
        }
        return std::get<1>(data_);
    }

    // 不检查错误的取值（性能敏感）
    const T& unsafeValue() const {
        return std::get<1>(data_);
    }

    T& unsafeValue() {
        return std::get<1>(data_);
    }

    ErrorCode errorCode() const { return code_; }
    const std::string& errorMessage() const { return std::get<2>(data_); }

private:
    // variant<monostate, T, string> 索引 0=未初始化, 1=值, 2=错误信息
    std::variant<std::monostate, T, std::string> data_;
    ErrorCode code_ = ErrorCode::Ok;
};

// void 特化
template<>
class Result<void> {
public:
    static Result ok() {
        Result r;
        r.ok_ = true;
        return r;
    }
    static Result fail(ErrorCode code, std::string message) {
        Result r;
        r.ok_ = false;
        r.code_ = code;
        r.message_ = std::move(message);
        return r;
    }
    bool isOk() const { return ok_; }
    bool isFail() const { return !ok_; }
    ErrorCode errorCode() const { return code_; }
    const std::string& errorMessage() const { return message_; }
private:
    bool ok_ = false;
    ErrorCode code_ = ErrorCode::Ok;
    std::string message_;
};

} // namespace core