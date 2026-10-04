// src/core/result.h
// DevDesk 统一错误处理（不抛异常，对非 C++ 用户友好）
#pragma once

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "core/logging.h"

namespace core {

// 错误码枚举（唯一权威定义）
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
    static Result ok(T value) {
        Result r;
        r.data_.template emplace<1>(std::move(value));
        return r;
    }

    static Result fail(ErrorCode code, std::string message) {
        Result r;
        r.code_ = code;
        r.data_.template emplace<2>(std::move(message));
        return r;
    }

    bool isOk() const { return data_.index() == 1; }
    bool isFail() const { return !isOk(); }

    // 安全取值：可能失败时的唯一取值口
    T valueOr(T defaultValue) const {
        if (isOk()) return std::get<1>(data_);
        return defaultValue;
    }

    // 显式取值：只在「按不变量不可能失败」处调用；失败即 LOG_FATAL + std::abort()，不抛异常
    //（抛在 -fno-exceptions 的 TU 编译不过、带异常又能被捕获，行为随编译选项漂移）。可能失败的
    // 取值一律走 valueOr()
    const T& value() const {
        if (!isOk()) {
            LOG_FATAL("Result::value() called on error: " + errorMessage());
            std::abort();
        }
        return std::get<1>(data_);
    }

    // 非 const 版本（需修改 result 中的对象，如 sqlite3_stmt）
    T& value() {
        if (!isOk()) {
            LOG_FATAL("Result::value() called on error: " + errorMessage());
            std::abort();
        }
        return std::get<1>(data_);
    }

    ErrorCode errorCode() const { return code_; }

    // 默认构造态（索引 0）无错误信息：返回占位串而非 get<2> 抛 bad_variant_access（-fno-exceptions
    // 下抛即 terminate）。静态存储保证引用有效
    const std::string& errorMessage() const {
        static const std::string kUninitialized = "（未初始化）";
        if (data_.index() == 0) return kUninitialized;
        return std::get<2>(data_);
    }

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