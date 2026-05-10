#include "client/PerceptionEventMonitor.hpp"
#include "telemetry/StudyEventLog.hpp"

#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::filesystem::current_path(original_);
    }

private:
    std::filesystem::path original_;
};

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name) {
#ifdef _WIN32
        char* existing = nullptr;
        std::size_t existingLength = 0u;
        if (_dupenv_s(&existing, &existingLength, name_.c_str()) == 0 && existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
            std::free(existing);
        }
#else
        const char* existing = std::getenv(name_.c_str());
        if (existing != nullptr) {
            hadOriginal_ = true;
            originalValue_ = existing;
        }
#endif
        set(value);
    }

    ~ScopedEnvVar() {
        if (hadOriginal_) {
            set(originalValue_);
        } else {
#ifdef _WIN32
            _putenv_s(name_.c_str(), "");
#else
            unsetenv(name_.c_str());
#endif
        }
    }

private:
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    std::string name_;
    bool hadOriginal_{false};
    std::string originalValue_{};
};

void testDefaultRunDirectoryUsesDateSortedExportRoot() {
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "netcodesim-study-event-root";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot / "repo");
    ScopedEnvVar scopedDataRoot("NETCODESIM_DATA_ROOT", (tempRoot / "repo").string());
    ScopedCurrentPath scopedCurrentPath(tempRoot / "repo");

    const std::filesystem::path expected =
        std::filesystem::weakly_canonical(tempRoot / "repo" / "logexports" /
                                          telemetry::currentLocalDateStamp() / "run_1");
    expect(std::filesystem::weakly_canonical(telemetry::defaultStudyEventRunDirectory("run:1")) ==
               expected,
           "study event logs should default under the date-sorted export root");
}

void testJsonlWriterSerializesFlatRecords() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "netcodesim_study_event_test.jsonl";
    std::filesystem::remove(path);

    {
        telemetry::JsonlStudyEventWriter writer(path);
        expect(writer.isOpen(), "study event writer should open the target path");

        telemetry::StudyEventRecord record;
        record.add("event_name", "combat.fire_pressed")
              .add("source", "client")
              .add("command_seq", std::uint32_t{42u})
              .add("shot_hit", false)
              .add("player_name", "quote\"slash\\newline\n");
        expect(writer.write(record), "study event writer should write records");
    }

    const std::string contents = readFile(path);
    expect(contents.find("\"event_name\":\"combat.fire_pressed\"") != std::string::npos,
           "JSONL record should contain the event name as a flat field");
    expect(contents.find("\"command_seq\":42") != std::string::npos,
           "JSONL record should keep numeric fields numeric");
    expect(contents.find("\"shot_hit\":false") != std::string::npos,
           "JSONL record should keep booleans boolean");
    expect(contents.find("quote\\\"slash\\\\newline\\n") != std::string::npos,
           "JSONL record should escape strings for Python JSON readers");
    expect(!contents.empty() && contents.back() == '\n',
           "JSONL records should be newline-delimited");

    std::filesystem::remove(path);
}

void testFovMonitorEmitsOnlyVisibilityTransitions() {
    client::PerceptionEventMonitor monitor;
    client::PerceptionFrame frame;
    frame.perspectiveActorId = 1;
    frame.observer.playerId = 1;
    frame.observer.position = {0.0f, frame.simConfig.playerEyeHeight, 0.0f};
    frame.observer.yaw = 0.0f;
    frame.observer.pitch = 0.0f;
    frame.config.verticalFovDegrees = 70.0f;
    frame.config.aspectRatio = 16.0f / 9.0f;
    frame.config.rangeMeters = 16.0f;

    sim::PlayerState subject;
    subject.playerId = 2;
    subject.position = {0.0f, frame.simConfig.playerEyeHeight, -5.0f};
    frame.subjects = {subject};

    std::vector<client::FovVisibilityTransition> transitions = monitor.update(frame);
    expect(transitions.size() == 1u &&
               transitions.front().kind == client::FovTransitionKind::Entered &&
               transitions.front().subjectActorId == 2,
           "visible subject ahead should emit one fov.entered transition");

    transitions = monitor.update(frame);
    expect(transitions.empty(), "unchanged visibility should not emit duplicate events");

    frame.subjects.front().position.x = 20.0f;
    transitions = monitor.update(frame);
    expect(transitions.size() == 1u &&
               transitions.front().kind == client::FovTransitionKind::Exited,
           "subject leaving the cone should emit one fov.exited transition");
}

void testFovMonitorHonorsLineOfSight() {
    client::PerceptionFrame frame;
    frame.perspectiveActorId = 1;
    frame.observer.playerId = 1;
    frame.observer.position = {0.0f, frame.simConfig.playerEyeHeight, 0.0f};
    frame.observer.yaw = 0.0f;
    frame.observer.pitch = 0.0f;
    frame.config.verticalFovDegrees = 70.0f;
    frame.config.aspectRatio = 16.0f / 9.0f;
    frame.config.rangeMeters = 16.0f;
    frame.config.requireLineOfSight = true;
    frame.environment.collisionBoxes.push_back(
        sim::CollisionBox{{0.0f, 0.9f, -2.5f}, {0.5f, 1.0f, 0.2f}});

    sim::PlayerState subject;
    subject.playerId = 2;
    subject.position = {0.0f, frame.simConfig.playerEyeHeight, -5.0f};

    const client::FovVisibilitySample sample = client::FovVisibilityModel::evaluate(
        client::FovVisibilityQuery{
            frame.observer,
            subject,
            frame.environment,
            frame.simConfig,
            frame.config
        });
    expect(sample.insideCone, "blocked subject should still be geometrically inside the cone");
    expect(!sample.lineOfSight && !sample.visible,
           "blocked subject should not count as visible for FOV transitions");
}

}  // namespace

int main() {
    try {
        testDefaultRunDirectoryUsesDateSortedExportRoot();
        testJsonlWriterSerializesFlatRecords();
        testFovMonitorEmitsOnlyVisibilityTransitions();
        testFovMonitorHonorsLineOfSight();
    } catch (const std::exception& ex) {
        std::cerr << "StudyEventLogTests failed: " << ex.what() << '\n';
        return 1;
    }

    std::cout << "StudyEventLogTests passed\n";
    return 0;
}
