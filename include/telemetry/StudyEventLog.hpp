#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace telemetry {

using StudyEventValue = std::variant<std::string, std::int64_t, std::uint64_t, double, bool>;

struct StudyEventField {
    std::string key{};
    StudyEventValue value{};
};

class StudyEventRecord {
public:
    StudyEventRecord& add(std::string key, std::string value);
    StudyEventRecord& add(std::string key, const char* value);
    StudyEventRecord& add(std::string key, std::int32_t value);
    StudyEventRecord& add(std::string key, std::uint32_t value);
    StudyEventRecord& add(std::string key, std::int64_t value);
    StudyEventRecord& add(std::string key, std::uint64_t value);
    StudyEventRecord& add(std::string key, double value);
    StudyEventRecord& add(std::string key, float value);
    StudyEventRecord& add(std::string key, bool value);

    const std::vector<StudyEventField>& fields() const {
        return fields_;
    }

private:
    std::vector<StudyEventField> fields_{};
};

class StudyEventSink {
public:
    virtual ~StudyEventSink() = default;
    virtual bool write(const StudyEventRecord& record) = 0;
};

class JsonlStudyEventWriter final : public StudyEventSink {
public:
    explicit JsonlStudyEventWriter(const std::filesystem::path& path = {});

    bool open(const std::filesystem::path& path);
    bool isOpen() const;
    const std::filesystem::path& path() const;

    bool write(const StudyEventRecord& record) override;

private:
    std::filesystem::path path_{};
    std::ofstream file_{};
};

std::string sanitizeRunId(std::string value);
std::string currentLocalDateStamp();
std::string currentLocalTimestampStamp();
std::filesystem::path defaultStudyEventExportRoot();
std::filesystem::path defaultStudyEventDateDirectory();
std::filesystem::path defaultStudyEventRunDirectory(const std::string& runId);
std::string serializeStudyEventRecord(const StudyEventRecord& record);

}  // namespace telemetry
