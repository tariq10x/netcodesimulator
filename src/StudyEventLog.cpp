#include "telemetry/StudyEventLog.hpp"

#include "app/UserDataPaths.hpp"

#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace telemetry {
namespace {

std::string escapeJsonString(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8u);
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20u) {
                    std::ostringstream code;
                    code << "\\u"
                         << std::hex
                         << std::setw(4)
                         << std::setfill('0')
                         << static_cast<int>(static_cast<unsigned char>(ch));
                    escaped += code.str();
                } else {
                    escaped.push_back(ch);
                }
                break;
        }
    }
    return escaped;
}

void appendJsonValue(std::ostringstream& stream, const StudyEventValue& value) {
    std::visit(
        [&stream](const auto& typedValue) {
            using ValueType = std::decay_t<decltype(typedValue)>;
            if constexpr (std::is_same_v<ValueType, std::string>) {
                stream << '"' << escapeJsonString(typedValue) << '"';
            } else if constexpr (std::is_same_v<ValueType, bool>) {
                stream << (typedValue ? "true" : "false");
            } else if constexpr (std::is_same_v<ValueType, double>) {
                if (std::isfinite(typedValue)) {
                    stream << std::setprecision(9) << typedValue;
                } else {
                    stream << "null";
                }
            } else {
                stream << typedValue;
            }
        },
        value);
}

std::tm localTimeFor(std::time_t timestamp) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &timestamp);
#else
    if (const std::tm* local = std::localtime(&timestamp); local != nullptr) {
        tm = *local;
    }
#endif
    return tm;
}

std::string formatLocalTime(const char* format) {
    const std::time_t now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::tm tm = localTimeFor(now);
    std::ostringstream stream;
    stream << std::put_time(&tm, format);
    return stream.str();
}

}  // namespace

StudyEventRecord& StudyEventRecord::add(std::string key, std::string value) {
    fields_.push_back(StudyEventField{std::move(key), std::move(value)});
    return *this;
}

StudyEventRecord& StudyEventRecord::add(std::string key, const char* value) {
    fields_.push_back(StudyEventField{std::move(key), std::string(value != nullptr ? value : "")});
    return *this;
}

StudyEventRecord& StudyEventRecord::add(std::string key, std::int32_t value) {
    fields_.push_back(StudyEventField{std::move(key), static_cast<std::int64_t>(value)});
    return *this;
}

StudyEventRecord& StudyEventRecord::add(std::string key, std::uint32_t value) {
    fields_.push_back(StudyEventField{std::move(key), static_cast<std::uint64_t>(value)});
    return *this;
}

StudyEventRecord& StudyEventRecord::add(std::string key, std::int64_t value) {
    fields_.push_back(StudyEventField{std::move(key), value});
    return *this;
}

StudyEventRecord& StudyEventRecord::add(std::string key, std::uint64_t value) {
    fields_.push_back(StudyEventField{std::move(key), value});
    return *this;
}

StudyEventRecord& StudyEventRecord::add(std::string key, double value) {
    fields_.push_back(StudyEventField{std::move(key), value});
    return *this;
}

StudyEventRecord& StudyEventRecord::add(std::string key, float value) {
    fields_.push_back(StudyEventField{std::move(key), static_cast<double>(value)});
    return *this;
}

StudyEventRecord& StudyEventRecord::add(std::string key, bool value) {
    fields_.push_back(StudyEventField{std::move(key), value});
    return *this;
}

JsonlStudyEventWriter::JsonlStudyEventWriter(const std::filesystem::path& path) {
    if (!path.empty()) {
        open(path);
    }
}

bool JsonlStudyEventWriter::open(const std::filesystem::path& path) {
    path_ = path;
    std::error_code ec;
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
    }
    file_.open(path_, std::ios::out | std::ios::app);
    return file_.is_open();
}

bool JsonlStudyEventWriter::isOpen() const {
    return file_.is_open();
}

const std::filesystem::path& JsonlStudyEventWriter::path() const {
    return path_;
}

bool JsonlStudyEventWriter::write(const StudyEventRecord& record) {
    if (!file_.is_open()) {
        return false;
    }
    file_ << serializeStudyEventRecord(record) << '\n';
    file_.flush();
    return static_cast<bool>(file_);
}

std::string sanitizeRunId(std::string value) {
    if (value.empty()) {
        return "run";
    }

    for (char& ch : value) {
        const bool alphaNum = (ch >= 'a' && ch <= 'z') ||
                              (ch >= 'A' && ch <= 'Z') ||
                              (ch >= '0' && ch <= '9');
        if (!alphaNum && ch != '-' && ch != '_') {
            ch = '_';
        }
    }
    return value;
}

std::string currentLocalDateStamp() {
    return formatLocalTime("%Y-%m-%d");
}

std::string currentLocalTimestampStamp() {
    return formatLocalTime("%Y%m%d_%H%M%S");
}

std::filesystem::path defaultStudyEventExportRoot() {
    return app::applicationRootDirectory() / "logexports";
}

std::filesystem::path defaultStudyEventDateDirectory() {
    return defaultStudyEventExportRoot() / currentLocalDateStamp();
}

std::filesystem::path defaultStudyEventRunDirectory(const std::string& runId) {
    return defaultStudyEventDateDirectory() / sanitizeRunId(runId);
}

std::string serializeStudyEventRecord(const StudyEventRecord& record) {
    std::ostringstream stream;
    stream << '{';
    const auto& fields = record.fields();
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index > 0u) {
            stream << ',';
        }
        stream << '"' << escapeJsonString(fields[index].key) << "\":";
        appendJsonValue(stream, fields[index].value);
    }
    stream << '}';
    return stream.str();
}

}  // namespace telemetry
